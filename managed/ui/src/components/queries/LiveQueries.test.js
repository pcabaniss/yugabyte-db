import axios from 'axios';
import { filterBySearchTokens } from './helpers/queriesHelper';
import { LiveQueries } from './LiveQueries';
import {
  mockYcqlQueries,
  mockLiveYsqlQueries,
  mockKeyMap,
  mockSearchTokens
} from './helpers/mockQueryData';
import { render, screen } from '../../test-utils';
import userEvent from '@testing-library/user-event';

jest.mock('axios');

describe('Live query search bar tests', () => {
  it('Search Bar: single filter on column and value', () => {
    expect(filterBySearchTokens(mockYcqlQueries, mockSearchTokens, mockKeyMap).length).toBe(2);
  });

  it('Search Bar: multiple filters on column and value', () => {
    mockSearchTokens.push({
      key: 'elapsedMillis',
      label: 'Elapsed Time',
      value: '<5'
    });
    expect(filterBySearchTokens(mockYcqlQueries, mockSearchTokens, mockKeyMap).length).toBe(1);
  });

  it('Search Bar: double wildcard range should not work', () => {
    mockSearchTokens.push({
      key: 'elapsedMillis',
      label: 'Elapsed Time',
      value: '*..*'
    });
    expect(filterBySearchTokens(mockYcqlQueries, mockSearchTokens, mockKeyMap).length).toBe(0);
  });
});

// Mock data to mimic React-Router history object
const mockLocation = {
  hash: '',
  host: 'localhost:3000',
  hostname: 'localhost',
  href: 'http://localhost:3000/universes/9e9fba85-eeef-4304-9558-a3efc5670fa0/queries',
  origin: 'http://localhost:3000',
  pathname: '/universes/9e9fba85-eeef-4304-9558-a3efc5670fa0/queries',
  port: '3000',
  protocol: 'http:',
  search: ''
};

beforeEach(() => {
  const ysqlQueries = {
    queries: mockLiveYsqlQueries
  };
  const ycqlQueries = {
    queries: mockYcqlQueries
  };
  const resp = {
    data: {
      ysql: ysqlQueries,
      ycql: ycqlQueries
    }
  };
  axios.get.mockResolvedValue(resp);
  render(<LiveQueries location={mockLocation} />);
});

describe('Live query dashboard tests', () => {
  it('render the Live Queries component', () => {
    expect(screen.getByRole('heading')).toHaveTextContent('Live Queries');
  });

  // "1" in tests below corresponds to table header row
  it('render YSQL queries to be displayed in table', () => {
    expect(screen.getAllByRole('row')).toHaveLength(1 + mockLiveYsqlQueries.length);
  });

  it('switch to display YCQL queries in table', () => {
    expect(screen.getAllByRole('row')).toHaveLength(1 + mockLiveYsqlQueries.length);
    userEvent.click(screen.getByRole('menuitem', { name: /ycql/i }));
    expect(screen.getAllByRole('row')).toHaveLength(1 + mockYcqlQueries.length);
  });
});
