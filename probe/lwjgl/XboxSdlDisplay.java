package org.lwjgl.opengl;

import java.awt.Canvas;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;
import org.lwjgl.LWJGLException;
import org.lwjgl.input.Cursor;
import org.lwjgl.input.Mouse;

/** SDL-backed display for the opt-in Xbox launch profile. */
final class XboxSdlDisplay implements DisplayImplementation {
    static final String PACKAGE = "xbox.lwjgl.sdl.package";
    // The hosted display has one virtual fullscreen mode. libGDX leaves null
    // entries when a backend advertises windowed-only modes in this list.
    private static final DisplayMode INITIAL = new DisplayMode(1280, 720, 32, 60);
    private static final int MAX_CURSOR_SIZE = 256;
    private static int width = INITIAL.getWidth();
    private static int height = INITIAL.getHeight();
    private static boolean nativeReady;
    private static boolean visible;
    private static boolean closeRequested;

    static boolean enabled() {
        return System.getProperty(PACKAGE) != null;
    }

    static native boolean open(String packageRoot);
    private static native long lookupGl(String name);
    private static native boolean create(int width, int height);
    private static native void destroy();
    private static native boolean pollClose();
    private static native int mouse(ByteBuffer coordinates);
    private static native int readMouseEvents(IntBuffer events, int count);
    private static native ByteBuffer makeCursor(int width, int height, int hotX, int hotY, IntBuffer pixels);
    private static native void selectCursor(ByteBuffer handle);
    private static native void releaseCursor(ByteBuffer handle);
    private static native void warpCursor(int x, int y);
    static native ByteBuffer openContext();
    static native boolean makeContextCurrent(ByteBuffer context);
    static native boolean contextIsCurrent(ByteBuffer context);
    static native void closeContext(ByteBuffer context);
    static native boolean present();
    static native void swapInterval(int value);

    static long glAddress(String name) {
        if (!nativeReady) return 0;
        switch (name) {
            case "glBindFramebuffer":
            case "glBindFramebufferEXT":
            case "glViewport":
            case "glClear":
            case "glTexImage2D":
            case "glCompileShader":
            case "glLinkProgram":
            case "glDrawElements":
            case "glDrawArrays":
                return lookupGl(name);
            default:
                return 0;
        }
    }

    public DisplayMode init() throws LWJGLException {
        File root = new File(System.getProperty(PACKAGE));
        System.load(new File(root, "slay-sdl-display.dll").getAbsolutePath());
        if (!open(root.getAbsolutePath())) throw new LWJGLException("Xbox SDL runtime initialization failed");
        nativeReady = true;
        System.out.println("slay_sdl_display=ready");
        return INITIAL;
    }

    public void createWindow(DrawableLWJGL drawable, DisplayMode mode, Canvas parent, int x, int y)
            throws LWJGLException {
        if (parent != null || !create(mode.getWidth(), mode.getHeight())) {
            throw new LWJGLException("Xbox SDL window creation failed");
        }
        width = mode.getWidth();
        height = mode.getHeight();
        closeRequested = false;
        visible = true;
        System.out.println("slay_sdl_display=window");
    }

    public void destroyWindow() { visible = false; destroy(); }
    public void switchDisplayMode(DisplayMode mode) {}
    public void resetDisplayMode() {}
    public int getGammaRampLength() { return 0; }
    public void setGammaRamp(FloatBuffer ramp) throws LWJGLException { throw new LWJGLException("Gamma control is unavailable"); }
    public String getAdapter() { return "SDL"; }
    public String getVersion() { return null; }
    public void setTitle(String title) {}
    public boolean isCloseRequested() { return closeRequested; }
    public boolean isVisible() { return visible; }
    public boolean isActive() { return visible; }
    public boolean isDirty() { return false; }
    public void update() { if (visible && pollClose()) closeRequested = true; }
    public void reshape(int x, int y, int w, int h) {}
    public DisplayMode[] getAvailableDisplayModes() { return new DisplayMode[] { INITIAL }; }
    public int getPbufferCapabilities() { return 0; }
    public boolean isBufferLost(PeerInfo handle) { return false; }
    public PeerInfo createPbuffer(int width, int height, PixelFormat format, ContextAttribs attribs,
            IntBuffer caps, IntBuffer attrs) throws LWJGLException {
        throw new LWJGLException("Xbox SDL Pbuffers are unavailable");
    }
    public void setPbufferAttrib(PeerInfo handle, int attrib, int value) { throw new UnsupportedOperationException(); }
    public void bindTexImageToPbuffer(PeerInfo handle, int buffer) { throw new UnsupportedOperationException(); }
    public void releaseTexImageFromPbuffer(PeerInfo handle, int buffer) { throw new UnsupportedOperationException(); }
    public int setIcon(ByteBuffer[] icons) { return 0; }
    public void setResizable(boolean resizable) {}
    public boolean wasResized() { return false; }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getX() { return 0; }
    public int getY() { return 0; }
    public float getPixelScaleFactor() { return 1; }

    public PeerInfo createPeerInfo(PixelFormat format, ContextAttribs attribs) {
        return new PeerInfo(ByteBuffer.allocateDirect(1)) {
            protected void doLockAndInitHandle() {}
            protected void doUnlock() {}
        };
    }

    private final ByteBuffer mouseState = ByteBuffer.allocateDirect(8).order(java.nio.ByteOrder.nativeOrder());
    private final IntBuffer mouseEvents = ByteBuffer.allocateDirect(32 * 5 * Integer.BYTES)
            .order(java.nio.ByteOrder.nativeOrder()).asIntBuffer();

    public boolean hasWheel() { return false; }
    public int getButtonCount() { return 3; }
    public void createMouse() {}
    public void destroyMouse() {}
    public void pollMouse(IntBuffer points, ByteBuffer buttons) {
        int mask = mouse(mouseState);
        points.put(0, mouseState.getInt(0));
        points.put(1, height - 1 - mouseState.getInt(4));
        points.put(2, 0);
        buttons.put(0, (byte)((mask & 1) != 0 ? 1 : 0));
        buttons.put(1, (byte)((mask & 4) != 0 ? 1 : 0));
        buttons.put(2, (byte)((mask & 2) != 0 ? 1 : 0));
    }
    public void readMouse(ByteBuffer buffer) {
        int capacity = Math.min(buffer.remaining() / Mouse.EVENT_SIZE, mouseEvents.capacity() / 5);
        int count = readMouseEvents(mouseEvents, capacity);
        for (int event = 0; event < count; ++event) {
            int offset = event * 5;
            buffer.put((byte)mouseEvents.get(offset));
            buffer.put((byte)mouseEvents.get(offset + 1));
            buffer.putInt(mouseEvents.get(offset + 2));
            buffer.putInt(height - 1 - mouseEvents.get(offset + 3));
            buffer.putInt(0);
            buffer.putLong(Integer.toUnsignedLong(mouseEvents.get(offset + 4)) * 1_000_000L);
        }
    }
    public void grabMouse(boolean grab) {}
    public int getNativeCursorCapabilities() { return Cursor.CURSOR_ONE_BIT_TRANSPARENCY; }
    public void setCursorPosition(int x, int y) { warpCursor(x, height - 1 - y); }
    public void setNativeCursor(Object handle) { selectCursor((ByteBuffer)handle); }
    public int getMinCursorSize() { return 1; }
    public int getMaxCursorSize() { return MAX_CURSOR_SIZE; }
    public void createKeyboard() {}
    public void destroyKeyboard() {}
    public void pollKeyboard(ByteBuffer buffer) {
        for (int key = 0; key < buffer.capacity(); ++key) buffer.put(key, (byte)0);
    }
    public void readKeyboard(ByteBuffer buffer) {}
    public Object createCursor(int w, int h, int x, int y, int count, IntBuffer images, IntBuffer delays)
            throws LWJGLException {
        if (count != 1 || w <= 0 || h <= 0 || w > MAX_CURSOR_SIZE || h > MAX_CURSOR_SIZE || images.remaining() < w * h) {
            throw new LWJGLException("Invalid Xbox SDL cursor image");
        }
        ByteBuffer handle = makeCursor(w, h, x, y, images.slice());
        if (handle == null) throw new LWJGLException("Xbox SDL could not create the color cursor");
        System.out.println("slay_sdl_display=cursor");
        return handle;
    }
    public void destroyCursor(Object handle) { releaseCursor((ByteBuffer)handle); }
    public boolean isInsideWindow() { return visible; }
}
