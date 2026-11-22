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
            int split = mode == null ? -1 : mode.indexOf('x');
            int width = split < 0 ? 0 : dimension(mode.substring(0, split), 640, 1920);
            int height = split < 0 ? 0 : dimension(mode.substring(split + 1), 360, 1080);
            if (width == 0 || height == 0) throw new IllegalArgumentException("Unsupported signed Slay render size");
            int hostWidth = HOSTED ? dimension(System.getenv("XBOX_SDL_RENDER_WIDTH"), 640, 1920) : 0;
            int hostHeight = HOSTED ? dimension(System.getenv("XBOX_SDL_RENDER_HEIGHT"), 360, 1080) : 0;
            boolean useHost = hostWidth != 0 && hostHeight != 0;
            WIDTH = useHost ? hostWidth : width;
            HEIGHT = useHost ? hostHeight : height;
        } catch (IOException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    private static int dimension(String value, int minimum, int maximum) {
        if (value == null || value.isEmpty() || value.length() > 4) return 0;
        for (int i = 0; i < value.length(); i++) {
            if (value.charAt(i) < '0' || value.charAt(i) > '9') return 0;
        }
        int pixels = Integer.parseInt(value);
        return pixels >= minimum && pixels <= maximum ? pixels : 0;
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
