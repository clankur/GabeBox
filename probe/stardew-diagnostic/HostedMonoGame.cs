using System;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Threading;

internal static class HostedMonoGame
{
    private const BindingFlags Members = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static;

    public static void Configure(string package, Action<string> record)
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("XBOX_SDL_HOST_MAILBOX")) ||
            !ReadDimension("XBOX_SDL_RENDER_WIDTH", 640, 1920, out int width) ||
            !ReadDimension("XBOX_SDL_RENDER_HEIGHT", 360, 1080, out int height)) return;

        StageAdapter(package, record);
        Assembly framework = Assembly.Load("MonoGame.Framework");
        // MonoGame's dispatcher records its UI thread on first use. This startup
        // hook runs on the managed application thread before the game entry point.
        Type threading = framework.GetType("Microsoft.Xna.Framework.Threading", true)!;
        RuntimeHelpers.RunClassConstructor(threading.TypeHandle);
        var dispatch = threading.GetMethod("BlockOnUIThread", Members, null, new[] { typeof(Action) }, null)!;
        new Thread(() => {
            try
            {
                var type = framework.GetType("Microsoft.Xna.Framework.Game", true)!;
                bool finished = false;
                for (int i = 0; i < 120 && !finished; ++i)
                {
                    Thread.Sleep(500);
                    dispatch.Invoke(null, new object[] { (Action)(() => {
                        var game = type.GetProperty("Instance", Members)!.GetValue(null);
                        if (game == null || !(bool)type.GetProperty("Initialized", Members)!.GetValue(game)!) return;
                        var window = type.GetProperty("Window", Members)!.GetValue(game)!;
                        finished = true;
                        if (!(bool)window.GetType().GetProperty("AllowUserResizing", Members)!.GetValue(window)!) return;
                        var manager = type.GetProperty("graphicsDeviceManager", Members)!.GetValue(game)!;
                        var settings = manager.GetType();
                        settings.GetProperty("PreferredBackBufferWidth")!.SetValue(manager, width);
                        settings.GetProperty("PreferredBackBufferHeight")!.SetValue(manager, height);
                        settings.GetMethod("ApplyChanges", Type.EmptyTypes)!.Invoke(manager, null);
                        record("stardew_shared_render_size=" + width + "x" + height);
                    })});
                }
            }
            catch (Exception error) { record("stardew_shared_resize=" + error.GetBaseException().GetType().Name); }
        }) { IsBackground = true }.Start();
    }

    private static void StageAdapter(string package, Action<string> record)
    {
        string source = Path.Combine(package, "SDL2.dll");
        if (!File.Exists(source))
        {
            record("stardew_shared_sdl=installed-fallback");
            return;
        }
        // MonoGame checks the runtime-specific directory before its installed root.
        // Keep that publisher file intact. Only update an adapter this host owns.
        string native = Path.Combine(AppContext.BaseDirectory, "runtimes", "win-x64", "native");
        string candidate = Path.Combine(native, "SDL2.dll");
        string marker = Path.Combine(native, "xbox-host-sdl.sha256");
        try {
            string expected = Digest(source);
            Directory.CreateDirectory(native);
            string existing = File.Exists(candidate) ? Digest(candidate) : "";
            if (existing != expected) {
                string[] owned = File.Exists(marker) ? File.ReadAllLines(marker) : Array.Empty<string>();
                if (existing.Length != 0 && Array.IndexOf(owned, existing) < 0) {
                    record("stardew_shared_sdl=existing-game-adapter");
                } else {
                    string pending = Path.Combine(native, Guid.NewGuid().ToString("N") + ".tmp");
                    try {
                        File.Copy(source, pending);
                        if (Digest(pending) != expected) throw new IOException("SDL copy verification failed");
                        File.WriteAllLines(marker, new[] { existing, expected });
                        if (existing.Length == 0) File.Move(pending, candidate);
                        else File.Replace(pending, candidate, null);
                    } finally { if (File.Exists(pending)) File.Delete(pending); }
                    record("stardew_shared_sdl=side-by-side");
                }
            } else record("stardew_shared_sdl=ready");
        } catch (Exception error) when (error is IOException || error is UnauthorizedAccessException) {
            record("stardew_shared_sdl=installed-fallback");
        }
    }

    private static string Digest(string filename)
    {
        using var file = File.OpenRead(filename);
        using var hash = SHA256.Create();
        return BitConverter.ToString(hash.ComputeHash(file)).Replace("-", "");
    }

    private static bool ReadDimension(string name, int min, int max, out int value) =>
        int.TryParse(Environment.GetEnvironmentVariable(name), NumberStyles.None, CultureInfo.InvariantCulture, out value) &&
        value >= min && value <= max;
}
