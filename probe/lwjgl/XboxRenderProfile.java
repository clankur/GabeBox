package org.lwjgl.opengl;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/** Slay's offscreen window is scaled to the television by the fullscreen player. */
public final class XboxRenderProfile {
    private static final boolean HOSTED = System.getenv("XBOX_SDL_HOST_MAILBOX") != null;
    public static final int WIDTH;
    public static final int HEIGHT;
    static {
        try (InputStream input = XboxRenderProfile.class.getResourceAsStream("/xbox-slay-render-size.txt")) {
            String mode = input == null ? "1280x720"
                    : new BufferedReader(new InputStreamReader(input, StandardCharsets.US_ASCII)).readLine();
            if ("960x540".equals(mode)) { WIDTH = 960; HEIGHT = 540; }
            else if ("854x480".equals(mode)) { WIDTH = 854; HEIGHT = 480; }
            else if ("640x360".equals(mode)) { WIDTH = 640; HEIGHT = 360; }
            else if ("1280x720".equals(mode)) { WIDTH = 1280; HEIGHT = 720; }
            else throw new IllegalArgumentException("Unsupported signed Slay render size");
        } catch (IOException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    // The game's windowed minimum (800x450) otherwise substitutes a 1280x720 window.
    static int windowWidth(int requested) { return HOSTED ? WIDTH : requested; }
    static int windowHeight(int requested) { return HOSTED ? HEIGHT : requested; }
    static int swapInterval(int requested) { return HOSTED ? 0 : requested; }

    static int producerFps(int configured) {
        if (!HOSTED) return configured;
        return configured == 60 ? 120 : Math.min(configured, 120);
    }
}
