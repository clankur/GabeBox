using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class GraphicsStartupDiagnostic
{
    [DllImport("SDL2.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr SDL_GetCurrentVideoDriver();

    [DllImport("SDL2.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr SDL_GetError();

    [DllImport("SDL2.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr SDL_GL_GetCurrentContext();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    private static extern IntPtr LoadLibraryExW(string filename, IntPtr reserved, uint flags);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern uint GetModuleFileNameW(IntPtr module, StringBuilder filename, uint length);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    private static extern IntPtr LoadPackagedLibrary(string filename, uint reserved);

    [DllImport("d3d11.dll", ExactSpelling = true)]
    private static extern int D3D11CreateDevice(IntPtr adapter, uint driverType, IntPtr software, uint flags,
        IntPtr featureLevels, uint featureLevelCount, uint sdkVersion, IntPtr device, out uint featureLevel,
        IntPtr immediateContext);

    public static void Record(Exception fault, Action<string> write)
    {
        foreach (var frame in new StackTrace(fault.InnerException ?? fault, false).GetFrames() ?? Array.Empty<StackFrame>())
        {
            var method = frame.GetMethod();
            var type = method?.DeclaringType?.FullName;
            if (type != null && (type.StartsWith("MonoGame.OpenGL.", StringComparison.Ordinal) ||
                                 type.StartsWith("Microsoft.Xna.Framework.Graphics.", StringComparison.Ordinal)))
            {
                write("stardew_graphics_frame=" + type + "." + method!.Name);
            }
        }

        try
        {
            var error = Marshal.PtrToStringAnsi(SDL_GetError()) ?? "";
            if (error.IndexOf("OpenGL", StringComparison.OrdinalIgnoreCase) >= 0)
                write("stardew_graphics_sdl_error=opengl");
            if (error.IndexOf("WGL", StringComparison.OrdinalIgnoreCase) >= 0)
                write("stardew_graphics_sdl_error=wgl");
            var driver = Marshal.PtrToStringAnsi(SDL_GetCurrentVideoDriver());
            write("stardew_graphics_sdl_driver=" + (driver switch
            {
                "windows" => "windows",
                "offscreen" => "offscreen",
                null => "none",
                _ => "other",
            }));
            write("stardew_graphics_gl_context=" + (SDL_GL_GetCurrentContext() == IntPtr.Zero ? "absent" : "present"));
        }
        catch (DllNotFoundException) { write("stardew_graphics_sdl_driver=unavailable"); }
        catch (EntryPointNotFoundException) { write("stardew_graphics_sdl_driver=unavailable"); }

        const uint SearchSystem32 = 0x800;
        var gl = LoadLibraryExW("opengl32.dll", IntPtr.Zero, SearchSystem32);
        if (gl != IntPtr.Zero)
        {
            write("stardew_graphics_system_opengl=loaded");
            var path = new StringBuilder(1024);
            uint length = GetModuleFileNameW(gl, path, (uint)path.Capacity);
            if (length > 0 && length < path.Capacity)
            {
                var directory = Path.GetDirectoryName(path.ToString());
                var appDirectory = Path.GetDirectoryName(typeof(GraphicsStartupDiagnostic).Assembly.Location);
                var source = string.Equals(directory, appDirectory, StringComparison.OrdinalIgnoreCase) ? "bundled" :
                    string.Equals(directory, Environment.SystemDirectory, StringComparison.OrdinalIgnoreCase) ? "system" : "other";
                write("stardew_graphics_opengl_origin=" + source);
            }
            FreeLibrary(gl);
        }
        else
        {
            write("stardew_graphics_system_opengl=0x" + Marshal.GetLastWin32Error().ToString("X8"));
        }

        var packagedGl = LoadPackagedLibrary("opengl32.dll", 0);
        if (packagedGl != IntPtr.Zero)
        {
            write("stardew_graphics_packaged_opengl=loaded");
            FreeLibrary(packagedGl);
        }
        else
        {
            write("stardew_graphics_packaged_opengl=0x" + Marshal.GetLastWin32Error().ToString("X8"));
        }

        try
        {
            const uint HardwareDriver = 1, D3D11SdkVersion = 7;
            int result = D3D11CreateDevice(IntPtr.Zero, HardwareDriver, IntPtr.Zero, 0, IntPtr.Zero, 0,
                D3D11SdkVersion, IntPtr.Zero, out uint level, IntPtr.Zero);
            write("stardew_graphics_d3d11_hresult=0x" + result.ToString("X8"));
            if (result >= 0) write("stardew_graphics_d3d11_feature_level=0x" + level.ToString("X4"));
        }
        catch (DllNotFoundException) { write("stardew_graphics_d3d11_hresult=unavailable"); }
        catch (EntryPointNotFoundException) { write("stardew_graphics_d3d11_hresult=unavailable"); }
    }
}
