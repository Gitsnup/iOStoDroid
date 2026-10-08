#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/cpu.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/runner.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/sjlj_unwind.hpp"
#include "compat_runtime/runtime_contract.hpp"

#include "json.hpp"

#include <jni.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr jsize kMaximumMainBinaryBytes = 256 * 1024 * 1024;

jstring jsonString(JNIEnv *env, const iostodroid::Json &json) {
    const std::string text = json.dump();
    return env->NewStringUTF(text.c_str());
}

iostodroid::Json blockedReport(const std::string &message, bool authorizationConfirmed) {
    iostodroid::Json report = iostodroid::Json::object();
    report["schemaVersion"] = std::uint64_t{1};
    report["runtimeContract"] = iostodroid::compat_runtime::kRuntimeContract;
    report["runtimeLibrary"] = iostodroid::compat_runtime::kRuntimeLibraryName;
    report["reportArtifactName"] = iostodroid::compat_runtime::kRuntimeReportFileName;
    report["status"] = "not_runnable";
    report["authorizationConfirmed"] = authorizationConfirmed;
    report["firstMissingImport"] = iostodroid::Json();
    report["resolvedSymbols"] = iostodroid::Json::array();
    report["unresolvedSymbols"] = iostodroid::Json::array();
    report["loader"] = iostodroid::Json::object();
    report["loader"]["status"] = "NOT_ATTEMPTED";
    report["loader"]["imageMapped"] = false;
    report["cpu"] = iostodroid::Json::object();
    report["cpu"]["status"] = "NOT_ATTEMPTED";
    report["execution"] = iostodroid::Json::object();
    report["execution"]["status"] = "NOT_ATTEMPTED";
    report["execution"]["entryPointReached"] = false;
    report["reason"] = message;
    report["message"] = message;
    report["inputEmbeddedInRuntimeArtifact"] = false;
    return report;
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_iostodroid_compat_runtime_RuntimeBridge_runAuthorizedMainBinary(JNIEnv *env, jobject,
                                                                     jbyteArray mainBinary,
                                                                     jboolean authorizationConfirmed) {
    if (authorizationConfirmed != JNI_TRUE)
        return jsonString(env, blockedReport("User authorization was not confirmed.", false));
    if (!mainBinary)
        return jsonString(env, blockedReport("The IPA main executable is missing.", true));
    const jsize length = env->GetArrayLength(mainBinary);
    if (length <= 0 || length > kMaximumMainBinaryBytes)
        return jsonString(env, blockedReport("The IPA main executable exceeds the runtime input limit.", true));

    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        env->GetByteArrayRegion(mainBinary, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
        if (env->ExceptionCheck())
            return nullptr;
        iostodroid::compat_runtime::ShimRegistry shims;
        iostodroid::compat_runtime::objc::ShimAdapter objcShims;
        iostodroid::compat_runtime::audio::ShimAdapter audioShims;
        iostodroid::compat_runtime::SjLjUnwindAdapter sjljUnwind;
        objcShims.registerBindings(shims);
        audioShims.registerBindings(shims);
        sjljUnwind.registerBindings(shims);
        const auto cpu = iostodroid::compat_runtime::createArm32CpuBackend();
        const iostodroid::compat_runtime::GuestRunner runner(shims, *cpu);
        return jsonString(env, runner.runMainBinary(bytes, true));
    } catch (const std::exception &error) {
        const std::string detail = std::string("Runtime initialization failed closed: ") + error.what();
        return jsonString(env, blockedReport(detail, true));
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_iostodroid_compat_runtime_RuntimeBridge_reportImportBlocked(JNIEnv *env, jobject,
                                                                jstring message,
                                                                jboolean authorizationConfirmed) {
    if (!message)
        return jsonString(env, blockedReport("IPA import failed closed.", authorizationConfirmed == JNI_TRUE));
    const char *characters = env->GetStringUTFChars(message, nullptr);
    if (!characters)
        return nullptr;
    const std::string detail(characters);
    env->ReleaseStringUTFChars(message, characters);
    return jsonString(env, blockedReport(detail, authorizationConfirmed == JNI_TRUE));
}
