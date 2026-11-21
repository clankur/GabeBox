import com.badlogic.gdx.Gdx;
import com.badlogic.gdx.files.FileHandle;
import com.badlogic.gdx.graphics.GL20;
import com.badlogic.gdx.graphics.Pixmap;
import com.badlogic.gdx.graphics.TextureData;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.reflect.Proxy;
import java.nio.ByteBuffer;
import javax.imageio.ImageIO;

/** Run with the built SDL adapter followed by libGDX 1.9.5 on the classpath. */
public final class SlaySceneTextureTest {
    private static byte[] encode(String format) throws Exception {
        BufferedImage src = new BufferedImage(6, 4,
                format.equals("png") ? BufferedImage.TYPE_INT_ARGB : BufferedImage.TYPE_INT_RGB);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 6; x++) src.setRGB(x, y, 0x7f336699);
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        if (!ImageIO.write(src, format, bytes)) throw new AssertionError("No encoder for " + format);
        return bytes.toByteArray();
    }

    private static void check(boolean ok) { if (!ok) throw new AssertionError(); }

    public static void main(String[] args) throws Exception {
        System.setProperty("xbox.lwjgl.sdl.package", "test");
        for (String file : new String[] {"title/title.jpg", "bottomScene/scene.png"}) {
            boolean png = file.endsWith("png");
            byte[] bytes = encode(png ? "png" : "jpg");
            FileHandle handle = new FileHandle(file) {
                @Override public InputStream read() { return new ByteArrayInputStream(bytes); }
            };
            int[] uploads = {0};
            Gdx.gl = (GL20)Proxy.newProxyInstance(GL20.class.getClassLoader(), new Class<?>[] { GL20.class },
                    (proxy, method, parameters) -> {
                        if (method.getName().equals("glTexImage2D")) {
                            check((int)parameters[3] == 3 && (int)parameters[4] == 2);
                            ByteBuffer pixels = (ByteBuffer)parameters[8];
                            check(pixels.remaining() == 24 && pixels.isDirect());
                            for (int i = 0; i < 24; i += 4) {
                                check(Math.abs((pixels.get(i) & 255) - 0x33) <= 2);
                                check(Math.abs((pixels.get(i + 1) & 255) - 0x66) <= 2);
                                check(Math.abs((pixels.get(i + 2) & 255) - 0x99) <= 2);
                                check((pixels.get(i + 3) & 255) == (png ? 0x7f : 255));
                            }
                            uploads[0]++;
                        }
                        return null;
                    });
            TextureData data = TextureData.Factory.loadFromFile(handle, Pixmap.Format.RGBA8888, false);
            for (int pass = 0; pass < 2; pass++) {
                data.prepare();
                check(data.isManaged() && data.getWidth() == 6 && data.getHeight() == 4);
                data.consumeCustomData(GL20.GL_TEXTURE_2D);
                check(!data.isPrepared());
            }
            check(uploads[0] == 2);
        }
        check(TextureData.Factory.loadFromFile(null, false) == null);
    }
}
