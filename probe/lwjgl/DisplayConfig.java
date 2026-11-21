package com.megacrit.cardcrawl.core;

import java.io.BufferedReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import org.lwjgl.opengl.XboxRenderProfile;

/** The hosted game's display settings. Save slots and run data still use the publisher's implementations. */
public final class DisplayConfig {
    private final int fps;

    private DisplayConfig(int fps) { this.fps = fps; }

    private static Path hostedFpsFile() {
        // The native Slay launcher provides its own persistent, writable slay-native-temp directory.
        return Paths.get(System.getProperty("java.io.tmpdir"), "xbox-slay-fps.txt");
    }

    private static int positiveFps(String value) {
        int limit = Integer.parseInt(value.trim());
        if (limit <= 0) throw new NumberFormatException("Nonpositive FPS setting");
        return limit;
    }

    public static DisplayConfig readConfig() {
        try (BufferedReader reader = Files.newBufferedReader(hostedFpsFile(), StandardCharsets.US_ASCII)) {
            String line = reader.readLine();
            if (line != null) return new DisplayConfig(positiveFps(line));
        } catch (IOException | NumberFormatException ignored) { }
        try (BufferedReader reader = Files.newBufferedReader(Paths.get("info.displayconfig"), StandardCharsets.UTF_8)) {
            // Older five-line files have a fullscreen flag in place of the FPS limit.
            reader.readLine();
            reader.readLine();
            String line = reader.readLine();
            if (line != null) return new DisplayConfig(positiveFps(line));
        } catch (IOException | NumberFormatException ignored) { }
        return new DisplayConfig(60);
    }

    public static synchronized void writeDisplayConfigFile(int width, int height, int fps, boolean fullscreen,
            boolean borderless, boolean vsync) {
        // The package supplies the single virtual display. Store explicit FPS changes only in Xbox app data.
        if (fps <= 0 || fps == readConfig().fps) return;
        Path target = hostedFpsFile();
        Path pending = null;
        try {
            pending = Files.createTempFile(target.getParent(), "xbox-slay-fps-", ".tmp");
            Files.write(pending, Integer.toString(fps).getBytes(StandardCharsets.US_ASCII));
            try {
                Files.move(pending, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(pending, target, StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException e) {
            System.err.println("slay_display_save=unavailable");
        } finally {
            if (pending != null) try { Files.deleteIfExists(pending); } catch (IOException ignored) { }
        }
    }

    public int getWidth() { return XboxRenderProfile.WIDTH; }
    public int getHeight() { return XboxRenderProfile.HEIGHT; }
    public int getMaxFPS() { return fps; }
    public boolean getIsFullscreen() { return false; }
    public boolean getWFS() { return false; }
    public boolean getIsVsync() { return false; }
}
