export type LogLevel = 'INFO' | 'WARN' | 'ERROR';
export type LogSource = 'esp' | 'backend';

export interface LogEntry {
  id: number;
  timestamp: string;
  level: LogLevel;
  code: string;
  message: string;
  source: LogSource;
}

export interface LogsQueryOptions {
  page?: number;
  limit?: number;
  level?: LogLevel;
}

export interface LogsQueryResult {
  logs: LogEntry[];
  total: number;
  page: number;
  pages: number;
  limit: number;
}

export function formatLocalTimestamp(date: Date = new Date()): string {
  const pad = (n: number): string => String(n).padStart(2, '0');
  const y = date.getFullYear();
  const m = pad(date.getMonth() + 1);
  const d = pad(date.getDate());
  const hh = pad(date.getHours());
  const mm = pad(date.getMinutes());
  const ss = pad(date.getSeconds());
  return `${y}-${m}-${d} ${hh}:${mm}:${ss}`;
}

export function normalizeLogLevel(raw: unknown): LogLevel | null {
  if (typeof raw !== 'string') return null;
  const upper = raw.trim().toUpperCase();
  if (upper === 'INFO' || upper === 'WARN' || upper === 'ERROR') {
    return upper;
  }
  return null;
}

export class LogsStore {
  public static readonly CAPACITY = 500;

  private readonly buffer: Array<LogEntry | undefined>;
  private head = 0;
  private count = 0;
  private nextId = 1;

  constructor(private readonly capacity: number = LogsStore.CAPACITY) {
    this.buffer = new Array<LogEntry | undefined>(capacity);
  }

  public add(
    level: LogLevel,
    code: string,
    message: string,
    source: LogSource,
    timestamp?: string
  ): LogEntry {
    const entry: LogEntry = {
      id: this.nextId++,
      timestamp:
        typeof timestamp === 'string' && timestamp.trim().length > 0
          ? timestamp.trim()
          : formatLocalTimestamp(),
      level,
      code: String(code || 'EVENT').slice(0, 32),
      message: String(message ?? '').slice(0, 240),
      source,
    };

    this.buffer[this.head] = entry;
    this.head = (this.head + 1) % this.capacity;
    if (this.count < this.capacity) {
      this.count++;
    }

    return entry;
  }

  public getNewestFirst(level?: LogLevel): LogEntry[] {
    const result: LogEntry[] = [];
    for (let i = 0; i < this.count; i++) {
      const idx = (this.head - 1 - i + this.capacity) % this.capacity;
      const entry = this.buffer[idx];
      if (!entry) continue;
      if (level && entry.level !== level) continue;
      result.push(entry);
    }
    return result;
  }

  public getOldestFirst(level?: LogLevel): LogEntry[] {
    const result: LogEntry[] = [];
    const start = (this.head - this.count + this.capacity) % this.capacity;
    for (let i = 0; i < this.count; i++) {
      const idx = (start + i) % this.capacity;
      const entry = this.buffer[idx];
      if (!entry) continue;
      if (level && entry.level !== level) continue;
      result.push(entry);
    }
    return result;
  }

  public query(options: LogsQueryOptions = {}): LogsQueryResult {
    const rawLimit = typeof options.limit === 'number' && Number.isFinite(options.limit)
      ? Math.floor(options.limit)
      : 20;
    const limit = Math.min(Math.max(rawLimit, 1), 100);

    const rawPage = typeof options.page === 'number' && Number.isFinite(options.page)
      ? Math.floor(options.page)
      : 1;
    const page = Math.max(rawPage, 1);

    const filtered = this.getNewestFirst(options.level);
    const total = filtered.length;
    const pages = total === 0 ? 1 : Math.ceil(total / limit);
    const offset = (page - 1) * limit;
    const logs = filtered.slice(offset, offset + limit);

    return {
      logs,
      total,
      page,
      pages,
      limit,
    };
  }

  public exportText(level?: LogLevel): string {
    const entries = this.getOldestFirst(level);
    if (entries.length === 0) {
      return '';
    }
    return (
      entries
        .map((e) => `[${e.timestamp}] [${e.level}] ${e.code} - ${e.message}`)
        .join('\n') + '\n'
    );
  }

  public clear(): number {
    const deleted = this.count;
    this.buffer.fill(undefined);
    this.head = 0;
    this.count = 0;
    return deleted;
  }
}
