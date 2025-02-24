// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/tserver/pg_client_service.h"

#include <mutex>
#include <queue>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index_container.hpp>

#include "yb/client/client.h"
#include "yb/client/meta_cache.h"
#include "yb/client/schema.h"
#include "yb/client/table.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_info.h"
#include "yb/client/tablet_server.h"

#include "yb/dockv/partition.h"
#include "yb/common/pg_types.h"
#include "yb/common/wire_protocol.h"

#include "yb/master/master_admin.proxy.h"
#include "yb/master/master_heartbeat.pb.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_context.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/scheduler.h"
#include "yb/rpc/tasks_pool.h"

#include "yb/tserver/pg_client_session.h"
#include "yb/tserver/pg_create_table.h"
#include "yb/tserver/pg_response_cache.h"
#include "yb/tserver/pg_sequence_cache.h"
#include "yb/tserver/pg_table_cache.h"
#include "yb/tserver/tablet_server_interface.h"
#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/net/net_util.h"
#include "yb/util/random_util.h"
#include "yb/util/result.h"
#include "yb/util/shared_lock.h"
#include "yb/util/size_literals.h"
#include "yb/util/status.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/thread.h"

using namespace std::literals;

DEFINE_UNKNOWN_uint64(pg_client_session_expiration_ms, 60000,
                      "Pg client session expiration time in milliseconds.");

DEFINE_RUNTIME_bool(pg_client_use_shared_memory, false,
                    "Use shared memory for executing read and write pg client queries");

DEFINE_test_flag(uint64, delay_before_get_old_transactions_heartbeat_intervals, 0,
                 "When non-zero, we sleep for set transaction heartbeat interval periods before "
                 "fetching old transactions. This delay is implemented to ensure that the "
                 "information returned for yb_lock_status is more up-to-date. Currently, the flag "
                 "is used in tests alone.");

DECLARE_uint64(transaction_heartbeat_usec);

namespace yb {
namespace tserver {

namespace {

template <class Resp>
void Respond(const Status& status, Resp* resp, rpc::RpcContext* context) {
  if (!status.ok()) {
    StatusToPB(status, resp->mutable_status());
  }
  context->RespondSuccess();
}

class PgClientSessionLocker;

class LockablePgClientSession : public PgClientSession {
 public:
  template <class... Args>
  explicit LockablePgClientSession(CoarseDuration lifetime, Args&&... args)
      : PgClientSession(std::forward<Args>(args)...),
        lifetime_(lifetime), expiration_(NewExpiration()) {
  }

  void StartExchange(const Uuid& instance_id) {
    exchange_.emplace(instance_id, id(), Create::kTrue, [this](size_t size) {
      Touch();
      std::shared_ptr<CountDownLatch> latch;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        latch = ProcessSharedRequest(size, &exchange_->exchange());
      }
      if (latch) {
        latch->Wait();
      }
    });
  }

  CoarseTimePoint expiration() const {
    return expiration_.load(std::memory_order_acquire);
  }

  void Touch() {
    auto new_expiration = NewExpiration();
    auto old_expiration = expiration_.load(std::memory_order_acquire);
    while (new_expiration > old_expiration) {
      if (expiration_.compare_exchange_weak(
          old_expiration, new_expiration, std::memory_order_acq_rel)) {
        break;
      }
    }
  }

 private:
  friend class PgClientSessionLocker;

  CoarseTimePoint NewExpiration() const {
    return CoarseMonoClock::now() + lifetime_;
  }

  std::mutex mutex_;
  std::optional<SharedExchangeThread> exchange_;
  const CoarseDuration lifetime_;
  std::atomic<CoarseTimePoint> expiration_;
};

class PgClientSessionLocker {
 public:
  using LockablePtr = std::shared_ptr<LockablePgClientSession>;

  explicit PgClientSessionLocker(const LockablePtr& lockable)
      : lockable_(lockable), lock_(lockable->mutex_) {
  }

  PgClientSession& operator*() const {
    return *lockable_;
  }

  PgClientSession* operator->() const {
    return lockable_.get();
  }

 private:
  LockablePtr lockable_;
  std::unique_lock<std::mutex> lock_;
};

using LockablePgClientSessionPtr = std::shared_ptr<LockablePgClientSession>;
using OldTxnsRespPtr = std::shared_ptr<tserver::GetOldTransactionsResponsePB>;
using RemoteTabletServerPtr = std::shared_ptr<client::internal::RemoteTabletServer>;
using OldTransactionMetadataPB = tserver::GetOldTransactionsResponsePB::OldTransactionMetadataPB;
using OldTransactionMetadataPBPtr = std::shared_ptr<OldTransactionMetadataPB>;
using client::internal::RemoteTabletPtr;

void GetTablePartitionList(const client::YBTablePtr& table, PgTablePartitionsPB* partition_list) {
  const auto table_partition_list = table->GetVersionedPartitions();
  const auto& partition_keys = partition_list->mutable_keys();
  partition_keys->Clear();
  partition_keys->Reserve(narrow_cast<int>(table_partition_list->keys.size()));
  for (const auto& key : table_partition_list->keys) {
    *partition_keys->Add() = key;
  }
  partition_list->set_version(table_partition_list->version);
}

} // namespace

template <class Extractor>
class ApplyToValue {
 public:
  using result_type = typename Extractor::result_type;

  template <class T>
  auto operator()(const T& t) const {
    return extractor_(t.value());
  }

 private:
  Extractor extractor_;
};

class PgClientServiceImpl::Impl {
 public:
  explicit Impl(
      std::reference_wrapper<const TabletServerIf> tablet_server,
      const std::shared_future<client::YBClient*>& client_future,
      const scoped_refptr<ClockBase>& clock,
      TransactionPoolProvider transaction_pool_provider,
      rpc::Scheduler* scheduler,
      const std::optional<XClusterContext>& xcluster_context,
      PgMutationCounter* pg_node_level_mutation_counter,
      MetricEntity* metric_entity,
      const std::shared_ptr<MemTracker>& parent_mem_tracker)
      : tablet_server_(tablet_server.get()),
        client_future_(client_future),
        clock_(clock),
        transaction_pool_provider_(std::move(transaction_pool_provider)),
        table_cache_(client_future),
        check_expired_sessions_(scheduler),
        xcluster_context_(xcluster_context),
        pg_node_level_mutation_counter_(pg_node_level_mutation_counter),
        response_cache_(parent_mem_tracker, metric_entity),
        instance_id_(Uuid::Generate()) {
    ScheduleCheckExpiredSessions(CoarseMonoClock::now());
  }

  ~Impl() {
    check_expired_sessions_.Shutdown();
  }

  Status Heartbeat(
      const PgHeartbeatRequestPB& req, PgHeartbeatResponsePB* resp, rpc::RpcContext* context) {
    if (req.session_id() == std::numeric_limits<uint64_t>::max()) {
      return Status::OK();
    }

    if (req.session_id()) {
      return ResultToStatus(DoGetSession(req.session_id()));
    }

    auto session_id = ++session_serial_no_;
    auto session = std::make_shared<LockablePgClientSession>(
        FLAGS_pg_client_session_expiration_ms * 1ms, session_id, &client(), clock_,
        transaction_pool_provider_, &table_cache_, xcluster_context_,
        pg_node_level_mutation_counter_, &response_cache_, &sequence_cache_);
    resp->set_session_id(session_id);
    if (FLAGS_pg_client_use_shared_memory) {
      resp->set_instance_id(instance_id_.data(), instance_id_.size());
      session->StartExchange(instance_id_);
    }

    std::lock_guard lock(mutex_);
    auto it = sessions_.insert(std::move(session)).first;
    session_expiration_queue_.push({(**it).expiration(), session_id});
    return Status::OK();
  }

  Status OpenTable(
      const PgOpenTableRequestPB& req, PgOpenTableResponsePB* resp, rpc::RpcContext* context) {
    if (req.invalidate_cache_time_us()) {
      table_cache_.InvalidateAll(CoarseTimePoint() + req.invalidate_cache_time_us() * 1us);
    }
    if (req.reopen()) {
      table_cache_.Invalidate(req.table_id());
    }

    client::YBTablePtr table;
    RETURN_NOT_OK(table_cache_.GetInfo(req.table_id(), &table, resp->mutable_info()));
    tserver::GetTablePartitionList(table, resp->mutable_partitions());
    return Status::OK();
  }

  Status GetTablePartitionList(
      const PgGetTablePartitionListRequestPB& req,
      PgGetTablePartitionListResponsePB* resp,
      rpc::RpcContext* context) {
    const auto table = VERIFY_RESULT(table_cache_.Get(req.table_id()));
    tserver::GetTablePartitionList(table, resp->mutable_partitions());
    return Status::OK();
  }

  Status GetDatabaseInfo(
      const PgGetDatabaseInfoRequestPB& req, PgGetDatabaseInfoResponsePB* resp,
      rpc::RpcContext* context) {
    return client().GetNamespaceInfo(
        GetPgsqlNamespaceId(req.oid()), "" /* namespace_name */, YQL_DATABASE_PGSQL,
        resp->mutable_info());
  }

  Status IsInitDbDone(
      const PgIsInitDbDoneRequestPB& req, PgIsInitDbDoneResponsePB* resp,
      rpc::RpcContext* context) {
    HostPort master_leader_host_port = client().GetMasterLeaderAddress();
    auto proxy = std::make_shared<master::MasterAdminProxy>(
        &client().proxy_cache(), master_leader_host_port);
    rpc::RpcController rpc;
    master::IsInitDbDoneRequestPB master_req;
    master::IsInitDbDoneResponsePB master_resp;
    RETURN_NOT_OK(proxy->IsInitDbDone(master_req, &master_resp, &rpc));
    if (master_resp.has_error()) {
      return STATUS_FORMAT(
          RuntimeError,
          "IsInitDbDone RPC response hit error: $0",
          master_resp.error().ShortDebugString());
    }
    if (master_resp.done() && master_resp.has_initdb_error() &&
        !master_resp.initdb_error().empty()) {
      return STATUS_FORMAT(RuntimeError, "initdb failed: $0", master_resp.initdb_error());
    }
    VLOG(1) << "IsInitDbDone response: " << master_resp.ShortDebugString();
    // We return true if initdb finished running, as well as if we know that it created the first
    // table (pg_proc) to make initdb idempotent on upgrades.
    resp->set_done(master_resp.done() || master_resp.pg_proc_exists());
    return Status::OK();
  }

  Status ReserveOids(
      const PgReserveOidsRequestPB& req, PgReserveOidsResponsePB* resp, rpc::RpcContext* context) {
    uint32_t begin_oid, end_oid;
    RETURN_NOT_OK(client().ReservePgsqlOids(
        GetPgsqlNamespaceId(req.database_oid()), req.next_oid(), req.count(), &begin_oid,
        &end_oid));
    resp->set_begin_oid(begin_oid);
    resp->set_end_oid(end_oid);

    return Status::OK();
  }

  Status GetCatalogMasterVersion(
      const PgGetCatalogMasterVersionRequestPB& req,
      PgGetCatalogMasterVersionResponsePB* resp,
      rpc::RpcContext* context) {
    uint64_t version;
    RETURN_NOT_OK(client().GetYsqlCatalogMasterVersion(&version));
    resp->set_version(version);
    return Status::OK();
  }

  Status CreateSequencesDataTable(
      const PgCreateSequencesDataTableRequestPB& req,
      PgCreateSequencesDataTableResponsePB* resp,
      rpc::RpcContext* context) {
    return tserver::CreateSequencesDataTable(&client(), context->GetClientDeadline());
  }

  std::future<Result<std::pair<TabletId, OldTxnsRespPtr>>> DoGetOldTransactionsForTablet(
      const TabletId& tablet_id, const uint32_t min_txn_age_ms, const uint32_t max_num_txns,
      const std::shared_ptr<TabletServerServiceProxy>& proxy) {
    auto req = std::make_shared<tserver::GetOldTransactionsRequestPB>();
    req->set_tablet_id(tablet_id);
    req->set_min_txn_age_ms(min_txn_age_ms);
    req->set_max_num_txns(max_num_txns);

    return MakeFuture<Result<std::pair<TabletId, OldTxnsRespPtr>>>([&](auto callback) {
      auto resp = std::make_shared<GetOldTransactionsResponsePB>();
      std::shared_ptr<rpc::RpcController> controller = std::make_shared<rpc::RpcController>();
      proxy->GetOldTransactionsAsync(
          *req.get(), resp.get(), controller.get(),
          [req, callback, controller, resp] {
        auto s = controller->status();
        if (!s.ok()) {
          s = s.CloneAndPrepend(
              Format("GetOldTransactions request for tablet $0 failed: ", req->tablet_id()));
          return callback(s);
        }
        callback(std::make_pair(req->tablet_id(), std::move(resp)));
      });
    });
  }

  struct OldTransactionComparator {
    bool operator()(
        const OldTransactionMetadataPBPtr lhs, const OldTransactionMetadataPBPtr rhs) const {
      // Order is reversed so that we get transactions with earlier start times first.
      if (lhs->start_time() != rhs->start_time()) {
        return lhs->start_time() > rhs->start_time();
      }
      return lhs->transaction_id() > rhs->transaction_id();
    }
  };

  Status GetLockStatus(
      const PgGetLockStatusRequestPB& req, PgGetLockStatusResponsePB* resp,
      rpc::RpcContext* context) {
    std::vector<master::TSInformationPB> live_tservers;
    RETURN_NOT_OK(tablet_server_.GetLiveTServers(&live_tservers));
    GetLockStatusRequestPB lock_status_req;
    if (!req.transaction_id().empty()) {
      // TODO(pglocks): Forward the request to tservers hosting the involved tablets of the txn,
      // as opposed to broadcasting the request to all live tservers.
      // https://github.com/yugabyte/yugabyte-db/issues/17886.
      //
      // GetLockStatusRequestPB supports providing multiple transaction ids, but postgres sends
      // only one transaction id in PgGetLockStatusRequestPB for now.
      // TODO(pglocks): Once we call GetTransactionStatus for involved tablets, ensure we populate
      // aborted_subtxn_set in the GetLockStatusRequests that we send to involved tablets as well.
      lock_status_req.add_transaction_ids(req.transaction_id());
      return DoGetLockStatus(lock_status_req, resp, context, live_tservers);
    }
    const auto& min_txn_age_ms = req.min_txn_age_ms();
    const auto& max_num_txns = req.max_num_txns();
    RSTATUS_DCHECK(max_num_txns > 0, InvalidArgument,
                   "Request must contain max_num_txns > 0, got $0", max_num_txns);
    // Sleep before fetching old transactions and their involved tablets. This is necessary for
    // yb_lock_status tests that expect to see complete lock info of respective transaction(s).
    // Else, the coordinator might not return updated involved tablet(s) and we could end up
    // returning incomplete lock info for a given transaction.
    if (PREDICT_FALSE(FLAGS_TEST_delay_before_get_old_transactions_heartbeat_intervals > 0)) {
      auto delay_usec = FLAGS_TEST_delay_before_get_old_transactions_heartbeat_intervals
                        * FLAGS_transaction_heartbeat_usec;
      SleepFor(MonoDelta::FromMicroseconds(delay_usec));
    }

    std::vector<std::future<Result<std::pair<TabletId, OldTxnsRespPtr>>>> res_futures;
    std::unordered_set<TabletId> status_tablet_ids;
    for (const auto& live_ts : live_tservers) {
      const auto& permanent_uuid = live_ts.tserver_instance().permanent_uuid();
      auto remote_tserver = VERIFY_RESULT(client().GetRemoteTabletServer(permanent_uuid));
      auto txn_status_tablets = VERIFY_RESULT(
            client().GetTransactionStatusTablets(remote_tserver->cloud_info_pb()));

      auto proxy = remote_tserver->proxy();
      for (const auto& tablet : txn_status_tablets.global_tablets) {
        res_futures.push_back(
            DoGetOldTransactionsForTablet(tablet, min_txn_age_ms, max_num_txns, proxy));
        status_tablet_ids.insert(tablet);
      }
      for (const auto& tablet : txn_status_tablets.placement_local_tablets) {
        res_futures.push_back(
            DoGetOldTransactionsForTablet(tablet, min_txn_age_ms, max_num_txns, proxy));
        status_tablet_ids.insert(tablet);
      }
    }
    // Limit num transactions to max_num_txns for which lock status is being queried.
    //
    // TODO(pglocks): We could end up storing duplicate records for the same transaction in the
    // priority queue, and end up reporting locks of #transaction < max_num_txns. This will be
    // fixed once https://github.com/yugabyte/yugabyte-db/issues/18140 is addressed.
    std::priority_queue<OldTransactionMetadataPBPtr,
                        std::vector<OldTransactionMetadataPBPtr>,
                        OldTransactionComparator> old_txns_pq;
    for (auto it = res_futures.begin(); it != res_futures.end(); ) {
      auto res = it->get();
      if (!res.ok()) {
        return res.status();
      }

      auto& [status_tablet_id, old_txns_resp] = *res;
      if (old_txns_resp->has_error()) {
        // Ignore leadership errors as we broadcast the request to all tservers.
        if (old_txns_resp->error().code() == TabletServerErrorPB::NOT_THE_LEADER) {
          it = res_futures.erase(it);
          continue;
        }
        const auto& s = StatusFromPB(old_txns_resp->error().status());
        StatusToPB(s, resp->mutable_status());
        return Status::OK();
      }

      status_tablet_ids.erase(status_tablet_id);
      for (auto& old_txn : old_txns_resp->txn()) {
        auto old_txn_ptr = std::make_shared<OldTransactionMetadataPB>(std::move(old_txn));
        old_txns_pq.push(std::move(old_txn_ptr));
        while (old_txns_pq.size() > max_num_txns) {
          old_txns_pq.pop();
        }
      }
      it++;
    }
    // Set status and return if we don't get a valid resp for all status tablets at least once.
    // It's ok if we get more than one resp for a status tablet, as we accumulate received
    // transactions and their involved tablets.
    if(!status_tablet_ids.empty()) {
      StatusToPB(
          STATUS_FORMAT(IllegalState,
                        "Couldn't fetch old transactions for the following status tablets: $0",
                        status_tablet_ids),
          resp->mutable_status());
      return Status::OK();
    }

    while (!old_txns_pq.empty()) {
      auto& old_txn = old_txns_pq.top();
      const auto& txn_id = old_txn->transaction_id();
      auto& node_entry = (*resp->mutable_transactions_by_node())[old_txn->host_node_uuid()];
      node_entry.add_transaction_ids(txn_id);
      for (const auto& tablet_id : old_txn->tablets()) {
        auto& tablet_entry = (*lock_status_req.mutable_transactions_by_tablet())[tablet_id];
        auto* transaction = tablet_entry.add_transactions();
        transaction->set_id(txn_id);
        transaction->mutable_aborted()->Swap(old_txn->mutable_aborted_subtxn_set());
      }
      old_txns_pq.pop();
    }
    return DoGetLockStatus(lock_status_req, resp, context, live_tservers);
  }

  Status DoGetLockStatus(
      const GetLockStatusRequestPB& req, PgGetLockStatusResponsePB* resp,
      rpc::RpcContext* context, const std::vector<master::TSInformationPB>& live_tservers) {
    if (req.transactions_by_tablet().empty() && req.transaction_ids().empty()) {
      return Status::OK();
    }
    // TODO(pglocks): parallelize RPCs
    rpc::RpcController controller;
    for (const auto& live_ts : live_tservers) {
      const auto& permanent_uuid = live_ts.tserver_instance().permanent_uuid();
      auto remote_tserver = VERIFY_RESULT(client().GetRemoteTabletServer(permanent_uuid));
      auto proxy = remote_tserver->proxy();
      GetLockStatusResponsePB node_resp;
      controller.Reset();
      auto s = proxy->GetLockStatus(req, &node_resp, &controller);
      if (!s.ok()) {
        resp->Clear();
        return s;
      }
      if (node_resp.has_error()) {
        resp->Clear();
        *resp->mutable_status() = node_resp.error().status();
        return Status::OK();
      }
      auto* node_locks = resp->add_node_locks();
      node_locks->set_permanent_uuid(permanent_uuid);
      node_locks->mutable_tablet_lock_infos()->Swap(node_resp.mutable_tablet_lock_infos());
    }

    auto s = RefineAccumulatedLockStatusResp(req, resp);
    if (!s.ok()) {
      s = s.CloneAndPrepend("Error refining accumulated LockStatus responses.");
      resp->Clear();
    }
    StatusToPB(s, resp->mutable_status());
    return Status::OK();
  }

  // Refines PgGetLockStatusResponsePB by dropping duplicate lock responses for a given tablet.
  // Also ensure that we see a lock status response from all involved tablets of each txn since
  // returning incomplete/impartial results might be misleading.
  Status RefineAccumulatedLockStatusResp(
      const GetLockStatusRequestPB& req, PgGetLockStatusResponsePB* resp) {
    std::unordered_map<std::string, std::unordered_set<TabletId>> txn_involved_tablets;
    for (const auto& [tablet_id, involved_txns] : req.transactions_by_tablet()) {
      for (const auto& txn : involved_txns.transactions()) {
        auto txn_id = VERIFY_RESULT(FullyDecodeTransactionId(txn.id()));
        txn_involved_tablets[txn_id.ToString()].insert(tablet_id);
      }
    }

    // Track the highest seen term for each tablet id.
    std::map<TabletId, uint64_t> peer_term;
    for (const auto& node_lock : resp->node_locks()) {
      for (const auto& tablet_lock_info : node_lock.tablet_lock_infos()) {
        const auto& tablet_id = tablet_lock_info.tablet_id();
        peer_term[tablet_id] = std::max(peer_term[tablet_id], tablet_lock_info.term());
      }
    }

    std::unordered_set<TabletId> tablets;
    std::set<TransactionId> seen_transactions;
    for (auto& node_lock : *resp->mutable_node_locks()) {
      auto* tablet_lock_infos = node_lock.mutable_tablet_lock_infos();
      for (auto lock_it = tablet_lock_infos->begin(); lock_it != tablet_lock_infos->end();) {
        const auto& tablet_id = lock_it->tablet_id();
        auto max_term_for_tablet = peer_term[tablet_id];
        if (lock_it->term() < max_term_for_tablet) {
          LOG(INFO) << "Dropping lock info from stale peer of tablet " << lock_it->tablet_id()
                    << " from node " << node_lock.permanent_uuid()
                    << " with term " << lock_it->term()
                    << " less than highest term seen " << max_term_for_tablet
                    << ". This should be rare but is not an error otherwise.";
          lock_it = node_lock.mutable_tablet_lock_infos()->erase(lock_it);
          continue;
        }
        RSTATUS_DCHECK(
            tablets.emplace(tablet_id).second, IllegalState,
            "Found tablet $0 more than once in PgGetLockStatusResponsePB", tablet_id);
        for (auto& [txn, _] : lock_it->transaction_locks()) {
          seen_transactions.insert(VERIFY_RESULT(TransactionId::FromString(txn)));
        }
        lock_it++;
      }
    }

    for (const auto& [txn_id, involved_tablets] : txn_involved_tablets) {
      for (const auto& tablet : involved_tablets) {
        auto it = tablets.find(tablet);
        RSTATUS_DCHECK(
            it != tablets.end(), IllegalState,
            "Expected to see transaction's $0 involved tablet $1 in PgGetLockStatusResponsePB",
            txn_id, tablet);
      }
    }
    // Ensure that the response contains host node uuid for all involved transactions.
    for (const auto& [_, txn_list] : resp->transactions_by_node()) {
      for (const auto& txn : txn_list.transaction_ids()) {
        seen_transactions.erase(VERIFY_RESULT(FullyDecodeTransactionId(txn)));
      }
    }
    // TODO(pglocks): We currently don't populate transaction's host node info when the incoming
    // PgGetLockStatusRequestPB has transaction_id field set. This shouldn't be the case once
    // https://github.com/yugabyte/yugabyte-db/issues/16913 is addressed. As part of the fix,
    // remove !req.transaction_ids().empty() in the below check.
    RSTATUS_DCHECK(seen_transactions.empty() || !req.transaction_ids().empty(), IllegalState,
           "Host node uuid not set for all involved transactions");
    return Status::OK();
  }

  Status TabletServerCount(
      const PgTabletServerCountRequestPB& req, PgTabletServerCountResponsePB* resp,
      rpc::RpcContext* context) {
    int result = 0;
    RETURN_NOT_OK(client().TabletServerCount(&result, req.primary_only(), /* use_cache= */ true));
    resp->set_count(result);
    return Status::OK();
  }

  Status ListLiveTabletServers(
      const PgListLiveTabletServersRequestPB& req, PgListLiveTabletServersResponsePB* resp,
      rpc::RpcContext* context) {
    auto tablet_servers = VERIFY_RESULT(client().ListLiveTabletServers(req.primary_only()));
    for (const auto& server : tablet_servers) {
      server.ToPB(resp->mutable_servers()->Add());
    }
    return Status::OK();
  }

  Status GetIndexBackfillProgress(
      const PgGetIndexBackfillProgressRequestPB& req, PgGetIndexBackfillProgressResponsePB* resp,
      rpc::RpcContext* context) {
    std::vector<TableId> index_ids;
    for (const auto& index_id : req.index_ids()) {
      index_ids.emplace_back(PgObjectId::GetYbTableIdFromPB(index_id));
    }
    return client().GetIndexBackfillProgress(index_ids, resp->mutable_rows_processed_entries());
  }

  Status ValidatePlacement(
      const PgValidatePlacementRequestPB& req, PgValidatePlacementResponsePB* resp,
      rpc::RpcContext* context) {
    master::ReplicationInfoPB replication_info;
    master::PlacementInfoPB* live_replicas = replication_info.mutable_live_replicas();

    for (const auto& block : req.placement_infos()) {
      auto pb = live_replicas->add_placement_blocks();
      pb->mutable_cloud_info()->set_placement_cloud(block.cloud());
      pb->mutable_cloud_info()->set_placement_region(block.region());
      pb->mutable_cloud_info()->set_placement_zone(block.zone());
      pb->set_min_num_replicas(block.min_num_replicas());

      if (block.leader_preference() < 0) {
        return STATUS(InvalidArgument, "leader_preference cannot be negative");
      } else if (block.leader_preference() > req.placement_infos_size()) {
        return STATUS(
            InvalidArgument,
            "Priority value cannot be more than the number of zones in the preferred list since "
            "each priority should be associated with at least one zone from the list");
      } else if (block.leader_preference() > 0) {
        while (replication_info.multi_affinitized_leaders_size() < block.leader_preference()) {
          replication_info.add_multi_affinitized_leaders();
        }

        auto zone_set =
            replication_info.mutable_multi_affinitized_leaders(block.leader_preference() - 1);
        auto ci = zone_set->add_zones();
        ci->set_placement_cloud(block.cloud());
        ci->set_placement_region(block.region());
        ci->set_placement_zone(block.zone());
      }
    }
    live_replicas->set_num_replicas(req.num_replicas());

    return client().ValidateReplicationInfo(replication_info);
  }

  Status GetTableDiskSize(
      const PgGetTableDiskSizeRequestPB& req, PgGetTableDiskSizeResponsePB* resp,
      rpc::RpcContext* context) {
    auto result =
        client().GetTableDiskSize(PgObjectId::GetYbTableIdFromPB(req.table_id()));
    if (!result.ok()) {
      StatusToPB(result.status(), resp->mutable_status());
    } else {
      resp->set_size(result->table_size);
      resp->set_num_missing_tablets(result->num_missing_tablets);
    }
    return Status::OK();
  }

  Status CheckIfPitrActive(
      const PgCheckIfPitrActiveRequestPB& req, PgCheckIfPitrActiveResponsePB* resp,
      rpc::RpcContext* context) {
    auto res = client().CheckIfPitrActive();
    if (!res.ok()) {
      StatusToPB(res.status(), resp->mutable_status());
    } else {
      resp->set_is_pitr_active(*res);
    }
    return Status::OK();
  }

  Status GetTserverCatalogVersionInfo(
      const PgGetTserverCatalogVersionInfoRequestPB& req,
      PgGetTserverCatalogVersionInfoResponsePB* resp,
      rpc::RpcContext* context) {
    GetTserverCatalogVersionInfoRequestPB request;
    GetTserverCatalogVersionInfoResponsePB info;
    const auto db_oid = req.db_oid();
    request.set_size_only(req.size_only());
    request.set_db_oid(db_oid);
    RETURN_NOT_OK(tablet_server_.get_ysql_db_oid_to_cat_version_info_map(
        request, &info));
    if (req.size_only()) {
      // We only ask for the size of catalog version map in tserver and should not need to
      // populate any entries.
      DCHECK_EQ(info.entries_size(), 0);
      resp->set_num_entries(info.num_entries());
      return Status::OK();
    }
    // If db_oid is kPgInvalidOid, we ask for the catalog version map of all databases.
    // Otherwise, we only ask for the catalog version info for the given database.
    if (db_oid != kPgInvalidOid) {
      // In a race condition it is possible that the database db_oid is already
      // dropped from another node even through we are still connecting to
      // db_oid. When that happens info.entries_size() can be 0.
      DCHECK_LE(info.entries_size(), 1);
    }
    resp->mutable_entries()->Swap(info.mutable_entries());
    return Status::OK();
  }

  Status IsObjectPartOfXRepl(
    const PgIsObjectPartOfXReplRequestPB& req, PgIsObjectPartOfXReplResponsePB* resp,
    rpc::RpcContext* context) {
    auto res = client().IsObjectPartOfXRepl(PgObjectId::GetYbTableIdFromPB(req.table_id()));
    if (!res.ok()) {
      StatusToPB(res.status(), resp->mutable_status());
    } else {
      resp->set_is_object_part_of_xrepl(*res);
    }
    return Status::OK();
  }

  void Perform(PgPerformRequestPB* req, PgPerformResponsePB* resp, rpc::RpcContext* context) {
    auto status = DoPerform(req, resp, context);
    if (!status.ok()) {
      Respond(status, resp, context);
    }
  }

  void InvalidateTableCache() {
    table_cache_.InvalidateAll(CoarseMonoClock::Now());
  }

  // Return the TabletServer hosting the specified status tablet.
  std::future<Result<RemoteTabletServerPtr>> GetTServerHostingStatusTablet(
      const TabletId& status_tablet_id, CoarseTimePoint deadline) {

    return MakeFuture<Result<RemoteTabletServerPtr>>([&](auto callback) {
      client().LookupTabletById(
          status_tablet_id, /* table =*/ nullptr, master::IncludeInactive::kFalse,
          master::IncludeDeleted::kFalse, deadline,
          [&, status_tablet_id, callback] (const auto& lookup_result) {
            if (!lookup_result.ok()) {
              return callback(lookup_result.status());
            }

            auto& remote_tablet = *lookup_result;
            if (!remote_tablet) {
              return callback(STATUS_FORMAT(
                  InvalidArgument,
                  Format("Status tablet with id: $0 not found", status_tablet_id)));
            }

            if (!remote_tablet->LeaderTServer()) {
              return callback(STATUS_FORMAT(
                  TryAgain, Format("Leader not found for tablet $0", status_tablet_id)));
            }
            const auto& permanent_uuid = remote_tablet->LeaderTServer()->permanent_uuid();
            callback(client().GetRemoteTabletServer(permanent_uuid));
          },
          // Force a client cache refresh so as to not hit NOT_LEADER error.
          client::UseCache::kFalse);
    });
  }

  Result<std::vector<RemoteTabletServerPtr>> GetAllLiveTservers() {
    std::vector<RemoteTabletServerPtr> remote_tservers;
    std::vector<master::TSInformationPB> live_tservers;
    RETURN_NOT_OK(tablet_server_.GetLiveTServers(&live_tservers));
    for (const auto& live_ts : live_tservers) {
      const auto& permanent_uuid = live_ts.tserver_instance().permanent_uuid();
      remote_tservers.push_back(VERIFY_RESULT(client().GetRemoteTabletServer(permanent_uuid)));
    }
    return remote_tservers;
  }

  Status CancelTransaction(const PgCancelTransactionRequestPB& req,
                           PgCancelTransactionResponsePB* resp,
                           rpc::RpcContext* context) {
    if (req.transaction_id().empty()) {
      return STATUS_FORMAT(IllegalState,
                           "Transaction Id not provided in PgCancelTransactionRequestPB");
    }
    tserver::CancelTransactionRequestPB node_req;
    node_req.set_transaction_id(req.transaction_id());

    std::vector<RemoteTabletServerPtr> remote_tservers;
    if (req.status_tablet_id().empty()) {
      remote_tservers = VERIFY_RESULT(GetAllLiveTservers());
    } else {
      const auto& remote_ts = VERIFY_RESULT(GetTServerHostingStatusTablet(
          req.status_tablet_id(), context->GetClientDeadline()).get());
      remote_tservers.push_back(remote_ts);
      node_req.set_status_tablet_id(req.status_tablet_id());
    }

    std::vector<std::future<Status>> status_future;
    std::vector<tserver::CancelTransactionResponsePB> node_resp(remote_tservers.size());
    for (size_t i = 0 ; i < remote_tservers.size() ; i++) {
      const auto& proxy = remote_tservers[i]->proxy();
      auto controller = std::make_shared<rpc::RpcController>();
      status_future.push_back(
          MakeFuture<Status>([&, controller](auto callback) {
            proxy->CancelTransactionAsync(
                node_req, &node_resp[i], controller.get(), [callback, controller] {
              callback(controller->status());
            });
          }));
    }

    auto status = STATUS_FORMAT(NotFound, "Transaction not found.");
    resp->Clear();
    for (size_t i = 0 ; i < status_future.size() ; i++) {
      const auto& s = status_future[i].get();
      if (!s.ok()) {
        LOG(WARNING) << "CancelTransaction request to TS failed with status: " << s;
        continue;
      }

      if (node_resp[i].has_error()) {
        // Errors take precedence over TransactionStatus::ABORTED statuses. This needs to be done to
        // correctly handle cancelation requests of promoted txns. Ignore all NotFound statuses as
        // we collate them, collect all other error types.
        const auto& status_from_pb = StatusFromPB(node_resp[i].error().status());
        if (status_from_pb.IsNotFound()) {
          continue;
        }
        status = status_from_pb.CloneAndAppend("\n").CloneAndAppend(status.message());
      }

      // One of the TServers reported successfull cancelation of the transaction. Reset the status
      // if we haven't seen any errors other than NOT_FOUND from the remaining TServers.
      if (status.IsNotFound()) {
        status = Status::OK();
      }
    }

    StatusToPB(status, resp->mutable_status());
    return Status::OK();
  }

  #define PG_CLIENT_SESSION_METHOD_FORWARD(r, data, method) \
  Status method( \
      const BOOST_PP_CAT(BOOST_PP_CAT(Pg, method), RequestPB)& req, \
      BOOST_PP_CAT(BOOST_PP_CAT(Pg, method), ResponsePB)* resp, \
      rpc::RpcContext* context) { \
    return VERIFY_RESULT(GetSession(req))->method(req, resp, context); \
  }

  BOOST_PP_SEQ_FOR_EACH(PG_CLIENT_SESSION_METHOD_FORWARD, ~, PG_CLIENT_SESSION_METHODS);

  size_t TEST_SessionsCount() {
    SharedLock lock(mutex_);
    return sessions_.size();
  }

 private:
  client::YBClient& client() { return *client_future_.get(); }

  template <class Req>
  Result<PgClientSessionLocker> GetSession(const Req& req) {
    return GetSession(req.session_id());
  }

  Result<LockablePgClientSessionPtr> DoGetSession(uint64_t session_id) {
    SharedLock<rw_spinlock> lock(mutex_);
    DCHECK_NE(session_id, 0);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return STATUS_FORMAT(InvalidArgument, "Unknown session: $0", session_id);
    }
    (**it).Touch();
    return *it;
  }

  Result<PgClientSessionLocker> GetSession(uint64_t session_id) {
    return PgClientSessionLocker(VERIFY_RESULT(DoGetSession(session_id)));
  }

  void ScheduleCheckExpiredSessions(CoarseTimePoint now) REQUIRES(mutex_) {
    auto time = session_expiration_queue_.empty()
        ? CoarseTimePoint(now + FLAGS_pg_client_session_expiration_ms * 1ms)
        : session_expiration_queue_.top().first + 1s;
    check_expired_sessions_.Schedule([this](const Status& status) {
      if (!status.ok()) {
        return;
      }
      this->CheckExpiredSessions();
    }, time - now);
  }

  void CheckExpiredSessions() {
    auto now = CoarseMonoClock::now();
    std::lock_guard lock(mutex_);
    while (!session_expiration_queue_.empty()) {
      auto& top = session_expiration_queue_.top();
      if (top.first > now) {
        break;
      }
      auto id = top.second;
      session_expiration_queue_.pop();
      auto it = sessions_.find(id);
      if (it != sessions_.end()) {
        auto current_expiration = (**it).expiration();
        if (current_expiration > now) {
          session_expiration_queue_.push({current_expiration, id});
        } else {
          sessions_.erase(it);
        }
      }
    }
    ScheduleCheckExpiredSessions(now);
  }

  Status DoPerform(PgPerformRequestPB* req, PgPerformResponsePB* resp, rpc::RpcContext* context) {
    return VERIFY_RESULT(GetSession(*req))->Perform(req, resp, context);
  }

  const TabletServerIf& tablet_server_;
  std::shared_future<client::YBClient*> client_future_;
  scoped_refptr<ClockBase> clock_;
  TransactionPoolProvider transaction_pool_provider_;
  PgTableCache table_cache_;
  rw_spinlock mutex_;

  boost::multi_index_container<
      LockablePgClientSessionPtr,
      boost::multi_index::indexed_by<
          boost::multi_index::hashed_unique<
              boost::multi_index::const_mem_fun<PgClientSession, uint64_t, &PgClientSession::id>
          >
      >
  > sessions_ GUARDED_BY(mutex_);

  using ExpirationEntry = std::pair<CoarseTimePoint, uint64_t>;

  struct CompareExpiration {
    bool operator()(const ExpirationEntry& lhs, const ExpirationEntry& rhs) const {
      // Order is reversed, because std::priority_queue keeps track of the largest element.
      // This comparator is important for the cleanup logic.
      return rhs.first < lhs.first;
    }
  };

  std::priority_queue<ExpirationEntry,
                      std::vector<ExpirationEntry>,
                      CompareExpiration> session_expiration_queue_;

  std::atomic<int64_t> session_serial_no_{0};

  rpc::ScheduledTaskTracker check_expired_sessions_;

  const std::optional<XClusterContext> xcluster_context_;

  PgMutationCounter* pg_node_level_mutation_counter_;

  PgResponseCache response_cache_;

  PgSequenceCache sequence_cache_;

  const Uuid instance_id_;
};

PgClientServiceImpl::PgClientServiceImpl(
    std::reference_wrapper<const TabletServerIf> tablet_server,
    const std::shared_future<client::YBClient*>& client_future,
    const scoped_refptr<ClockBase>& clock,
    TransactionPoolProvider transaction_pool_provider,
    const std::shared_ptr<MemTracker>& parent_mem_tracker,
    const scoped_refptr<MetricEntity>& entity,
    rpc::Scheduler* scheduler,
    const std::optional<XClusterContext>& xcluster_context,
    PgMutationCounter* pg_node_level_mutation_counter)
    : PgClientServiceIf(entity),
      impl_(new Impl(
          tablet_server, client_future, clock, std::move(transaction_pool_provider), scheduler,
          xcluster_context, pg_node_level_mutation_counter, entity.get(), parent_mem_tracker)) {}

PgClientServiceImpl::~PgClientServiceImpl() = default;

void PgClientServiceImpl::Perform(
    const PgPerformRequestPB* req, PgPerformResponsePB* resp, rpc::RpcContext context) {
  impl_->Perform(const_cast<PgPerformRequestPB*>(req), resp, &context);
}

void PgClientServiceImpl::InvalidateTableCache() {
  impl_->InvalidateTableCache();
}

size_t PgClientServiceImpl::TEST_SessionsCount() {
  return impl_->TEST_SessionsCount();
}

#define YB_PG_CLIENT_METHOD_DEFINE(r, data, method) \
void PgClientServiceImpl::method( \
    const BOOST_PP_CAT(BOOST_PP_CAT(Pg, method), RequestPB)* req, \
    BOOST_PP_CAT(BOOST_PP_CAT(Pg, method), ResponsePB)* resp, \
    rpc::RpcContext context) { \
  Respond(impl_->method(*req, resp, &context), resp, &context); \
}

BOOST_PP_SEQ_FOR_EACH(YB_PG_CLIENT_METHOD_DEFINE, ~, YB_PG_CLIENT_METHODS);

}  // namespace tserver
}  // namespace yb
