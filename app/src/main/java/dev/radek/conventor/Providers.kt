package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * iOS install names and external symbols mapped to the Android implementation
 * that actually provides them.
 *
 * Entries are provider candidates, not universal support claims. A [KIND_LIBRARY]
 * entry can name an Android system ABI; [KIND_PLATFORM] and [KIND_RUNTIME] entries
 * are triage hints until an ABI-safe adapter is compiled and wired into generated
 * output. Only individually tested compatibility exports are verified; unimplemented
 * imports remain explicit blockers or stubs.
 */
object Providers {
    const val KIND_LIBRARY = "native-library"
    const val KIND_PLATFORM = "platform-api"
    const val KIND_RUNTIME = "runtime"

    const val STATUS_PROVIDED = "provided"
    const val STATUS_COMPATIBILITY = "compatibility"
    const val STATUS_CANDIDATE = "candidate"
    const val STATUS_NO_EXECUTION_PATH_YET = "no-execution-path-yet"
    // Backwards-compatible name for callers that used the old blocked state.
    const val STATUS_BLOCKED = STATUS_NO_EXECUTION_PATH_YET

    /** Native libraries the converted image may legitimately depend on. */
    val NATIVE_LIBRARIES = listOf(
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so",
        "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libEGL.so", "libaaudio.so",
        "libmediandk.so", "libsqlite.so", "libc++_shared.so", "libioscompat.so"
    )

    data class Provider(
        val installName: String,
        val framework: String,
        val android: String,
        val kind: String,
        val status: String,
        val detail: String
    )

    private val TABLE = listOf(
        Provider("OpenGLES.framework/OpenGLES", "OpenGLES", "libGLESv2.so · libGLESv3.so · libEGL.so · libGLESv1_CM.so",
            KIND_LIBRARY, STATUS_PROVIDED,
            "Android provides GLES/EGL system libraries with Khronos C ABIs; this identifies a possible system target only and does not prove that an IPA import resolves or is linked."),
        Provider("libiconv.2.dylib", "libiconv", "bionic libc.so iconv · iconv_open · iconv_close",
            KIND_LIBRARY, STATUS_PROVIDED,
            "Android bionic exports iconv on supported API levels. A system symbol inventory match does not prove this IPA's ABI, load, or callsite linkage."),
        Provider("libsqlite3.dylib", "libsqlite3", "libsqlite.so sqlite3_*",
            KIND_LIBRARY, STATUS_PROVIDED, "Android ships a SQLite system library as libsqlite.so; this does not prove an IPA callsite is rewritten or linked."),
        Provider("libSystem.B.dylib", "libSystem", "bionic libc.so · libm.so · libdl.so · liblog.so · pthreads",
            KIND_LIBRARY, STATUS_PROVIDED, "Bionic provides Android's libc/libm/libdl and pthread system libraries; Darwin-specific layouts and behavior are not implied, and no IPA linkage is proven."),
        Provider("libc++.1.dylib", "libc++", "NDK libc++_shared.so (packaging/runtime presence unverified)",
            KIND_RUNTIME, STATUS_CANDIDATE,
            "The NDK LLVM libc++ is a possible target, but this mapping does not prove the library is present in an output APK or that Apple's caller ABI and runtime behavior match."),
        Provider("libc++abi.dylib", "libc++abi", "NDK libc++abi (toolchain candidate)",
            KIND_RUNTIME, STATUS_CANDIDATE,
            "A toolchain candidate only; the Apple C++ ABI, exception behavior, and output link have not been validated."),
        Provider("libstdc++.6.dylib", "libstdc++", "GNU libstdc++ ABI: no drop-in NDK provider; libc++_shared.so has low-level overlap only",
            KIND_RUNTIME, STATUS_CANDIDATE,
            "GNU libstdc++ and LLVM libc++ use different C++ ABIs and mangling. Only some low-level C symbols may overlap; no NDK libstdc++ serving or compatible C++ runtime link is proven."),
        Provider("libgcc_s.1.dylib", "libgcc_s", "compiler-rt builtins / libunwind per-symbol candidates; no libgcc_s.so alias",
            KIND_RUNTIME, STATUS_CANDIDATE,
            "Android NDK does not ship a drop-in libgcc_s.so. Compiler helper symbols and unwind/personality symbols need per-symbol ABI and toolchain validation. This is a candidate only, not a loadable-library mapping or completed link."),
        Provider("libz.1.dylib", "libz", "libz.so deflate · inflate · crc32",
            KIND_LIBRARY, STATUS_PROVIDED, "zlib is part of the Android platform."),
        Provider("libresolv.9.dylib", "libresolv", "bionic getaddrinfo · res_*",
            KIND_LIBRARY, STATUS_PROVIDED, "Resolver entry points are in bionic libc."),
        Provider("libcompression.dylib", "libcompression", "libz.so · java.util.zip (candidate building blocks)",
            KIND_PLATFORM, STATUS_CANDIDATE,
            "No libcompression ABI adapter or game callsite link is implemented; zlib/Java zip are only possible building blocks, and LZFSE/LZMA behavior is not proven."),

        Provider("Foundation.framework/Foundation", "Foundation",
            "Android Java/Kotlin API analogues (semantic rewrite candidates only)", KIND_RUNTIME, STATUS_CANDIDATE,
            "No Foundation/Objective-C ABI adapter is implemented. NSString, collections, bundle lookup, notifications, and defaults are not linked or backed by Android APIs for imported game code."),
        Provider("CoreFoundation.framework/CoreFoundation", "CoreFoundation",
            "libioscompat.so host-tested C subset (CFString/CFData/CFArray/CFDictionary/CFNumber/CFDate + limited CFRunLoop)", KIND_RUNTIME, STATUS_COMPATIBILITY,
            "Partial only: a small opaque CF object/collection model and queued C-callback run-loop subset have host tests. This is not full CoreFoundation; Apple callbacks/Blocks, run-loop sources/timers, toll-free bridging, loader callouts, and game callsite linking are not implemented."),
        Provider("libobjc.A.dylib", "libobjc", "compat-runtime-v1 standalone Objective-C model (not linked to the IPA loader)",
            KIND_RUNTIME, STATUS_COMPATIBILITY,
            "A host-tested class/metaclass/selector/dispatch and retain/release/autorelease model exists, but it is not wired to the guest import loader as an Apple Objective-C ABI adapter; no game callsites are linked."),
        Provider("UIKit.framework/UIKit", "UIKit",
            "android.app.Activity / android.view.View / TextView / ImageView analogues (rewrite candidates only)",
            KIND_PLATFORM, STATUS_CANDIDATE,
            "Android UI classes exist, but UIKit classes, lifecycle, object layout, input and API behavior are not adapted or linked for the IPA."),
        Provider("CoreGraphics.framework/CoreGraphics", "CoreGraphics",
            "Android Canvas/Paint/Bitmap analogues (rewrite candidates only)",
            KIND_PLATFORM, STATUS_CANDIDATE,
            "No CoreGraphics ABI adapter or game callsite rewrite is implemented; Android drawing APIs are only possible semantic targets."),
        Provider("QuartzCore.framework/QuartzCore", "QuartzCore",
            "libioscompat.so time/C frame-link subset · Android Choreographer (not Apple QuartzCore ABI)", KIND_PLATFORM, STATUS_COMPATIBILITY,
            "Partial only: host-tested time exports and a C frame-link callback service exist. CADisplayLink's Objective-C ABI, CALayer, CAAnimation, callsite rewriting, and game linkage are not implemented."),
        Provider("OpenAL.framework/OpenAL", "OpenAL", "libaaudio.so / AAudio (possible output target)",
            KIND_PLATFORM, STATUS_CANDIDATE,
            "No OpenAL ABI, source/buffer model, mixer, or IPA callsite adapter is implemented; AAudio is only a possible output target."),
        Provider("AudioToolbox.framework/AudioToolbox", "AudioToolbox",
            "AAudio / AudioTrack / SoundPool (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No AudioToolbox ABI or AudioQueue/AudioServices/ExtAudioFile behavior is adapted or linked to imported game code."),
        Provider("CoreAudio.framework/CoreAudio", "CoreAudio",
            "AAudio / AudioManager (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No CoreAudio ABI, AudioComponent renderer, or IPA callsite adapter is implemented."),
        Provider("AVFoundation.framework/AVFoundation", "AVFoundation",
            "MediaPlayer / MediaExtractor / MediaCodec (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "Android media APIs are possible semantic targets only; AVFoundation classes, lifecycle, behavior, and callsite rewriting are not implemented."),
        Provider("MediaPlayer.framework/MediaPlayer", "MediaPlayer",
            "MediaPlayer / SoundPool / AudioManager (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No MediaPlayer.framework ABI adapter or imported game callsite link is implemented."),
        Provider("CoreMedia.framework/CoreMedia", "CoreMedia",
            "Media NDK / MediaFormat (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No CoreMedia ABI or CMSampleBuffer/CMTime-to-Android adapter is implemented; no calls are linked."),
        Provider("CoreVideo.framework/CoreVideo", "CoreVideo",
            "AHardwareBuffer / ANativeWindow (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No CoreVideo pixel-buffer ABI or pool/surface adapter is implemented; these are target APIs only."),
        Provider("CFNetwork.framework/CFNetwork", "CFNetwork",
            "bionic sockets / HttpURLConnection (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No CFNetwork stream/message ABI adapter or IPA callsite rewrite is implemented."),
        Provider("CoreText.framework/CoreText", "CoreText",
            "Typeface / StaticLayout (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "Android text APIs are possible semantic targets only; CoreText glyph-run and layout behavior is not adapted."),
        Provider("ImageIO.framework/ImageIO", "ImageIO",
            "BitmapFactory / ImageDecoder (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "The importer can decode selected bundle icons, but no ImageIO ABI is provided to imported game code or linked."),
        Provider("Security.framework/Security", "Security",
            "java.security / AndroidKeyStore (possible targets)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No Security.framework/Keychain ABI or permission/behavior adapter is implemented; no game calls are linked."),
        Provider("SystemConfiguration.framework/SystemConfiguration", "SystemConfiguration",
            "ConnectivityManager (possible target)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No SystemConfiguration/Reachability callback ABI adapter or IPA callsite link is implemented."),
        Provider("MobileCoreServices.framework/MobileCoreServices", "MobileCoreServices",
            "MimeTypeMap (possible target)", KIND_PLATFORM, STATUS_CANDIDATE,
            "No MobileCoreServices/UTI ABI adapter is implemented for imported game code."),
        Provider("Accelerate.framework/Accelerate", "Accelerate",
            "No verified Android implementation; NEON is only a possible technique", KIND_RUNTIME, STATUS_CANDIDATE,
            "No vDSP/vImage shim implementation or tested NEON call path is present."),
        Provider("Metal.framework/Metal", "Metal", "Vulkan / OpenGL ES (candidate rendering targets)",
            KIND_PLATFORM, STATUS_CANDIDATE,
            "No Metal ABI or command-buffer/pipeline rewriting is implemented; Vulkan/GLES are ideas, not a proven mapping."),
        Provider("WebKit.framework/WebKit", "WebKit", "WebView (possible target)",
            KIND_PLATFORM, STATUS_CANDIDATE,
            "No WKWebView ABI, navigation/lifecycle bridge, or IPA callsite adapter is implemented."),
        Provider("AdSupport.framework/AdSupport", "AdSupport", "none",
            KIND_PLATFORM, STATUS_BLOCKED,
            "IDFA has no Android contract; the advertising id requires Play Services and explicit consent."),
        Provider("StoreKit.framework/StoreKit", "StoreKit", "none",
            KIND_PLATFORM, STATUS_BLOCKED,
            "In-app purchase receipts are an App Store contract; Play Billing is a different flow and is not faked."),
        Provider("GameKit.framework/GameKit", "GameKit", "none",
            KIND_PLATFORM, STATUS_BLOCKED,
            "Game Center multiplayer/leaderboards have no Android counterpart."),
        Provider("Social.framework/Social", "Social", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "Share sheets are app-specific intents and are not impersonated."),
        Provider("MessageUI.framework/MessageUI", "MessageUI", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "In-app mail/SMS composition is not provided."),
        Provider("AddressBook.framework/AddressBook", "AddressBook", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "Contacts access requires its own runtime permission flow."),
        Provider("MapKit.framework/MapKit", "MapKit", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "Apple Maps tiles cannot be served on Android."),
        Provider("iAd.framework/iAd", "iAd", "none", KIND_PLATFORM, STATUS_BLOCKED, "The iAd network no longer exists."),
        Provider("EventKit.framework/EventKit", "EventKit", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "Calendar access requires its own permission flow."),
        Provider("CoreLocation.framework/CoreLocation", "CoreLocation", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "Location requires a runtime permission flow the converted app has not requested."),
        Provider("CoreBluetooth.framework/CoreBluetooth", "CoreBluetooth", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "Bluetooth LE needs its own permission and GATT stack binding.")
    )

    /**
     * Broad symbol-family triage hints only; these are not proof of generated or linked code.
     * Concrete compiled compatibility exports are tracked separately by AndroidApiMapper; broad
     * framework-family labels remain candidates.
     */
    private val SYMBOL_PROVIDERS = listOf(
        "gl" to "OpenGL ES (libGLESv2.so/libGLESv3.so)",
        "egl" to "EGL (libEGL.so)",
        "al" to "OpenAL over AAudio",
        "alc" to "OpenAL context over AAudio",
        "Audio" to "AudioToolbox/CoreAudio over AAudio",
        "ExtAudio" to "ExtAudioFile over MediaExtractor",
        "CG" to "CoreGraphics over android.graphics",
        "CA" to "QuartzCore; partial C frame-clock bridge only",
        "CM" to "CoreMedia over MediaCodec",
        "CV" to "CoreVideo over AHardwareBuffer",
        "CF" to "CoreFoundation over the runtime",
        "NS" to "Foundation over the runtime",
        "UI" to "UIKit over android.view",
        "AV" to "AVFoundation over android.media",
        "MP" to "MediaPlayer over android.media",
        "iconv" to "bionic iconv",
        "sqlite3" to "libsqlite.so",
        "objc_" to "libioscompat.so message dispatch",
        "dispatch_" to "java.util.concurrent + pthreads",
        "pthread_" to "bionic pthreads",
        "mach_absolute_time" to "clock_gettime(CLOCK_MONOTONIC)",
        "Sec" to "java.security/AndroidKeyStore",
        "inflate" to "libz.so",
        "deflate" to "libz.so",
        "uncompress" to "libz.so"
    )

    private fun compilerRuntimeCandidate(symbol: String): String? {
        val name = symbol.trimStart('_')
        val unwind = listOf("Unwind_", "gcc_personality_v0", "gxx_personality_v0", "aeabi_unwind_", "gnu_unwind_")
        if (unwind.any { prefix -> name.startsWith(prefix) }) {
            return "NDK libunwind/libc++abi (unwind ABI candidate; not linked)"
        }
        val builtins = listOf(
            "aeabi_", "divdi3", "udivdi3", "moddi3", "umoddi3", "muldi3", "ashldi3", "ashrdi3",
            "lshrdi3", "udivmoddi4", "divti3", "udivti3", "modti3", "umodti3", "multi3", "muloti4",
            "ashlti3", "ashrti3", "lshrti3", "addvti3", "subvti3", "absvti2", "cmpdi2", "ucmpdi2",
            "clear_cache", "register_frame", "deregister_frame", "fix", "float",
        )
        return if (builtins.any { prefix -> name.startsWith(prefix) }) {
            "NDK compiler-rt builtins (toolchain link candidate; not linked)"
        } else null
    }

    fun forInstallName(path: String): Provider? {
        val trimmed = path.trim()
        var best: Provider? = null
        for (provider in TABLE) {
            if (trimmed.endsWith(provider.installName) || trimmed.contains("/" + provider.installName)) {
                if (best == null || provider.installName.length > best.installName.length) best = provider
            }
        }
        return best
    }

    /** Provider for an imported symbol name, or null when nothing maps it. */
    fun forSymbol(symbol: String): String? {
        compilerRuntimeCandidate(symbol)?.let { return it }
        AndroidApiMapper.compiledCompatibilityProvider(symbol)?.let { return it }
        val candidate = symbol.trimStart('_')
        for ((prefix, provider) in SYMBOL_PROVIDERS) if (candidate.startsWith(prefix)) return provider
        // libc/libm/pthread entry points are provided by bionic directly.
        if (candidate.isNotEmpty() && candidate[0].isLowerCase()) return "bionic libc/libm"
        return null
    }

    /** Build honest, per-import evidence totals for one dependency edge. */
    fun evidenceForImports(
        observedSymbols: Collection<String>,
        mappedSymbols: Map<String, JSONObject>,
        associationComplete: Boolean = true,
    ): JSONObject {
        val observed = observedSymbols.filter { it.isNotBlank() }.toSortedSet()
        val verified = sortedSetOf<String>()
        val hostTested = sortedSetOf<String>()
        val stubOnly = sortedSetOf<String>()
        val none = sortedSetOf<String>()
        for (symbol in observed) {
            val evidence = mappedSymbols[symbol]?.optJSONObject("evidence")
            val verifiedHere = evidence?.optBoolean("exportsVerifiedOnThisDevice", false) == true
            val testedHere = evidence?.optBoolean("hostTestedImplementation", false) == true
            val stubHere = evidence?.optBoolean("stubOnly", false) == true
            if (verifiedHere) verified += symbol
            if (testedHere) hostTested += symbol
            if (stubHere) stubOnly += symbol
            if (!verifiedHere && !testedHere && !stubHere) none += symbol
        }
        val kinds = mutableListOf<String>()
        if (verified.isNotEmpty()) kinds += "exports-verified-on-this-device"
        if (hostTested.isNotEmpty()) kinds += "host-tested-implementation"
        if (stubOnly.isNotEmpty()) kinds += "stub-only"
        if (none.isNotEmpty() || observed.isEmpty()) kinds += "none"
        return JSONObject()
            .put("observedImportCount", observed.size)
            .put("exportsVerifiedOnThisDevice", verified.size)
            .put("hostTestedImplementations", hostTested.size)
            .put("stubOnlyCount", stubOnly.size)
            .put("noneCount", none.size)
            .put("none", verified.isEmpty() && hostTested.isEmpty() && stubOnly.isEmpty())
            .put("evidenceKinds", JSONArray(kinds))
            .put("verifiedExportSymbols", JSONArray(verified.take(12)))
            .put("hostTestedSymbols", JSONArray(hostTested.take(12)))
            .put("stubOnlySymbols", JSONArray(stubOnly.take(12)))
            .put("noEvidenceSymbols", JSONArray(none.take(12)))
            .put("associationStatus", if (associationComplete) "COMPLETE" else "PARTIAL")
            .put("runtimeBackingClaimed", false)
            .put("linkedGameCallCount", 0)
            .put("recompiledBytesLinked", 0)
            .put("note", "Evidence is about exports, host tests, or explicit stubs only; no IPA callsite is claimed linked or running.")
    }

    /**
     * Classify one dependency edge exactly like `radek/providers.py` does on the
     * host, so the app and the CLI report the same grade and disclosure.
     */
    fun classify(installName: String): JSONObject {
        val provider = forInstallName(installName)
        return if (provider == null) {
            JSONObject().put("installName", installName)
                .put("classification", STATUS_NO_EXECUTION_PATH_YET)
                .put("status", STATUS_NO_EXECUTION_PATH_YET).put("provider", "")
                .put("kind", "").put("providerKind", "")
                .put("reason", "No execution path yet: this Darwin image has no verified Android provider or ABI adapter.")
                .put("evidence", evidenceForImports(emptyList(), emptyMap()))
        } else {
            JSONObject().put("installName", installName)
                .put("framework", provider.framework)
                .put("classification", provider.status)
                .put("status", provider.status).put("kind", provider.kind).put("providerKind", provider.kind)
                .put("provider", provider.android)
                .put("reason", provider.detail)
                .put("evidence", evidenceForImports(emptyList(), emptyMap()))
        }
    }

    /**
     * Candidate-catalog coverage only. A non-null candidate does not prove ABI
     * compatibility or that its code is linked; callers must keep this separate
     * from verified exports and generated/linked implementation counts.
     */
    fun coverage(dependencies: JSONArray, imports: JSONArray): Int {
        val total = dependencies.length() + imports.length()
        if (total == 0) return 100
        var mapped = 0
        for (i in 0 until dependencies.length()) {
            val name = dependencies.getJSONObject(i).optString("path")
            val provider = forInstallName(name)
            if (provider != null && provider.status != STATUS_BLOCKED) mapped++
        }
        for (i in 0 until imports.length()) {
            val name = imports.getJSONObject(i).optString("name")
            if (forSymbol(name) != null) mapped++
        }
        return mapped * 100 / total
    }

    /** Human-readable table for the detail screen. */
    fun describe(installName: String): String {
        val provider = forInstallName(installName)
            ?: return "NO-EXECUTION-PATH-YET · no Android provider"
        val marker = when (provider.status) {
            STATUS_PROVIDED -> "PROVIDED"
            STATUS_COMPATIBILITY -> "COMPATIBILITY"
            STATUS_CANDIDATE -> "CANDIDATE"
            else -> "NO-EXECUTION-PATH-YET"
        }
        return "$marker · ${provider.framework} → ${provider.android}"
    }
}
