// The romdev server this script talks to: ROMDEV_URL, else http://127.0.0.1:$ROMDEV_PORT, else http://127.0.0.1:7331.
// A GPU pool job sets ROMDEV_URL (and ROMDEV_PORT) so the script reaches the server pinned to the job's GPU: a
// hardcoded address would put the job's GPU work on another server's GPU.
const raw = process.env.ROMDEV_URL || `http://127.0.0.1:${process.env.ROMDEV_PORT || 7331}`;
/** the server's base URL, no trailing slash */
export const ROMDEV_URL = raw.replace(/\/+$/, '');
/** the server's port (a number) */
export const ROMDEV_PORT = Number(new URL(ROMDEV_URL).port) || 80;
/** the server's host name */
export const ROMDEV_HOST = new URL(ROMDEV_URL).hostname;
