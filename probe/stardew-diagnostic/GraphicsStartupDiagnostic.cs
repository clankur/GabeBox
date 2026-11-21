using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

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
            write("stardew_graphics_sdl_driver=" + (driver == "windows" ? "windows" : driver == null ? "none" : "other"));
            write("stardew_graphics_gl_context=" + (SDL_GL_GetCurrentContext() == IntPtr.Zero ? "absent" : "present"));
        }
        catch (DllNotFoundException) { write("stardew_graphics_sdl_driver=unavailable"); }
        catch (EntryPointNotFoundException) { write("stardew_graphics_sdl_driver=unavailable"); }

        const uint SearchSystem32 = 0x800;
        var gl = LoadLibraryExW("opengl32.dll", IntPtr.Zero, SearchSystem32);
        if (gl != IntPtr.Zero)
        {
            write("stardew_graphics_system_opengl=loaded");
            FreeLibrary(gl);
        }
        else
        {
            write("stardew_graphics_system_opengl=0x" + Marshal.GetLastWin32Error().ToString("X8"));
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
