---
title: Automatic Data Expiration
headerTitle: Automatic Data Expiration
linkTitle: Automatic data expiration
description: Distribute your time-ordered data and retrieve fast
headcontent: Distribute your time-ordered data and retrieve fast
menu:
  preview:
    identifier: timeseries-automatic-expiration
    parent: common-patterns-timeseries
    weight: 300
type: docs
---


Consider a scenario where you only need the last few values and the older data is not of any value and can be purged.You would have to set up a background job to clean out older data using many other databases. With YugabyteDB however, you can set an expiration value to the columns with `USING TTL` operator.

{{<note title="Note">}}
TTL-based expiration is only available in [YCQL](../../../../api/ycql/).
{{</note>}}

## Setup

{{<cluster-setup-tabs>}}

## Row-level TTL

Consider a speed metrics tracking system that tracks the data from the speed sensor of many cars.

Create a table and insert data with an example schema as follows:

```sql
CREATE KEYSPACE IF NOT EXISTS yugabyte;
USE yugabyte;
CREATE TABLE exp_demo (
    ts timestamp,/* time at which the event was generated */
    car text, /* name of the car */
    speed int,   /* speed of your car */
    PRIMARY KEY(car, ts)
) WITH CLUSTERING ORDER BY (ts DESC);
```

```sql
INSERT INTO exp_demo(ts,car,speed) VALUES('2023-07-01 10:00:01','car-1',50) USING TTL 10;
INSERT INTO exp_demo(ts,car,speed) VALUES('2023-07-01 10:00:02','car-2',25) USING TTL 15;
INSERT INTO exp_demo(ts,car,speed) VALUES('2023-07-01 10:00:03','car-1',39) USING TTL 15;
INSERT INTO exp_demo(ts,car,speed) VALUES('2023-07-01 10:00:04','car-1',49) USING TTL 20;
INSERT INTO exp_demo(ts,car,speed) VALUES('2023-07-01 10:00:05','car-2', 3) USING TTL 25;
```

As soon as you insert the data, start selecting all rows over and over. Eventually, you will see all the data disappear.

```sql
SELECT * from exp_demo;
```

## Column-level TTL

Instead of setting the TTL on an entire row, you can set TTL per column for more fine-grained expiration. For example,

1. Add a row.

    ```sql
    INSERT INTO exp_demo(ts,car,speed) VALUES('2023-08-01 10:00:01', 'car-5', 50);
    ```

1. Fetch the rows.

    ```sql
    SELECT * FROM exp_demo WHERE car='car-5';
    ```

    ```output
     car   | ts                              | speed
    -------+---------------------------------+-------
     car-5 | 2023-08-01 17:00:01.000000+0000 |    50
    ```

1. Now, set the expiry on the speed column of that row.

    ```sql
    UPDATE exp_demo USING TTL 5 SET speed=10 WHERE car='car-5' AND ts ='2023-08-01 10:00:01';
    ```

1. Wait for `5` seconds and fetch the row for `car-5`.

    ```sql
    SELECT * FROM exp_demo WHERE car='car-5';
    ```

    ```output
     car   | ts                              | speed
    -------+---------------------------------+-------
     car-5 | 2023-08-01 17:00:01.000000+0000 |   null
    ```

Note that the row is present but the value for the `speed` column is `null`.

## Table-level TTL

Instead of explicitly setting the TTL at the row level or column level, you can set a TTL on the table. This also saves a lot of space as the TTL value is stored in only one place and not per row or column.

The table-level TTL can be defined using the [default_time_to_live property](../../../../api/ycql/ddl_create_table/#table-properties-1).

## Learn more

- [TTL for data expiration](../../../learn/ttl-data-expiration-ycql/)