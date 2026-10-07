// JNI entry for game-runtime boot-attempt APKs (contract "game-runtime-v1").
//
// The APK embeds one authorized IPA main executable plus the game bundle. The
// launcher reads the executable from its own assets and calls here once: the
// runtime maps the image, binds unimplemented imports to abort-on-call traps,
// and executes real guest instructions until the first actually-used missing
// import. The returned JSON report keeps status "not_runnable" (a boot
// attempt is never gameplay evidence); the launcher keeps the stopping import
// visible in its diagnostic screen without pretending to be playable.
#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/compiler_rt_shims.hpp"
#include "compat_runtime/gles_shims.hpp"
#include "compat_runtime/cpu.hpp"
#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/runner.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/sjlj_unwind.hpp"
#include "compat_runtime/trap_shims.hpp"
#include "compat_runtime/runtime_contract.hpp"

#include "json.hpp"

#include <jni.h>

#include <android/native_window_jni.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {
constexpr jsize kMaximumMainBinaryBytes = 256 * 1024 * 1024;

// The native window acquired from the launcher's Surface. It is kept alive for
// as long as the guest may create or swap an EGL surface; a replacement window
// releases the previous one.
ANativeWindow *gAttachedWindow = nullptr;

void attachSurface(JNIEnv *env, jobject surface) {
    ANativeWindow *window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (window == gAttachedWindow) {
        if (window != nullptr)
            ANativeWindow_release(window);
        return;
    }
    radek::compat_runtime::gles::setDefaultNativeWindow(window);
    if (gAttachedWindow != nullptr)
        ANativeWindow_release(gAttachedWindow);
    gAttachedWindow = window;
}

jstring jsonString(JNIEnv *env, const radek::Json &json) {
    const std::string text = json.dump();
    return env->NewStringUTF(text.c_str());
}

radek::Json blockedReport(const std::string &message, bool authorizationConfirmed) {
    radek::Json report = radek::Json::object();
    report["schemaVersion"] = std::uint64_t{1};
    report["runtimeContract"] = radek::compat_runtime::kRuntimeContract;
    report["runtimeLibrary"] = radek::compat_runtime::kRuntimeLibraryName;
    report["reportArtifactName"] = radek::compat_runtime::kRuntimeReportFileName;
    report["status"] = "not_runnable";
    report["trapMode"] = true;
    report["authorizationConfirmed"] = authorizationConfirmed;
    report["firstMissingImport"] = radek::Json();
    report["trappedImport"] = radek::Json();
    report["resolvedSymbols"] = radek::Json::array();
    report["unresolvedSymbols"] = radek::Json::array();
    report["trappedSymbols"] = radek::Json::array();
    report["unboundNlistSymbols"] = radek::Json::array();
    report["loader"] = radek::Json::object();
    report["loader"]["status"] = "NOT_ATTEMPTED";
    report["loader"]["imageMapped"] = false;
    report["cpu"] = radek::Json::object();
    report["cpu"]["status"] = "NOT_ATTEMPTED";
    report["execution"] = radek::Json::object();
    report["execution"]["status"] = "NOT_ATTEMPTED";
    report["execution"]["entryPointReached"] = false;
    report["reason"] = message;
    report["message"] = message;
    report["inputEmbeddedInRuntimeArtifact"] = true;
    return report;
}
} // namespace

// The launcher hands its SurfaceView's surface over before (or while) the boot
// attempt runs, so the guest's EAGL drawable can present through EGL on screen.
extern "C" JNIEXPORT void JNICALL
Java_dev_radek_gameruntime_GameBootActivity_setGameSurface(JNIEnv *env, jclass, jobject surface) {
    try {
        attachSurface(env, surface);
    } catch (...) {
        // A surface the runtime cannot use never fails the boot attempt; the
        // GL layer reports the missing drawable through its own diagnostics.
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt(JNIEnv *env, jobject,
                                                               jbyteArray mainBinary,
                                                               jboolean authorizationConfirmed) {
    if (authorizationConfirmed != JNI_TRUE)
        return jsonString(env, blockedReport("User authorization was not confirmed.", false));
    if (!mainBinary)
        return jsonString(env, blockedReport("The embedded game executable is missing.", true));
    const jsize length = env->GetArrayLength(mainBinary);
    if (length <= 0 || length > kMaximumMainBinaryBytes)
        return jsonString(env, blockedReport("The embedded game executable exceeds the runtime input limit.", true));

    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        env->GetByteArrayRegion(mainBinary, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
        if (env->ExceptionCheck())
            return nullptr;
        radek::compat_runtime::ShimRegistry shims;
        radek::compat_runtime::objc::ShimAdapter objcShims;
        radek::compat_runtime::libsystem::ShimAdapter libsystemShims;
        radek::compat_runtime::audio::ShimAdapter audioShims;
        radek::compat_runtime::SjLjUnwindAdapter sjljUnwind;
        radek::compat_runtime::compiler_rt::ShimAdapter compilerRuntime;
        objcShims.registerBindings(shims);
        libsystemShims.registerBindings(shims);
        audioShims.registerBindings(shims);
        sjljUnwind.registerBindings(shims);
        compilerRuntime.registerBindings(shims);
        radek::compat_runtime::TrapShimAdapter traps;
        const auto cpu = radek::compat_runtime::createArm32CpuBackend();
        radek::compat_runtime::BootAttemptRunner runner(shims, *cpu, traps,
                                                       objcShims.lifecycleHooks());
        return jsonString(env, runner.run(bytes, true));
    } catch (const std::exception &error) {
        const std::string detail = std::string("Runtime initialization failed closed: ") + error.what();
        return jsonString(env, blockedReport(detail, true));
    }
}
