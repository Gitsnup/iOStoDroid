#include "macho.hpp"
#include <jni.h>
extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_analyze(JNIEnv *env, jobject,
                                                                                   jbyteArray input) {
    try {
        auto n = env->GetArrayLength(input);
        if (n > 64 * 1024 * 1024)
            throw std::runtime_error("executable exceeds on-device 64 MiB limit");
        std::vector<uint8_t> b(n);
        env->GetByteArrayRegion(input, 0, n, reinterpret_cast<jbyte *>(b.data()));
        if (env->ExceptionCheck())
            return nullptr;
        return env->NewStringUTF(radek::analyze(b).dump().c_str());
    } catch (const std::exception &e) {
        env->ThrowNew(env->FindClass("java/io/IOException"), e.what());
        return nullptr;
    }
}
