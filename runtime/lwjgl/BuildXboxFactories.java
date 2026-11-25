import java.nio.file.Files;
import java.nio.file.Path;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Label;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;

public final class BuildXboxFactories implements Opcodes {
    public static void main(String[] args) throws Exception {
        ClassReader reader = new ClassReader(Files.readAllBytes(Path.of(args[0])));
        boolean frames = reader.readUnsignedShort(6) >= V1_6;
        String prefix = "org/lwjgl/opengl/";
        String factory, type, provider;
        switch (reader.getClassName()) {
            case "com/badlogic/gdx/graphics/TextureData$Factory":
                transformTextureFactory(reader, Path.of(args[1]), frames);
                return;
            case "org/lwjgl/opengl/Display":
                factory = "createDisplayImplementation";
                type = "DisplayImplementation";
                provider = "XboxSdlDisplay";
                break;
            case "org/lwjgl/opengl/ContextGL":
                factory = "createImplementation";
                type = "ContextImplementation";
                provider = "XboxSdlContext";
                break;
            case "org/lwjgl/opengl/GLContext":
                transformGlResolver(reader, Path.of(args[1]), prefix, frames);
                return;
            default: throw new IllegalArgumentException("Unexpected LWJGL class.");
        }
        ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
        int[] changes = { 0 };
        reader.accept(new ClassVisitor(ASM9, writer) {
            @Override
            public MethodVisitor visitMethod(int access, String name, String desc, String signature, String[] exceptions) {
                MethodVisitor base = super.visitMethod(access, name, desc, signature, exceptions);
                if (type.equals("DisplayImplementation") && name.equals("sync") && desc.equals("(I)V")) {
                    return new MethodVisitor(ASM9, base) {
                        @Override public void visitCode() {
                            super.visitCode();
                            super.visitVarInsn(ILOAD, 0);
                            super.visitMethodInsn(INVOKESTATIC, prefix + "XboxRenderProfile", "producerFps", "(I)I", false);
                            super.visitVarInsn(ISTORE, 0);
                        }
                    };
                }
                if (!factory.equals(name) || !desc.equals("()L" + prefix + type + ";")) return base;
                ++changes[0];
                return new MethodVisitor(ASM9, base) {
                    @Override
                    public void visitCode() {
                        super.visitCode();
                        Label original = new Label();
                        super.visitMethodInsn(INVOKESTATIC, prefix + "XboxSdlDisplay", "enabled", "()Z", false);
                        super.visitJumpInsn(IFEQ, original);
                        super.visitTypeInsn(NEW, prefix + provider);
                        super.visitInsn(DUP);
                        super.visitMethodInsn(INVOKESPECIAL, prefix + provider, "<init>", "()V", false);
                        super.visitInsn(ARETURN);
                        super.visitLabel(original);
                        if (frames) super.visitFrame(F_SAME, 0, null, 0, null);
                    }
                };
            }
        }, 0);
        if (changes[0] != 1) throw new IllegalStateException("Expected exactly one platform factory.");
        Files.write(Path.of(args[1]), writer.toByteArray());
    }

    private static void transformTextureFactory(ClassReader reader, Path output, boolean frames) throws Exception {
        String textureData = "com/badlogic/gdx/graphics/TextureData";
        String signature = "(Lcom/badlogic/gdx/files/FileHandle;Lcom/badlogic/gdx/graphics/Pixmap$Format;Z)L" + textureData + ";";
        ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
        int[] changes = { 0 };
        reader.accept(new ClassVisitor(ASM9, writer) {
            @Override
            public MethodVisitor visitMethod(int access, String name, String desc, String generic, String[] exceptions) {
                MethodVisitor base = super.visitMethod(access, name, desc, generic, exceptions);
                if (!name.equals("loadFromFile") || !desc.equals(signature)) return base;
                ++changes[0];
                return new MethodVisitor(ASM9, base) {
                    @Override public void visitCode() {
                        super.visitCode();
                        Label original = new Label();
                        super.visitVarInsn(ALOAD, 0);
                        super.visitVarInsn(ALOAD, 1);
                        super.visitVarInsn(ILOAD, 2);
                        super.visitMethodInsn(INVOKESTATIC, "com/badlogic/gdx/graphics/XboxSceneTexture", "forFile", desc, false);
                        super.visitInsn(DUP);
                        super.visitJumpInsn(IFNULL, original);
                        super.visitInsn(ARETURN);
                        super.visitLabel(original);
                        if (frames) super.visitFrame(F_SAME1, 0, null, 1, new Object[] { textureData });
                        super.visitInsn(POP);
                    }
                };
            }
        }, 0);
        if (changes[0] != 1) throw new IllegalStateException("Expected exactly one texture data factory.");
        Files.write(output, writer.toByteArray());
    }

    private static void transformGlResolver(ClassReader reader, Path output, String prefix, boolean frames) throws Exception {
        ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
        int[] changes = { 0 };
        reader.accept(new ClassVisitor(ASM9, writer) {
            @Override
            public MethodVisitor visitMethod(int access, String name, String desc, String signature, String[] exceptions) {
                MethodVisitor base = super.visitMethod(access, name, desc, signature, exceptions);
                if (!name.equals("getFunctionAddress") || !desc.equals("(Ljava/lang/String;)J")) return base;
                ++changes[0];
                return new MethodVisitor(ASM9, base) {
                    @Override
                    public void visitCode() {
                        super.visitCode();
                        Label original = new Label();
                        super.visitVarInsn(ALOAD, 0);
                        super.visitMethodInsn(INVOKESTATIC, prefix + "XboxSdlDisplay", "glAddress", desc, false);
                        super.visitInsn(DUP2);
                        super.visitInsn(LCONST_0);
                        super.visitInsn(LCMP);
                        super.visitJumpInsn(IFEQ, original);
                        super.visitInsn(LRETURN);
                        super.visitLabel(original);
                        if (frames) super.visitFrame(F_SAME1, 0, null, 1, new Object[] { LONG });
                        super.visitInsn(POP2);
                    }
                };
            }
        }, 0);
        if (changes[0] != 1) throw new IllegalStateException("Expected exactly one GL function resolver.");
        Files.write(output, writer.toByteArray());
    }
}
