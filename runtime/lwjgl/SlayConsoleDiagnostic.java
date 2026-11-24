import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.util.Arrays;
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

    private static void configureFontAtlas(File packageRoot) throws IOException {
        Path settings = packageRoot.toPath().resolve("slay-font-atlas-limit.txt");
        if (!Files.isRegularFile(settings)) return;
        String setting = new String(Files.readAllBytes(settings), StandardCharsets.US_ASCII).trim();
        if (!setting.equals("512") && !setting.equals("1024")) {
            throw new IOException("Unexpected font atlas limit");
        }
        try {
            Class<?> generator = Class.forName("com.badlogic.gdx.graphics.g2d.freetype.FreeTypeFontGenerator");
            Method getLimit = generator.getMethod("getMaxTextureSize");
            Method setLimit = generator.getMethod("setMaxTextureSize", int.class);
            int requested = Integer.parseInt(setting);
            int initial = (Integer)getLimit.invoke(null);
            if (initial == -1 || requested < initial) setLimit.invoke(null, requested);
            System.out.println("slay_font_atlas_initial=" + initial);
            System.out.println("slay_font_atlas_limit=" + getLimit.invoke(null));
        } catch (ReflectiveOperationException e) {
            System.out.println("slay_font_atlas_limit=unavailable");
        }
    }

    private static void checkLwjglBinding() {
        System.out.println("slay_lwjgl_egl_binding=begin");
        Method useContext = null;
        boolean bound = false;
        try {
            Class.forName("com.badlogic.gdx.backends.lwjgl.LwjglNativesLoader").getMethod("load").invoke(null);
            System.out.println("slay_lwjgl_egl_binding=natives-ready");
            useContext = Class.forName("org.lwjgl.opengl.GLContext").getMethod("useContext", Object.class);
            useContext.invoke(null, new Object());
            bound = true;
            String version = (String) Class.forName("org.lwjgl.opengl.GL11").getMethod("glGetString", int.class)
                .invoke(null, 0x1F02); // GL_VERSION
            System.out.println("slay_lwjgl_egl_binding=" + (version == null ? "no-version" : "gl-version"));
            Class<?> shaderApi = Class.forName("org.lwjgl.opengl.GL20");
            int shader = (Integer) shaderApi.getMethod("glCreateShader", int.class).invoke(null, 0x8B31); // GL_VERTEX_SHADER
            if (shader != 0) {
                shaderApi.getMethod("glDeleteShader", int.class).invoke(null, shader);
                System.out.println("slay_lwjgl_egl_binding=shader-created");
            } else {
                System.out.println("slay_lwjgl_egl_binding=no-shader");
            }
        } catch (ReflectiveOperationException | LinkageError e) {
            Throwable cause = e.getCause() == null ? e : e.getCause();
            System.out.println("slay_lwjgl_egl_binding_fault=" + cause.getClass().getName());
            String message = cause.getMessage();
            if (message != null && message.contains("in java.library.path")) {
                System.out.println("slay_lwjgl_egl_binding=native-search-path");
            } else if (message != null && message.contains("Can't find dependent libraries")) {
                System.out.println("slay_lwjgl_egl_binding=native-dependency");
            }
        } finally {
            if (bound) {
                try { useContext.invoke(null, (Object) null); }
                catch (ReflectiveOperationException | LinkageError e) {
                    System.out.println("slay_lwjgl_egl_binding=release-error");
                }
            }
        }
    }

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
        File packageRoot = new File(library).getParentFile();
        if (new File(packageRoot, "slay-sdl-display.dll").isFile()) {
            System.setProperty("xbox.lwjgl.sdl.package", packageRoot.getAbsolutePath());
            System.out.println("slay_jvm_max_heap_bytes=" + Runtime.getRuntime().maxMemory());
            configureFontAtlas(packageRoot);
        } else {
            checkMesaInGameJvm(packageRoot);
        }
        Class.forName(gameMain).getMethod("main", String[].class).invoke(null, (Object) new String[0]);
    }

    private static void launchHosted(String game, String library, String jar, Path log) throws Exception {
        File packageRoot = new File(library).getParentFile();
        if (!new File(packageRoot, "slay-live-host.txt").isFile() || System.getenv("XBOX_SDL_HOST_MAILBOX") == null) {
            throw new IOException("The signed Slay player environment is unavailable");
        }
        OutputStream file = Files.newOutputStream(log, StandardOpenOption.CREATE_NEW);
        PrintStream output = new PrintStream(new OutputStream() {
            private int remaining = OUTPUT_LIMIT;

            @Override public synchronized void write(int value) throws IOException {
                if (remaining > 0) {
                    file.write(value);
                    --remaining;
                }
            }

            @Override public synchronized void write(byte[] bytes, int offset, int length) throws IOException {
                int allowed = Math.min(remaining, length);
                file.write(bytes, offset, allowed);
                remaining -= allowed;
            }

            @Override public void flush() throws IOException { file.flush(); }
            @Override public void close() throws IOException { file.close(); }
        }, true, "UTF-8");
        System.setOut(output);
        System.setErr(output);
        Runtime.getRuntime().addShutdownHook(new Thread(output::close));
        System.out.println("slay_console=helper-started");
        launchFromGameDirectory(game, library, jar);
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 5 && args[0].equals("--launch-hosted")) {
            launchHosted(args[1], args[2], args[3], Paths.get(args[4]));
            return;
        }
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
        boolean sdl = new File(packageRoot, "slay-sdl-display.dll").isFile();
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
        if (sdl) {
            String trace = log.getFileName().toString().replaceFirst("\\.log$", ".sdl.txt");
            builder.environment().put("XBOX_SDL_TRACE_FILE", log.resolveSibling(trace).toAbsolutePath().toString());
            builder.environment().put("XBOX_SDL_OPENGL_FRAME_DELAY_MS", "8000");
            builder.environment().put("XBOX_SDL_TRACE_GL", "1");
            if (new File(packageRoot, "slay-low-memory.txt").isFile()) {
                builder.command().addAll(1, Arrays.asList("-Xms32m", "-Xmx256m", "-XX:+UseSerialGC",
                        "-XX:TieredStopAtLevel=1", "-XX:ReservedCodeCacheSize=48m"));
                Files.write(log, "slay_jvm_profile=low-memory\n".getBytes(StandardCharsets.US_ASCII),
                            StandardOpenOption.APPEND);
            }
        }
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
            if (child.waitFor(sdl ? 28 : 20, TimeUnit.SECONDS)) {
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
