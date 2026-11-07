// The version an app carries (a cart's manifest `version`, an export's package
// names): one rule for the cart build (tools/cart-version.mjs, which
// cw_add_app runs every build) and for cwexport, so the two always agree.
//
//   resolveAppVersion({ set, dir, fallback })
//     set       an explicit version (cw_add_app VERSION, CW_APP_VERSION, cwexport --version)
//     dir       the app's source directory: its git checkout's version (gitVersion)
//     fallback  project.cwproj's `version` (1 and 1.2 mean 1.0.0 and 1.2.0)
//   the first of them that is given, else 0.1.0.
//
// Every version is semver 2.0.0 (wasmcart's manifest example is "1.0.0"; npm,
// launchers and stores compare them as semver).
import { spawnSync } from 'node:child_process';

/**
 * What is wrong with a version as semver 2.0.0 (MAJOR.MINOR.PATCH, optionally
 * -prerelease and +build), or null when it is fine. cmake/app.cmake's
 * cw__check_semver applies the same rule at configure time.
 * @param {string} v
 * @returns {string|null}
 */
export function semverProblem(v) {
  const id = '[0-9A-Za-z-]+';
  const m = new RegExp(`^(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)(?:-(${id}(?:\\.${id})*))?(?:\\+${id}(?:\\.${id})*)?$`).exec(String(v));
  if (!m) return 'not MAJOR.MINOR.PATCH[-prerelease][+build], or a number with a leading zero';
  const bad = (m[4] || '').split('.').find((x) => /^0\d+$/.test(x));
  return bad ? `prerelease identifier ${bad} is a number with a leading zero` : null;
}

/**
 * A git checkout's version: at a v<semver> tag with no changes to tracked
 * files, the tag without its v (a tag build is the release by construction);
 * otherwise the last v* tag reachable from HEAD plus -dev.g<commit>
 * (0.0.0-dev.g<commit> before the first tag), so a dev build is never taken
 * for a release. The g keeps the identifier valid semver when the abbreviated
 * commit is all digits with a leading zero. Null outside a git checkout.
 * @param {string} dir
 * @returns {string|null}
 */
export function gitVersion(dir) {
  const git = (...a) => {
    const r = spawnSync('git', ['-C', dir, ...a], { encoding: 'utf8' });
    return r.status === 0 ? r.stdout.trim() : null;
  };
  const sha = git('rev-parse', '--short=7', 'HEAD');
  if (!sha) return null;
  const semverTag = (t) => (t && t.startsWith('v') && !semverProblem(t.slice(1)) ? t.slice(1) : null);
  const exact = (git('tag', '--points-at', 'HEAD', '--list', 'v[0-9]*') || '').split('\n').map(semverTag).filter(Boolean);
  const dirty = git('status', '--porcelain', '--untracked-files=no');
  if (exact.length && dirty === '') return exact.sort(compareSemver).at(-1);
  const last = semverTag(git('describe', '--tags', '--abbrev=0', '--match', 'v[0-9]*'));
  return `${last || '0.0.0'}-dev.g${sha}`;
}

/** semver precedence of two release versions (prereleases sort before their release) */
function compareSemver(a, b) {
  const p = (v) => v.split(/[-+]/)[0].split('.').map(Number);
  const [x, y] = [p(a), p(b)];
  for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] - y[i];
  return (a.includes('-') ? 0 : 1) - (b.includes('-') ? 0 : 1);
}

/** project.cwproj's 1 and 1.2 as 1.0.0 and 1.2.0 */
export function normalizeVersion(v) {
  return /^\d+(\.\d+)?$/.test(v) ? (v + '.0.0').split('.').slice(0, 3).join('.') : v;
}

/**
 * @param {{set?: string, dir?: string, fallback?: string}} o
 * @returns {{version: string, from: string, error: string|null}}
 */
export function resolveAppVersion({ set, dir, fallback }) {
  let version, from;
  if (set) [version, from] = [set, 'set'];
  else if (dir && (version = gitVersion(dir))) from = 'git';
  else if (fallback) [version, from] = [normalizeVersion(fallback), 'project.cwproj'];
  else [version, from] = ['0.1.0', 'default'];
  const why = semverProblem(version);
  return { version, from, error: why ? `version "${version}" (${from}) is not semver (${why}): use 1.2.3, 1.2.3-rc.1 or 1.2.3-dev.g1a2b3c4` : null };
}
