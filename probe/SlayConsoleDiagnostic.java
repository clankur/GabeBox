import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.util.concurrent.TimeUnit;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;

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

    private static native int setGameDirectory(String path);
    private static native int[] runMesaControl();

    private static void checkMesaInGameJvm(File packageRoot) {
        File mesaLibrary = new File(packageRoot, "mesa-jni-control.dll");
        if (!mesaLibrary.isFile()) return;
        try {
            System.out.println("slay_mesa_child=loading");
            System.load(mesaLibrary.getAbsolutePath());
            System.out.println("slay_mesa_child=entered");
            int[] result = runMesaControl();
            System.out.printf("slay_mesa_child_stage=%d%nslay_mesa_child_win32=0x%08X%nslay_mesa_child_egl=0x%08X%n" +
                              "slay_mesa_child_gl=0x%08X%nslay_mesa_child_fbo=0x%08X%n",
                              result[0], result[1], result[2], result[3], result[4]);
        } catch (UnsatisfiedLinkError e) {
            System.out.println("slay_mesa_child=load-error");
        }
    }

    private static void reportBundledLwjgl(File gameJar, Path log) throws Exception {
        try (JarFile archive = new JarFile(gameJar)) {
            JarEntry entry = archive.getJarEntry("lwjgl64.dll");
            if (entry == null || entry.getSize() <= 0 || entry.getSize() > 2 * 1024 * 1024) return;
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream input = archive.getInputStream(entry)) {
                byte[] buffer = new byte[4096];
                for (int count; (count = input.read(buffer)) != -1;) digest.update(buffer, 0, count);
            }
            StringBuilder entryHash = new StringBuilder("slay_console_lwjgl_sha256=");
            for (byte value : digest.digest()) entryHash.append(String.format("%02x", value & 0xff));
            Files.write(log, (entryHash + "\n").getBytes(StandardCharsets.UTF_8), StandardOpenOption.APPEND);
        }
    }

    private static void launchFromGameDirectory(String game, String library, String jar) throws Exception {
        final String gameMain = "com.megacrit.cardcrawl.desktop.DesktopLauncher";
        try (JarFile archive = new JarFile(jar)) {
            if (archive.getManifest() == null || !gameMain.equals(archive.getManifest().getMainAttributes().getValue("Main-Class"))) {
                System.out.println("slay_console_cwd=unexpected-main-class");
                System.exit(1);
            }
        }
        try {
            System.load(library);
        } catch (UnsatisfiedLinkError e) {
            System.out.println("slay_console_cwd=native-load-error");
            System.exit(1);
        }
        int error = setGameDirectory(game);
        if (error != 0) {
            System.out.printf("slay_console_cwd_error=0x%08X%n", error);
            System.exit(1);
        }
        System.out.println("slay_console_cwd=game-directory");
        checkMesaInGameJvm(new File(library).getParentFile());
        Class.forName(gameMain).getMethod("main", String[].class).invoke(null, (Object) new String[0]);
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 4 && args[0].equals("--launch-from-game-directory")) {
            launchFromGameDirectory(args[1], args[2], args[3]);
            return;
        }
        Path log = Paths.get(args[0]);
        Files.write(log, "slay_console=helper-started\n".getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE_NEW);
        File game = new File(args[1]);
        File launcher = new File(game, "jre/bin/java.exe");
        File jar = new File(game, "desktop-1.0.jar");
        String inputs = "slay_console_jar=" + (jar.isFile() && jar.canRead() ? "readable" : "unavailable") + "\n";
        Files.write(log, inputs.getBytes(StandardCharsets.UTF_8), StandardOpenOption.APPEND);
        String packageRoot = System.getProperty("java.class.path");
        File workingDirectory = new File(packageRoot, "slay-working-directory.dll");
        ProcessBuilder builder;
        if (workingDirectory.isFile()) {
            reportBundledLwjgl(jar, log);
            Path runtimeTemp = Files.createDirectories(log.getParent().resolve("slay-native-temp"));
            File displayFallback = new File(packageRoot, "slay-lwjgl-display.jar");
            String classpath = packageRoot + File.pathSeparator;
            if (displayFallback.isFile()) classpath += displayFallback.getAbsolutePath() + File.pathSeparator;
            classpath += jar.getAbsolutePath();
            builder = new ProcessBuilder(launcher.getAbsolutePath(), "-Duser.dir=" + game.getAbsolutePath(),
                                         "-Djava.io.tmpdir=" + runtimeTemp.toAbsolutePath(), "-cp", classpath, "SlayConsoleDiagnostic",
                                         "--launch-from-game-directory", game.getAbsolutePath(),
                                         workingDirectory.getAbsolutePath(), jar.getAbsolutePath());
        } else {
            builder = new ProcessBuilder(launcher.getAbsolutePath(), "-Duser.dir=" + game.getAbsolutePath(),
                                         "-jar", jar.getAbsolutePath());
        }
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
