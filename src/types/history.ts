export type HistoryRange = '1h' | '6h' | '24h' | '7d';
export type HistoryResolution = '1m' | '5m' | '15m' | '1h';
export type HistorySensorKey = 'eloremeno' | 'visszatero' | 'kazan' | 'kulteri' | 'szoba';
export type HistorySensorFilter = 'all' | HistorySensorKey;

export interface HistorySample {
  timestamp: string;
  eloremeno?: number | null;
  visszatero?: number | null;
  kazan?: number | null;
  kulteri?: number | null;
  szoba?: number | null;
  pumpOn: boolean;
}

export interface HistoryQueryResponse {
  samples: HistorySample[];
  range: HistoryRange;
  resolution: HistoryResolution;
  requestedResolution?: HistoryResolution;
  resolutionClamped?: boolean;
  count: number;
}

export interface SensorSeriesMeta {
  key: HistorySensorKey;
  label: string;
  subtitle: string;
  color: string;
}

export const SENSOR_SERIES: ReadonlyArray<SensorSeriesMeta> = [
  {
    key: 'eloremeno',
    label: 'Előremenő',
    subtitle: 'Kazánból kilépő víz',
    color: '#fb923c', // narancs (orange-400)
  },
  {
    key: 'visszatero',
    label: 'Visszatérő',
    subtitle: 'Fűtési kör visszatérő',
    color: '#38bdf8', // világoskék (sky-400)
  },
  {
    key: 'kazan',
    label: 'Kazán',
    subtitle: 'Kazántest hőfok',
    color: '#f87171', // piros (red-400)
  },
  {
    key: 'kulteri',
    label: 'Kültéri',
    subtitle: 'Külső környezet',
    color: '#94a3b8', // szürke/kék (slate-400)
  },
  {
    key: 'szoba',
    label: 'Szoba (LM35)',
    subtitle: 'Szobai hőmérséklet',
    color: '#34d399', // zöld (emerald-400)
  },
];

export const DEFAULT_RESOLUTION_BY_RANGE: Record<HistoryRange, HistoryResolution> = {
  '1h': '1m',
  '6h': '5m',
  '24h': '15m',
  '7d': '1h',
};

export const RANGE_LABELS: Record<HistoryRange, string> = {
  '1h': '1 óra',
  '6h': '6 óra',
  '24h': '24 óra',
  '7d': '7 nap',
};

export const RESOLUTION_LABELS: Record<HistoryResolution, string> = {
  '1m': '1 perc',
  '5m': '5 perc',
  '15m': '15 perc',
  '1h': '1 óra',
};
