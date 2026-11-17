export type LogLevel = 'INFO' | 'WARN' | 'ERROR';
export type LogLevelFilter = 'ALL' | LogLevel;
export type LogSource = 'esp' | 'backend';

export interface LogEntry {
  id: number;
  timestamp: string;
  level: LogLevel;
  code: string;
  message: string;
  source: LogSource;
}

export interface LogsQueryResponse {
  logs: LogEntry[];
  total: number;
  page: number;
  pages: number;
  limit: number;
}
