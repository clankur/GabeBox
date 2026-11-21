package org.lwjgl.opengl;

import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import org.lwjgl.LWJGLException;

final class XboxSdlContext implements ContextImplementation {
    private boolean presented;

    public ByteBuffer create(PeerInfo peer, IntBuffer attribs, ByteBuffer shared) throws LWJGLException {
        if (attribs != null || shared != null) throw new LWJGLException("This SDL profile requires one default GL context");
        ByteBuffer context = XboxSdlDisplay.openContext();
        if (context == null) throw new LWJGLException("Xbox SDL context creation failed");
        System.out.println("slay_sdl_display=context");
        return context;
    }

    public void swapBuffers() throws LWJGLException {
        if (!XboxSdlDisplay.present()) throw new LWJGLException("Xbox SDL presentation failed");
        if (!presented) {
            presented = true;
            System.out.println("slay_sdl_display=presented");
        }
    }
    public void releaseDrawable(ByteBuffer context) {}
    public void releaseCurrentContext() throws LWJGLException { makeCurrent(null, null); }
    public void update(ByteBuffer context) {}
    public void makeCurrent(PeerInfo peer, ByteBuffer context) throws LWJGLException {
        if (!XboxSdlDisplay.makeContextCurrent(context)) throw new LWJGLException("Xbox SDL could not bind the context");
    }
    public boolean isCurrent(ByteBuffer context) { return XboxSdlDisplay.contextIsCurrent(context); }
    public void setSwapInterval(int value) { XboxSdlDisplay.swapInterval(value); }
    public void destroy(PeerInfo peer, ByteBuffer context) { XboxSdlDisplay.closeContext(context); }
}
