package org.lwjgl.opengl;

import java.nio.IntBuffer;

/** Samples the same SDL controller used by the shared Xbox host. */
public final class XboxSdlGamepad {
    private static Runnable listener;

    private XboxSdlGamepad() {}

    private static native int readNative(IntBuffer axes);

    public static int read(IntBuffer axes) {
        return XboxSdlDisplay.enabled() ? readNative(axes) : -1;
    }

    public static void listen(Runnable update) {
        listener = update;
    }

    static void poll() {
        if (listener != null) listener.run();
    }
}
