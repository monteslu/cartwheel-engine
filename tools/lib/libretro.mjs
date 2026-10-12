// Which wasmcart-libretro core and headless libretro frontend (its
// test/lr_frontend: Mesa surfaceless EGL, RetroArch's call order) the tests
// run, in one place.
//
// The build is wasmcart-libretro's build/ beside the checkout (or beside the
// main checkout when this is a git worktree under worktrees/), configured with
// -DWASMCART_LIBRETRO_TESTS=ON. CW_LIBRETRO_CORE names the core,
// CW_LR_FRONTEND the frontend. There is no other fallback: a missing build
// throws, so a run never takes an old core from some other directory.
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const builds = [join(ROOT, '..'), join(ROOT, '..', '..')].map((d) => join(d, 'wasmcart-libretro', 'build'));

function named(envName) {
  const v = process.env[envName];
  if (!existsSync(v)) throw new Error(`libretro: ${envName}=${v} does not exist`);
  return v;
}

/** { core, frontend }: the libretro core and lr_frontend; throws if either is missing */
export function libretroBuild() {
  const b = builds.find((d) => existsSync(join(d, 'wasmcart_libretro.so')) && existsSync(join(d, 'lr_frontend')));
  const core = process.env.CW_LIBRETRO_CORE ? named('CW_LIBRETRO_CORE') : b && join(b, 'wasmcart_libretro.so');
  const frontend = process.env.CW_LR_FRONTEND ? named('CW_LR_FRONTEND') : b && join(b, 'lr_frontend');
  if (!core || !frontend)
    throw new Error(`libretro: no wasmcart_libretro.so + lr_frontend in ${builds.join(' or ')} ` +
      '(build wasmcart-libretro with -DWASMCART_LIBRETRO_TESTS=ON, or set CW_LIBRETRO_CORE / CW_LR_FRONTEND)');
  return { core, frontend };
}
