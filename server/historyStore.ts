import type { HeatingStatusPayload } from './types.ts';

export type HistoryRange = '1h' | '6h' | '24h' | '7d';
export type HistoryResolution = '1m' | '5m' | '15m' | '1h';
export type HistorySensorKey = 'eloremeno' | 'visszatero' | 'kazan' | 'kulteri' | 'szoba';
export type HistorySensorFilter = 'all' | HistorySensorKey;

export interface HistorySample {
  timestamp: string;
  eloremeno: number | null;
  visszatero: number | null;
  kazan: number | null;
  kulteri: number | null;
  szoba: number | null;
  pumpOn: boolean;
}

export type HistoryFilteredSample = {
  timestamp: string;
  pumpOn: boolean;
} & Partial<Record<HistorySensorKey, number | null>>;

export interface HistoryQueryOptions {
  range?: HistoryRange;
  resolution?: HistoryResolution;
  sensor?: HistorySensorFilter;
  nowMs?: number;
}

export interface HistoryQueryResult {
  samples: HistoryFilteredSample[];
  range: HistoryRange;
  resolution: HistoryResolution;
  requestedResolution: HistoryResolution;
  resolutionClamped: boolean;
  count: number;
}

interface InternalSample extends HistorySample {
  epochMs: number;
}

export const HISTORY_RANGE_MS: Record<HistoryRange, number> = {
  '1h': 60 * 60 * 1000,
  '6h': 6 * 60 * 60 * 1000,
  '24h': 24 * 60 * 60 * 1000,
  '7d': 7 * 24 * 60 * 60 * 1000,
};

export const HISTORY_RESOLUTION_MS: Record<HistoryResolution, number> = {
  '1m': 60 * 1000,
  '5m': 5 * 60 * 1000,
  '15m': 15 * 60 * 1000,
  '1h': 60 * 60 * 1000,
};

export const DEFAULT_RESOLUTION_FOR_RANGE: Record<HistoryRange, HistoryResolution> = {
  '1h': '1m',
  '6h': '5m',
  '24h': '15m',
  '7d': '1h',
};

const VALID_RANGES: readonly HistoryRange[] = ['1h', '6h', '24h', '7d'];
const VALID_RESOLUTIONS: readonly HistoryResolution[] = ['1m', '5m', '15m', '1h'];
const VALID_SENSORS: readonly HistorySensorFilter[] = [
  'all',
  'eloremeno',
  'visszatero',
  'kazan',
  'kulteri',
  'szoba',
];

export function normalizeHistoryRange(raw: unknown): HistoryRange | null {
  if (typeof raw !== 'string') return null;
  const val = raw.trim() as HistoryRange;
  return (VALID_RANGES as readonly string[]).includes(val) ? val : null;
}

export function normalizeHistoryResolution(raw: unknown): HistoryResolution | null {
  if (typeof raw !== 'string') return null;
  const val = raw.trim() as HistoryResolution;
  return (VALID_RESOLUTIONS as readonly string[]).includes(val) ? val : null;
}

export function normalizeHistorySensor(raw: unknown): HistorySensorFilter | null {
  if (typeof raw !== 'string') return null;
  const val = raw.trim() as HistorySensorFilter;
  return (VALID_SENSORS as readonly string[]).includes(val) ? val : null;
}

export function clampResolutionForRange(
  range: HistoryRange,
  requested: HistoryResolution
): { effective: HistoryResolution; clamped: boolean } {
  if (range === '7d' && requested !== '1h') {
    return { effective: '1h', clamped: true };
  }
  if (range === '24h' && requested === '1m') {
    return { effective: '5m', clamped: true };
  }
  return { effective: requested, clamped: false };
}

function sanitizeTemp(val: unknown): number | null {
  if (typeof val !== 'number' || !Number.isFinite(val) || val <= -120) {
    return null;
  }
  return Math.round(val * 10) / 10;
}

const BUDAPEST_PARTS_FMT = new Intl.DateTimeFormat('en-CA', {
  timeZone: 'Europe/Budapest',
  year: 'numeric',
  month: '2-digit',
  day: '2-digit',
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  hour12: false,
});

function budapestWallTimeToEpochMs(wallStr: string): number | null {
  const match = /^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})$/.exec(wallStr.trim());
  if (!match) return null;

  const year = Number(match[1]);
  const month = Number(match[2]);
  const day = Number(match[3]);
  const hour = Number(match[4]);
  const minute = Number(match[5]);
  const second = Number(match[6]);

  const targetUtcApprox = Date.UTC(year, month - 1, day, hour, minute, second);
  const parts = BUDAPEST_PARTS_FMT.formatToParts(new Date(targetUtcApprox));
  const getPart = (type: Intl.DateTimeFormatPartTypes): number => {
    const found = parts.find((p) => p.type === type)?.value;
    return found ? Number(found) : 0;
  };

  const bHour = getPart('hour') === 24 ? 0 : getPart('hour');
  const renderedAsUtc = Date.UTC(
    getPart('year'),
    getPart('month') - 1,
    getPart('day'),
    bHour,
    getPart('minute'),
    getPart('second')
  );
  const offsetMs = renderedAsUtc - targetUtcApprox;
  return targetUtcApprox - offsetMs;
}

function resolveSampleTimestamp(
  status: HeatingStatusPayload,
  fallbackNowMs: number
): { timestamp: string; epochMs: number } {
  if (status.time?.espNtpSynced && typeof status.time.espTime === 'string') {
    const parsedEpoch = budapestWallTimeToEpochMs(status.time.espTime);
    if (parsedEpoch !== null && Number.isFinite(parsedEpoch)) {
      return {
        timestamp: new Date(parsedEpoch).toISOString(),
        epochMs: parsedEpoch,
      };
    }
  }
  return {
    timestamp: new Date(fallbackNowMs).toISOString(),
    epochMs: fallbackNowMs,
  };
}

function averageNonNull(values: Array<number | null>): number | null {
  let sum = 0;
  let count = 0;
  for (const v of values) {
    if (v !== null && Number.isFinite(v)) {
      sum += v;
      count++;
    }
  }
  if (count === 0) return null;
  return Math.round((sum / count) * 10) / 10;
}

function filterSampleSensors(
  sample: HistorySample,
  sensor: HistorySensorFilter
): HistoryFilteredSample {
  if (sensor === 'all') {
    return {
      timestamp: sample.timestamp,
      eloremeno: sample.eloremeno,
      visszatero: sample.visszatero,
      kazan: sample.kazan,
      kulteri: sample.kulteri,
      szoba: sample.szoba,
      pumpOn: sample.pumpOn,
    };
  }

  return {
    timestamp: sample.timestamp,
    [sensor]: sample[sensor],
    pumpOn: sample.pumpOn,
  };
}

export class HistoryStore {
  // 7 nap * 24 óra * 60 perc = 10080 minta (1 perces mintavétel)
  public static readonly CAPACITY = 10080;
  public static readonly SAMPLE_INTERVAL_MS = 60_000;

  private readonly buffer: Array<InternalSample | undefined>;
  private head = 0;
  private count = 0;
  private lastRecordedAtMs: number | null = null;

  constructor(
    private readonly capacity: number = HistoryStore.CAPACITY,
    private readonly sampleIntervalMs: number = HistoryStore.SAMPLE_INTERVAL_MS
  ) {
    this.buffer = new Array<InternalSample | undefined>(capacity);
  }

  public recordFromStatus(status: HeatingStatusPayload, nowMs: number = Date.now()): boolean {
    if (this.lastRecordedAtMs !== null && nowMs - this.lastRecordedAtMs < this.sampleIntervalMs) {
      return false;
    }

    const { timestamp, epochMs } = resolveSampleTimestamp(status, nowMs);
    const temps = status.homersekletek;

    const entry: InternalSample = {
      timestamp,
      epochMs,
      eloremeno: sanitizeTemp(temps?.eloremeno),
      visszatero: sanitizeTemp(temps?.visszatero),
      kazan: sanitizeTemp(temps?.kazan),
      kulteri: sanitizeTemp(temps?.kulteri),
      szoba: sanitizeTemp(status.currentTemp),
      pumpOn: Boolean(status.kimenetek?.szivattyu),
    };

    this.buffer[this.head] = entry;
    this.head = (this.head + 1) % this.capacity;
    if (this.count < this.capacity) {
      this.count++;
    }
    this.lastRecordedAtMs = nowMs;
    return true;
  }

  public getOldestFirst(): InternalSample[] {
    const result: InternalSample[] = [];
    const start = (this.head - this.count + this.capacity) % this.capacity;
    for (let i = 0; i < this.count; i++) {
      const idx = (start + i) % this.capacity;
      const entry = this.buffer[idx];
      if (entry) {
        result.push(entry);
      }
    }
    return result;
  }

  public query(options: HistoryQueryOptions = {}): HistoryQueryResult {
    const range: HistoryRange = options.range ?? '24h';
    const requestedResolution: HistoryResolution =
      options.resolution ?? DEFAULT_RESOLUTION_FOR_RANGE[range];
    const sensor: HistorySensorFilter = options.sensor ?? 'all';
    const nowMs = options.nowMs ?? Date.now();

    const { effective: resolution, clamped: resolutionClamped } = clampResolutionForRange(
      range,
      requestedResolution
    );

    const cutoffMs = nowMs - HISTORY_RANGE_MS[range];
    const allSamples = this.getOldestFirst();
    const inRange = allSamples.filter((s) => s.epochMs >= cutoffMs);

    if (inRange.length === 0) {
      return {
        samples: [],
        range,
        resolution,
        requestedResolution,
        resolutionClamped,
        count: 0,
      };
    }

    let aggregated: HistorySample[];

    if (resolution === '1m') {
      aggregated = inRange.map(({ epochMs: _epochMs, ...rest }) => rest);
    } else {
      const bucketMs = HISTORY_RESOLUTION_MS[resolution];
      const buckets = new Map<number, InternalSample[]>();

      for (const sample of inRange) {
        const bucketStart = Math.floor(sample.epochMs / bucketMs) * bucketMs;
        let list = buckets.get(bucketStart);
        if (!list) {
          list = [];
          buckets.set(bucketStart, list);
        }
        list.push(sample);
      }

      const sortedBuckets = Array.from(buckets.entries()).sort((a, b) => a[0] - b[0]);
      aggregated = sortedBuckets.map(([bucketStart, items]) => ({
        timestamp: new Date(bucketStart).toISOString(),
        eloremeno: averageNonNull(items.map((x) => x.eloremeno)),
        visszatero: averageNonNull(items.map((x) => x.visszatero)),
        kazan: averageNonNull(items.map((x) => x.kazan)),
        kulteri: averageNonNull(items.map((x) => x.kulteri)),
        szoba: averageNonNull(items.map((x) => x.szoba)),
        pumpOn: items.some((x) => x.pumpOn),
      }));
    }

    const filtered = aggregated.map((s) => filterSampleSensors(s, sensor));

    return {
      samples: filtered,
      range,
      resolution,
      requestedResolution,
      resolutionClamped,
      count: filtered.length,
    };
  }
}
