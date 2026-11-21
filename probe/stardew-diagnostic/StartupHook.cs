using System;
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

        void Record(string entry)
        {
            try { File.AppendAllText(log, entry + "\n"); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        Record("stardew_managed=entered");
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
        AppDomain.CurrentDomain.UnhandledException += (_, args) => Record("stardew_managed=unhandled-exception");
        AppDomain.CurrentDomain.ProcessExit += (_, args) => Record("stardew_managed=process-exit");
    }
}
