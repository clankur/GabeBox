package com.badlogic.gdx.graphics;

import com.badlogic.gdx.Gdx;
import com.badlogic.gdx.files.FileHandle;
import com.badlogic.gdx.utils.GdxRuntimeException;
import java.awt.image.BufferedImage;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.util.Iterator;
import java.util.regex.Pattern;
import javax.imageio.ImageIO;
import javax.imageio.ImageReadParam;
import javax.imageio.ImageReader;
import javax.imageio.stream.MemoryCacheImageInputStream;

/** Upload half-resolution scene pages while keeping the atlas's original coordinate system. */
public final class XboxSceneTexture implements TextureData {
    private static final Pattern PAGE = Pattern.compile(
            "(?:title/title[0-9]*|(?:bottom|city|beyond|ending)Scene/scene[0-9]*)\\.(?:jpg|png)");
    private final FileHandle file;
    private int width, height;
    private BufferedImage image;
    // Shared only during the synchronous GL upload. Avoid a pending native buffer per page.
    private static ByteBuffer uploadBuffer;

    private XboxSceneTexture(FileHandle file) { this.file = file; }

    public static TextureData forFile(FileHandle file, Pixmap.Format format, boolean mipmaps) {
        if (file == null || mipmaps || (format != null && format != Pixmap.Format.RGBA8888)
                || System.getProperty("xbox.lwjgl.sdl.package") == null || !PAGE.matcher(file.path()).matches()) return null;
        return new XboxSceneTexture(file);
    }

    public void prepare() {
        if (isPrepared()) throw new GdxRuntimeException("Scene page is already prepared");
        try (InputStream bytes = file.read(); MemoryCacheImageInputStream stream = new MemoryCacheImageInputStream(bytes)) {
            Iterator<ImageReader> readers = ImageIO.getImageReaders(stream);
            if (!readers.hasNext()) throw new IOException("Unsupported image format");
            ImageReader reader = readers.next();
            try {
                reader.setInput(stream, true, true);
                width = reader.getWidth(0);
                height = reader.getHeight(0);
                ImageReadParam options = reader.getDefaultReadParam();
                options.setSourceSubsampling(2, 2, 0, 0);
                image = reader.read(0, options);
            } finally {
                reader.dispose();
            }
        } catch (IOException error) {
            throw new GdxRuntimeException("Could not read Xbox scene page " + file.path(), error);
        }
    }

    public void consumeCustomData(int target) {
        if (!isPrepared()) throw new GdxRuntimeException("Scene page is not prepared");
        try {
            upload(target, image);
        } finally {
            image.flush();
            image = null;
        }
    }

    private static synchronized void upload(int target, BufferedImage image) {
        int w = image.getWidth(), h = image.getHeight();
        int bytes = Math.multiplyExact(Math.multiplyExact(w, h), 4);
        if (uploadBuffer == null || uploadBuffer.capacity() < bytes) uploadBuffer = ByteBuffer.allocateDirect(bytes);
        uploadBuffer.clear();
        uploadBuffer.limit(bytes);
        // BufferedImage exposes ARGB integers. The GL buffer stores RGBA bytes in big-endian order.
        IntBuffer pixels = uploadBuffer.asIntBuffer();
        int[] row = new int[w];
        for (int y = 0; y < h; y++) {
            image.getRGB(0, y, w, 1, row, 0, w);
            for (int x = 0; x < w; x++) row[x] = Integer.rotateLeft(row[x], 8);
            pixels.put(row);
        }
        Gdx.gl.glPixelStorei(GL20.GL_UNPACK_ALIGNMENT, 1);
        Gdx.gl.glTexImage2D(target, 0, GL20.GL_RGBA, w, h, 0, GL20.GL_RGBA, GL20.GL_UNSIGNED_BYTE, uploadBuffer);
    }

    public boolean isPrepared() { return image != null; }
    public TextureDataType getType() { return TextureDataType.Custom; }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public Pixmap.Format getFormat() { return Pixmap.Format.RGBA8888; }
    public boolean isManaged() { return true; }
    public boolean useMipMaps() { return false; }
    public boolean disposePixmap() { return false; }
    public Pixmap consumePixmap() { throw new GdxRuntimeException("Scene pages use a custom upload"); }
}
