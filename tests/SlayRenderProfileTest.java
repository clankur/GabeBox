import com.megacrit.cardcrawl.core.DisplayConfig;
import org.lwjgl.opengl.DisplayMode;
import org.lwjgl.opengl.XboxRenderProfile;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;

/** Run in an empty temporary directory with XBOX_SDL_HOST_MAILBOX set. */
public final class SlayRenderProfileTest {
    private static void check(boolean valid) { if (!valid) throw new AssertionError(); }

    public static void main(String[] args) throws Exception {
        Path display = Paths.get("info.displayconfig");
        if (Files.exists(display)) throw new IllegalArgumentException("Use an empty test directory");
        System.setProperty("java.io.tmpdir", Files.createTempDirectory(Paths.get("."), "host").toString());
        Method pace = XboxRenderProfile.class.getDeclaredMethod("producerFps", int.class);
        pace.setAccessible(true);
        check((int)pace.invoke(null, 30) == 30 && (int)pace.invoke(null, 60) == 120);
        check((int)pace.invoke(null, 75) == 75 && (int)pace.invoke(null, 240) == 120);
        check(DisplayConfig.readConfig().getMaxFPS() == 60 && !Files.exists(display));
        Files.write(display, "1920\n1080\nfalse\nfalse\ntrue\n".getBytes(StandardCharsets.US_ASCII));
        check(DisplayConfig.readConfig().getMaxFPS() == 60);
        byte[] original = "1920\n1080\n30\ntrue\nfalse\ntrue\n".getBytes(StandardCharsets.US_ASCII);
        Files.write(display, original);
        DisplayConfig config = DisplayConfig.readConfig();
        DisplayMode mode = new DisplayMode(1280, 720);
        check(mode.getWidth() == config.getWidth() && mode.getHeight() == config.getHeight());
        check(config.getWidth() == Integer.parseInt(args[0]) && config.getHeight() == Integer.parseInt(args[1]));
        check(config.getMaxFPS() == 30 && !config.getIsVsync() && !config.getIsFullscreen());
        DisplayConfig.writeDisplayConfigFile(mode.getWidth(), mode.getHeight(), 24, true, false, true);
        check(DisplayConfig.readConfig().getMaxFPS() == 24 && Arrays.equals(original, Files.readAllBytes(display)));
        DisplayConfig.writeDisplayConfigFile(mode.getWidth(), mode.getHeight(), 60, false, false, false);
        check(DisplayConfig.readConfig().getMaxFPS() == 60 && Arrays.equals(original, Files.readAllBytes(display)));
    }
}
