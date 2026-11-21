#include <windows.h>
#include <jni.h>
#include <cstring>

#include "mesa-control.h"

extern "C" JNIEXPORT jintArray JNICALL Java_SlayConsoleDiagnostic_runMesaControl(JNIEnv *env, jclass) {
    const auto result = RunMesaControl();
    // The shared control checks each channel before it sets stage 8.
    const jint fields[] = {
        static_cast<jint>(result.stage), static_cast<jint>(result.loadError), result.eglError,
        static_cast<jint>(result.glError), static_cast<jint>(result.framebufferStatus)
    };
    auto report = env->NewIntArray(ARRAYSIZE(fields));
    if (report) env->SetIntArrayRegion(report, 0, ARRAYSIZE(fields), fields);
    return report;
}
