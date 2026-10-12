// CW_WORKERS for carts in Chromium. A native run reads CW_WORKERS itself; a
// cart has no environment (wasmcart's security model) and sizes its job pool
// from the host's hardware thread count (wc_host_info_t.hardware_threads,
// which the web host takes from navigator.hardwareConcurrency). So a page
// running a cart reports CW_WORKERS + 1 there: the same pool as natively.

/** with CW_WORKERS set, make the page's navigator.hardwareConcurrency CW_WORKERS + 1 */
export async function workersOverride(page) {
  const w = process.env.CW_WORKERS;
  if (w === undefined || w === '') return;
  const n = Math.max(0, Math.min(63, parseInt(w, 10) || 0)) + 1;
  await page.addInitScript((threads) => {
    Object.defineProperty(Navigator.prototype, 'hardwareConcurrency', { get: () => threads, configurable: true });
  }, n);
}
