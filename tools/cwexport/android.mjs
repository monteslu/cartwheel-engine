// Android arm64: a Gradle project (SDL2's Java activity, the game prebuilt as
// jniLibs/arm64-v8a/libmain.so with SDL2 static inside, GLES 3.0 from the
// NDK, the asset pack as assets/game.cwpak) and the APK Gradle builds from it.
//
// Signing: with preset keys `keystore` (path, relative to the project) and
// `keystore_alias`, and the passwords in CW_ANDROID_STORE_PASSWORD /
// CW_ANDROID_KEY_PASSWORD, the APK is a signed release build. Without them it
// is a debug build (Android's debug key): installable for testing, not for a
// store.
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { join, isAbsolute } from 'node:path';
import { homedir } from 'node:os';
import { buildApp, packAssets, strip, write, freshDir, copy, copyTree, run, slug, ROOT, ExportError, listFiles } from './util.mjs';
import { writeIcon } from './icon.mjs';

const TEMPLATE = join(ROOT, 'tools/cwexport/templates/android');
const SDL_ANDROID = join(ROOT, 'deps/sdl2/android-project');
const GRADLE_VERSION = '8.14.3';

/** The Android SDK: ANDROID_HOME, ANDROID_SDK_ROOT, else ~/Android/Sdk. */
export function sdkDir() {
  return process.env.ANDROID_HOME || process.env.ANDROID_SDK_ROOT || join(homedir(), 'Android/Sdk');
}

export function validate(ctx) {
  const sdk = sdkDir();
  if (!existsSync(join(sdk, 'platforms'))) throw new ExportError(`no Android SDK at ${sdk}: set ANDROID_HOME`);
  if (!existsSync(join(sdk, 'platforms/android-36'))) throw new ExportError(`the Android SDK at ${sdk} lacks platforms/android-36 (sdkmanager "platforms;android-36")`);
  if (!process.env.ANDROID_NDK_HOME && !(existsSync(join(sdk, 'ndk')) && readdirSync(join(sdk, 'ndk')).length))
    throw new ExportError(`no Android NDK in ${sdk}/ndk: install one (sdkmanager "ndk;27.2.12479018") or set ANDROID_NDK_HOME`);
  if (!existsSync(join(SDL_ANDROID, 'app/src/main/java/org/libsdl/app/SDLActivity.java'))) throw new ExportError('deps/sdl2 is missing: run scripts/fetch-deps.sh');
  const id = appId(ctx);
  if (!/^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+$/.test(id)) throw new ExportError(`app_id "${id}" is not a Java package name (e.g. org.example.mygame)`);
  if (ctx.opt('keystore') && !ctx.opt('keystore_alias')) throw new ExportError('keystore is set without keystore_alias');
}

const appId = (ctx) => ctx.opt('app_id', `org.cartwheel.${slug(ctx.app).replace(/[^a-z0-9_]/g, '_')}`);

/** "1.2.3" -> 10203 (Android's integer version) */
function versionCode(v) {
  const p = String(v).split('.').map((x) => parseInt(x, 10) || 0);
  return (p[0] || 0) * 10000 + (p[1] || 0) * 100 + (p[2] || 0) || 1;
}

export async function runAndroid(ctx) {
  const app = await buildApp(ctx, 'android-arm64');
  const name = slug(ctx.app);
  const proj = freshDir(join(ctx.outDir, `${name}-android`));
  ctx.progress(50, 'gradle project');
  const signing = ctx.opt('keystore');
  const ks = signing && (isAbsolute(signing) ? signing : join(ctx.projectDir, signing));
  if (ks && !existsSync(ks)) throw new ExportError(`keystore ${signing}: no such file`);
  const vars = {
    '@NAME@': name, '@TITLE@': ctx.title.replace(/&/g, '&amp;').replace(/</g, '&lt;'), '@APP_ID@': appId(ctx),
    '@VERSION@': ctx.version, '@VERSION_CODE@': String(versionCode(ctx.version)),
    '@ORIENTATION@': ctx.opt('orientation', 'sensorLandscape'),
    '@SIGNING_CONFIGS@': ks ? `    signingConfigs {
        release {
            storeFile file('${ks.replace(/\\/g, '/')}')
            storePassword System.getenv('CW_ANDROID_STORE_PASSWORD')
            keyAlias '${ctx.opt('keystore_alias')}'
            keyPassword System.getenv('CW_ANDROID_KEY_PASSWORD')
        }
    }` : '',
    '@RELEASE_SIGNING@': ks ? '            signingConfig signingConfigs.release' : '',
  };
  for (const f of listFiles(TEMPLATE)) {
    let t = readFileSync(join(TEMPLATE, f), 'utf8');
    for (const [k, v] of Object.entries(vars)) t = t.replaceAll(k, v);
    write(join(proj, f), t);
  }
  // SDL2's Java side (the same SDL version as the native library) and its Gradle wrapper
  copyTree(join(SDL_ANDROID, 'app/src/main/java/org/libsdl'), join(proj, 'app/src/main/java/org/libsdl'));
  copy(join(SDL_ANDROID, 'gradlew'), join(proj, 'gradlew'));
  copy(join(SDL_ANDROID, 'gradlew.bat'), join(proj, 'gradlew.bat'));
  copy(join(SDL_ANDROID, 'gradle/wrapper/gradle-wrapper.jar'), join(proj, 'gradle/wrapper/gradle-wrapper.jar'));
  write(join(proj, 'gradle/wrapper/gradle-wrapper.properties'), `distributionBase=GRADLE_USER_HOME
distributionPath=wrapper/dists
distributionUrl=https\\://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip
zipStoreBase=GRADLE_USER_HOME
zipStorePath=wrapper/dists
`);
  const { chmodSync } = await import('node:fs');
  chmodSync(join(proj, 'gradlew'), 0o755);
  write(join(proj, 'local.properties'), `sdk.dir=${sdkDir()}\n`);
  // the game
  copy(app.binary, join(proj, 'app/src/main/jniLibs/arm64-v8a/libmain.so'));
  await strip(join(proj, 'app/src/main/jniLibs/arm64-v8a/libmain.so'), ctx.log);
  const pak = await packAssets(ctx, app, join(proj, 'app/src/main/assets/game.cwpak'));
  if (!pak) write(join(proj, 'app/src/main/assets/.keep'), '');
  for (const [dir, px] of [['mdpi', 48], ['hdpi', 72], ['xhdpi', 96], ['xxhdpi', 144], ['xxxhdpi', 192]])
    writeIcon(ctx, join(proj, `app/src/main/res/mipmap-${dir}/ic_launcher.png`), px);
  write(join(proj, 'README.txt'), `${ctx.title} ${ctx.version}: Android Studio / Gradle project.

./gradlew assembleRelease (signed with your keystore) or assembleDebug.
The game is prebuilt: app/src/main/jniLibs/arm64-v8a/libmain.so (re-export
from cartwheel to change it); its assets are app/src/main/assets/game.cwpak.
`);
  ctx.progress(65, 'gradle assemble');
  const task = ks ? 'assembleRelease' : 'assembleDebug';
  await run(join(proj, 'gradlew'), ['--no-daemon', '-q', task], { cwd: proj, log: ctx.log, env: { ANDROID_HOME: sdkDir() } });
  const variant = ks ? 'release' : 'debug';
  const built = join(proj, `app/build/outputs/apk/${variant}/app-${variant}.apk`);
  if (!existsSync(built)) throw new ExportError(`gradle made no ${built}`);
  const apk = join(ctx.outDir, `${name}-${ctx.version}-android-arm64${ks ? '' : '-debug'}.apk`);
  copy(built, apk);
  return [apk, proj];
}
