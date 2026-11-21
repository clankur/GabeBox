import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.concurrent.TimeUnit;

public final class SlayConsoleDiagnostic {
    public static void main(String[] args) throws IOException, InterruptedException {
        Path log = Paths.get(args[0]);
        Files.write(log, "slay_console=helper-started\n".getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE_NEW);
        File game = new File(args[1]);
        File launcher = new File(game, "jre/bin/java.exe");
        ProcessBuilder builder = new ProcessBuilder(launcher.getAbsolutePath(), "-jar", "desktop-1.0.jar");
        builder.directory(game).redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.appendTo(log.toFile()));
        Process child;
        try {
            child = builder.start();
        } catch (IOException e) {
            Files.write(log, "\nslay_console=create-error\n".getBytes(StandardCharsets.UTF_8), StandardOpenOption.APPEND);
            return;
        }
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
        Files.write(log, ("\nslay_console=" + outcome + "\n").getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.APPEND);
    }
}
