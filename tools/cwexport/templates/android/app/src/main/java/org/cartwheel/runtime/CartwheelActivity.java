package org.cartwheel.runtime;

import android.content.pm.ApplicationInfo;

import org.libsdl.app.SDLActivity;

/**
 * The cartwheel game activity: SDL2's activity with the game (SDL statically
 * linked into it) as the one native library, libmain.so.
 *
 * Debuggable builds only: an intent string extra "cw_env" ("KEY=VALUE;...")
 * reaches the game as "--cw-env KEY=VALUE;..." and becomes environment
 * variables before it starts (src/platform/native_entry.c), so tools can turn
 * on cartwheel's test hooks, e.g. CW_REMOTE=tcp:127.0.0.1:7400 (the debug
 * channel over `adb reverse`). Release builds ignore it.
 */
public class CartwheelActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        boolean debuggable = (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        String env = getIntent() != null ? getIntent().getStringExtra("cw_env") : null;
        if (debuggable && env != null && !env.isEmpty()) return new String[] { "--cw-env", env };
        return new String[0];
    }
}
