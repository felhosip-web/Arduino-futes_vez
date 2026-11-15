export const API_URL =
  (import.meta.env.VITE_API_URL as string | undefined)?.replace(/\/$/, '') || '';

export const DEVICE_ID = 'haz-futes';

export const HEARTBEAT_INTERVALS = [5, 10, 15, 20, 25, 30] as const;
