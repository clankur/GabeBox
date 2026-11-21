import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.concurrent.TimeUnit;

public final class SlayConsoleDiagnostic {
    private static final int OUTPUT_LIMIT = 64 * 1024;

    private static void captureOutput(InputStream source, Path log) {
        try (InputStream input = source;
             OutputStream output = Files.newOutputStream(log, StandardOpenOption.APPEND)) {
            byte[] buffer = new byte[4096];
            int remaining = OUTPUT_LIMIT;
            boolean truncated = false;
            for (int count; (count = input.read(buffer)) != -1;) {
                int copy = Math.min(count, remaining);
                output.write(buffer, 0, copy);
                remaining -= copy;
                truncated |= count > copy;
            }
            if (truncated) output.write("\nslay_console_capture=truncated\n".getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            try {
                Files.write(log, "\nslay_console_capture=read-error\n".getBytes(StandardCharsets.UTF_8),
                            StandardOpenOption.APPEND);
            } catch (IOException ignored) { }
        }
    }

    public static void main(String[] args) throws IOException, InterruptedException {
        Path log = Paths.get(args[0]);
        Files.write(log, "slay_console=helper-started\n".getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE_NEW);
        File game = new File(args[1]);
        File launcher = new File(game, "jre/bin/java.exe");
        File jar = new File(game, "desktop-1.0.jar");
        String inputs = "slay_console_jar=" + (jar.isFile() && jar.canRead() ? "readable" : "unavailable") + "\n";
        Files.write(log, inputs.getBytes(StandardCharsets.UTF_8), StandardOpenOption.APPEND);
        ProcessBuilder builder = new ProcessBuilder(launcher.getAbsolutePath(), "-Duser.dir=" + game.getAbsolutePath(),
                                                    "-jar", jar.getAbsolutePath());
        builder.directory(game).redirectErrorStream(true);
        Process child;
        try {
            child = builder.start();
        } catch (IOException e) {
            Files.write(log, "\nslay_console=create-error\n".getBytes(StandardCharsets.UTF_8), StandardOpenOption.APPEND);
            return;
        }
        Thread capture = new Thread(() -> captureOutput(child.getInputStream(), log), "slay-console-capture");
        capture.setDaemon(true);
        capture.start();
        String outcome;
        try {
            if (child.waitFor(20, TimeUnit.SECONDS)) {
                outcome = "exit-" + child.exitValue();
            } else {
                outcome = "running-at-timeout";
            }
        } finally {
            if (child.isAlive()) child.destroyForcibly().waitFor(3, TimeUnit.SECONDS);
        }
        capture.join(2000);
        if (capture.isAlive()) {
            Files.write(log, "\nslay_console_capture=incomplete\n".getBytes(StandardCharsets.UTF_8),
                        StandardOpenOption.APPEND);
        }
        Files.write(log, ("\nslay_console=" + outcome + "\n").getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.APPEND);
    }
}
