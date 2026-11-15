export const API_URL =
  (import.meta.env.VITE_API_URL as string | undefined)?.replace(/\/$/, '') || '';

export const DEVICE_ID = 'haz-futes';
