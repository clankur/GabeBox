#include <windows.h>
#include <jni.h>
#include <cstring>

#include "mesa-control.h"

extern "C" JNIEXPORT jintArray JNICALL Java_SlayConsoleDiagnostic_runMesaControl(JNIEnv *env, jclass type) {
    const auto bindingCheck = env->GetStaticMethodID(type, "checkLwjglBinding", "()V");
    if (!bindingCheck) return nullptr;
    const auto result = RunMesaControl([](unsigned stage) {
        char marker[] = "slay_mesa_child_checkpoint=0\n";
        marker[sizeof(marker) - 3] += static_cast<char>(stage);
        DWORD written;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), marker, sizeof(marker) - 1, &written, nullptr);
    }, [&] {
        env->CallStaticVoidMethod(type, bindingCheck);
    });
    if (env->ExceptionCheck()) return nullptr;
    // The shared control checks each channel before it sets stage 8.
    const jint fields[] = {
        static_cast<jint>(result.stage), static_cast<jint>(result.loadError), result.eglError,
        static_cast<jint>(result.glError), static_cast<jint>(result.framebufferStatus)
    };
    auto report = env->NewIntArray(ARRAYSIZE(fields));
    if (report) env->SetIntArrayRegion(report, 0, ARRAYSIZE(fields), fields);
    return report;
}
