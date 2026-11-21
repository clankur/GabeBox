import java.nio.file.Files;
import java.nio.file.Paths;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Label;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;

public final class BuildLwjglDisplayFallback implements Opcodes {
    private static final String DISPLAY = "org/lwjgl/opengl/WindowsDisplay";
    private static final String MODE = "org/lwjgl/opengl/DisplayMode";
    private static final String EXCEPTION = "org/lwjgl/LWJGLException";
    private static final String SIGNATURE = "()L" + MODE + ";";
    private static final String FALLBACK = "getCurrentDisplayModeForXboxTrial";

    public static void main(String[] args) throws Exception {
        ClassReader reader = new ClassReader(Files.readAllBytes(Paths.get(args[0])));
        if (!DISPLAY.equals(reader.getClassName())) throw new IllegalArgumentException("Expected the upstream WindowsDisplay class");
        boolean needsFrames = reader.readUnsignedShort(6) >= V1_6;
        ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
        final int[] replacements = {0};
        reader.accept(new ClassVisitor(ASM9, writer) {
            @Override
            public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
                MethodVisitor method = super.visitMethod(access, name, descriptor, signature, exceptions);
                if (!name.equals("init") || !descriptor.equals(SIGNATURE)) return method;
                return new MethodVisitor(ASM9, method) {
                    @Override
                    public void visitMethodInsn(int opcode, String owner, String name, String descriptor, boolean isInterface) {
                        if (opcode == INVOKESTATIC && owner.equals(DISPLAY) && name.equals("getCurrentDisplayMode") &&
                                descriptor.equals(SIGNATURE)) {
                            name = FALLBACK;
                            replacements[0]++;
                        }
                        super.visitMethodInsn(opcode, owner, name, descriptor, isInterface);
                    }
                };
            }
        }, 0);
        if (replacements[0] != 1) throw new IllegalStateException("Expected one native display-mode query in init");

        MethodVisitor method = writer.visitMethod(ACC_PRIVATE | ACC_STATIC, FALLBACK, SIGNATURE, null,
                                                  new String[]{EXCEPTION});
        Label start = new Label(), end = new Label(), handler = new Label(), windowed = new Label();
        method.visitCode();
        method.visitTryCatchBlock(start, end, handler, EXCEPTION);
        method.visitLabel(start);
        method.visitMethodInsn(INVOKESTATIC, DISPLAY, "getCurrentDisplayMode", SIGNATURE, false);
        method.visitLabel(end);
        method.visitInsn(ARETURN);

        method.visitLabel(handler);
        if (needsFrames) method.visitFrame(F_SAME1, 0, null, 1, new Object[]{EXCEPTION});
        method.visitInsn(DUP);
        method.visitMethodInsn(INVOKEVIRTUAL, EXCEPTION, "getMessage", "()Ljava/lang/String;", false);
        method.visitLdcInsn("Couldn't get current display settings (120)");
        method.visitInsn(SWAP);
        method.visitMethodInsn(INVOKEVIRTUAL, "java/lang/String", "equals", "(Ljava/lang/Object;)Z", false);
        method.visitJumpInsn(IFNE, windowed);
        method.visitInsn(ATHROW);

        method.visitLabel(windowed);
        if (needsFrames) method.visitFrame(F_SAME1, 0, null, 1, new Object[]{EXCEPTION});
        method.visitInsn(POP);
        method.visitFieldInsn(GETSTATIC, "java/lang/System", "out", "Ljava/io/PrintStream;");
        method.visitLdcInsn("slay_console_display=windowed-fallback");
        method.visitMethodInsn(INVOKEVIRTUAL, "java/io/PrintStream", "println", "(Ljava/lang/String;)V", false);
        method.visitTypeInsn(NEW, MODE);
        method.visitInsn(DUP);
        method.visitIntInsn(SIPUSH, 1280);
        method.visitIntInsn(SIPUSH, 720);
        method.visitMethodInsn(INVOKESPECIAL, MODE, "<init>", "(II)V", false);
        method.visitInsn(ARETURN);
        method.visitMaxs(0, 0);
        method.visitEnd();
        writer.visitEnd();
        Files.write(Paths.get(args[1]), writer.toByteArray());
    }
}
