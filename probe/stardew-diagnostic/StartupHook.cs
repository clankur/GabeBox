using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

internal static class StartupHook
{
    public static void Initialize()
    {
        var target = Path.GetFileNameWithoutExtension(Environment.GetCommandLineArgs()[0]);
        var log = Environment.GetEnvironmentVariable("XBOX_STEAM_MANAGED_LOG");
        if (target != "Stardew Valley" || string.IsNullOrEmpty(log))
            return;

        if (Environment.GetEnvironmentVariable("XBOX_SDL_TRACE_FILE") == null)
            Environment.SetEnvironmentVariable("XBOX_SDL_TRACE_FILE", Path.ChangeExtension(log, ".sdl.txt"));

        var package = Path.GetDirectoryName(typeof(StartupHook).Assembly.Location);
        var local = Path.GetDirectoryName(log);
        if (package != null && File.Exists(Path.Combine(package, "stardew-sdl-surfaceless.txt")))
        {
            Environment.SetEnvironmentVariable("SDL_VIDEODRIVER", "offscreen");
            Environment.SetEnvironmentVariable("SDL_VIDEO_GL_DRIVER", Path.Combine(package, "opengl32.dll"));
            Environment.SetEnvironmentVariable("SDL_VIDEO_EGL_DRIVER", Path.Combine(package, "libEGL.dll"));
            Environment.SetEnvironmentVariable("SDL_VIDEO_WIN_D3DCOMPILER", "none");
            Environment.SetEnvironmentVariable("XBOX_SDL_SURFACELESS", "1");
        }

        void Record(string entry)
        {
            try { File.AppendAllText(log, entry + "\n"); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        Record("stardew_managed=entered");
        if (package != null && local != null && File.Exists(Path.Combine(package, "stardew-game-directory.txt")))
        {
            var expectedGame = Path.Combine(local, "SteamLibrary", "steamapps", "common", "Stardew Valley");
            if (string.Equals(Path.GetFullPath(AppContext.BaseDirectory).TrimEnd('\\'), expectedGame,
                              StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    Directory.SetCurrentDirectory(expectedGame);
                    Record("stardew_game_directory=ready");
                }
                catch (IOException) { Record("stardew_game_directory=error"); }
                catch (UnauthorizedAccessException) { Record("stardew_game_directory=error"); }
            }
            else Record("stardew_game_directory=unexpected");
        }
        int remaining = 12;
        AppDomain.CurrentDomain.FirstChanceException += (_, args) =>
        {
            var exception = args.Exception;
            string? kind = exception switch
            {
                DllNotFoundException _ => "dll-not-found",
                EntryPointNotFoundException _ => "entry-point-not-found",
                BadImageFormatException _ => "bad-image-format",
                PlatformNotSupportedException _ => "platform-not-supported",
                _ => null,
            };
            if (kind is null || Interlocked.Decrement(ref remaining) < 0)
                return;

            Record("stardew_managed_exception=" + kind);
            if (exception.Message.IndexOf("opengl32", StringComparison.OrdinalIgnoreCase) >= 0)
                Record("stardew_managed_library=opengl32");
            if (exception.Message.IndexOf("SDL2", StringComparison.OrdinalIgnoreCase) >= 0)
                Record("stardew_managed_library=sdl2");
        };
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            Record("stardew_managed=unhandled-exception");
            var fault = args.ExceptionObject as Exception;
            var inner = fault;
            for (int depth = 0; inner != null && depth < 5; depth++, inner = inner.InnerException)
            {
                Record("stardew_managed_fault=" + inner.GetType().FullName);
                if (inner.Message.IndexOf("opengl", StringComparison.OrdinalIgnoreCase) >= 0)
                    Record("stardew_managed_detail=opengl");
                if (inner.Message.IndexOf("SDL", StringComparison.OrdinalIgnoreCase) >= 0)
                    Record("stardew_managed_detail=sdl");
                if (inner is UnauthorizedAccessException)
                {
                    Record("stardew_managed_detail=access-denied");
                    try { RecordFileAccess(inner, local, package, Record); }
                    catch (Exception) { Record("stardew_file_location=unclassified"); }
                }
            }
            if (fault?.GetType().FullName == "Microsoft.Xna.Framework.Graphics.NoSuitableGraphicsDeviceException")
            {
                try { GraphicsStartupDiagnostic.Record(fault, Record); }
                catch (Exception) { Record("stardew_graphics_probe=incomplete"); }
            }
        };
        AppDomain.CurrentDomain.ProcessExit += (_, args) => Record("stardew_managed=process-exit");
    }

    private static void RecordFileAccess(Exception fault, string? local, string? package, Action<string> record)
    {
        bool Mentions(string? root) => !string.IsNullOrEmpty(root) &&
            fault.Message.IndexOf(root!, StringComparison.OrdinalIgnoreCase) >= 0;
        var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        record("stardew_data_roaming=" + (roaming.Length == 0 ? "empty" : "resolved"));
        var location = Mentions(AppContext.BaseDirectory) ? "game-directory" :
            Mentions(local) ? "app-local" : Mentions(package) ? "package" : Mentions(roaming) ? "roaming" : "unclassified";
        record("stardew_file_location=" + location);
        int remaining = 12;
        foreach (var frame in new StackTrace(fault).GetFrames() ?? Array.Empty<StackFrame>())
        {
            var method = frame.GetMethod();
            var type = method?.DeclaringType?.FullName;
            if (type != null && (type.StartsWith("StardewValley.", StringComparison.Ordinal) ||
                                 type.StartsWith("System.IO.", StringComparison.Ordinal) ||
                                 type.StartsWith("Microsoft.Win32.SafeHandles.", StringComparison.Ordinal)))
            {
                record("stardew_file_frame=" + type + "." + method!.Name);
                if (--remaining == 0) break;
            }
        }
    }
}
