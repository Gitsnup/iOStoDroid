"""Tests for the iOS -> Android provider table.

The table exists in two places because both the host CLI and the Android app must
report the same thing: `radek/providers.py` and
`app/src/main/java/dev/radek/conventor/Providers.kt`. One test here parses the
Kotlin source and asserts the two agree, so they cannot drift.
"""

import re
import unittest
from pathlib import Path

from radek import providers

ROOT = Path(__file__).resolve().parent.parent
KOTLIN = ROOT / "app/src/main/java/dev/radek/conventor/Providers.kt"
API_MAPPER_KOTLIN = ROOT / "app/src/main/java/dev/radek/conventor/AndroidApiMapper.kt"
NATIVE_JNI = ROOT / "native/src/jni.cpp"

# The dependency lines reported as BLOCKED by the on-device UI.
REPORTED = (
    "/System/Library/Frameworks/Foundation.framework/Foundation",
    "/System/Library/Frameworks/OpenGLES.framework/OpenGLES",
    "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
    "/System/Library/Frameworks/AVFoundation.framework/AVFoundation",
    "/System/Library/Frameworks/MediaPlayer.framework/MediaPlayer",
    "/System/Library/Frameworks/CoreAudio.framework/CoreAudio",
    "/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox",
    "/System/Library/Frameworks/UIKit.framework/UIKit",
    "/System/Library/Frameworks/OpenAL.framework/OpenAL",
    "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
    "/System/Library/Frameworks/CFNetwork.framework/CFNetwork",
    "/System/Library/Frameworks/CoreMedia.framework/CoreMedia",
    "/System/Library/Frameworks/CoreVideo.framework/CoreVideo",
    "/usr/lib/libiconv.2.dylib",
    "/usr/lib/libgcc_s.1.dylib",
)


class ProviderTests(unittest.TestCase):
    def test_every_reported_framework_has_a_real_provider(self):
        for install_name in REPORTED:
            with self.subTest(install_name=install_name):
                provider = providers.for_install_name(install_name)
                self.assertIsNotNone(provider, install_name)
                self.assertNotEqual(providers.STATUS_BLOCKED, provider.status)
                self.assertTrue(provider.android)
                self.assertTrue(provider.detail)
                self.assertIn(provider.kind, (providers.KIND_LIBRARY, providers.KIND_PLATFORM, providers.KIND_RUNTIME))

    def test_identical_abis_bind_to_android_system_libraries(self):
        for install_name, expected in (
            ("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", "libGLESv2.so"),
            ("/usr/lib/libiconv.2.dylib", "iconv"),
            ("/usr/lib/libsqlite3.dylib", "libsqlite.so"),
            ("/usr/lib/libSystem.B.dylib", "libc.so"),
            ("/usr/lib/libz.1.dylib", "libz.so"),
        ):
            with self.subTest(install_name=install_name):
                provider = providers.for_install_name(install_name)
                self.assertEqual(providers.STATUS_PROVIDED, provider.status)
                self.assertEqual(providers.KIND_LIBRARY, provider.kind)
                self.assertIn(expected, provider.android)

    def test_legacy_libgcc_install_name_maps_only_to_symbol_level_ndk_candidates(self):
        provider = providers.for_install_name("/usr/lib/libgcc_s.1.dylib")
        self.assertIsNotNone(provider)
        self.assertEqual("libgcc_s", provider.framework)
        self.assertEqual(providers.KIND_RUNTIME, provider.kind)
        self.assertEqual(providers.STATUS_COMPATIBILITY, provider.status)
        self.assertIn("compiler-rt", provider.android)
        self.assertIn("libunwind", provider.android)
        self.assertIn("not a loadable-library alias", provider.detail)
        self.assertIn("NDK compiler-rt builtins", providers.for_symbol("___aeabi_uidiv"))
        self.assertIn("NDK libunwind", providers.for_symbol("__Unwind_Resume"))
        self.assertIn("NDK compiler-rt builtins", providers.for_symbol("___divti3"))
        self.assertIn("NDK compiler-rt builtins", providers.for_symbol("___divdi3"))
        self.assertIn("NDK libunwind", providers.for_symbol("__aeabi_unwind_cpp_pr0"))

    def test_platform_apis_map_onto_real_android_classes(self):
        for install_name, expected in (
            ("/System/Library/Frameworks/UIKit.framework/UIKit", "android.view"),
            ("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", "AudioTrack"),
            ("/System/Library/Frameworks/OpenAL.framework/OpenAL", "libaaudio.so"),
            ("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", "Choreographer"),
            ("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", "MediaPlayer"),
            ("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", "Canvas"),
            ("/System/Library/Frameworks/CoreVideo.framework/CoreVideo", "AHardwareBuffer"),
            ("/System/Library/Frameworks/CFNetwork.framework/CFNetwork", "HttpURLConnection"),
        ):
            with self.subTest(install_name=install_name):
                self.assertIn(expected, providers.for_install_name(install_name).android)

    def test_apis_without_an_android_contract_stay_blocked(self):
        for install_name in (
            "/System/Library/Frameworks/StoreKit.framework/StoreKit",
            "/System/Library/Frameworks/GameKit.framework/GameKit",
            "/System/Library/Frameworks/AdSupport.framework/AdSupport",
            "/System/Library/Frameworks/CoreLocation.framework/CoreLocation",
            "/usr/lib/libDoesNotExist.dylib",
        ):
            with self.subTest(install_name=install_name):
                self.assertEqual(providers.STATUS_BLOCKED, providers.classify(install_name)["status"])

    def test_longest_suffix_wins(self):
        provider = providers.for_install_name("/System/Library/Frameworks/UIKit.framework/UIKit")
        self.assertEqual("UIKit", provider.framework)

    def test_symbol_providers(self):
        for symbol, expected in (
            ("_glClear", "OpenGL ES"),
            ("_eglSwapBuffers", "EGL"),
            ("_iconv_open", "iconv"),
            ("_sqlite3_open", "libsqlite"),
            ("_objc_msgSend", "message dispatch"),
            ("_dispatch_async", "concurrent"),
            ("_pthread_create", "pthread"),
            ("_mach_absolute_time", "libioscompat.so"),
            ("_CFAbsoluteTimeGetCurrent", "CFAbsoluteTimeGetCurrent"),
            ("_CACurrentMediaTime", "CACurrentMediaTime"),
            ("_mach_timebase_info", "mach_timebase_info"),
            ("_malloc", "libioscompat.so:radek_compat_malloc"),
        ):
            with self.subTest(symbol=symbol):
                provider = providers.for_symbol(symbol)
                self.assertIsNotNone(provider, symbol)
                self.assertIn(expected, provider)

    def test_unknown_lowercase_symbols_are_not_assumed_to_be_bionic(self):
        self.assertIsNone(providers.for_symbol("_unknownAppleFunction"))
        self.assertIsNone(providers.for_symbol("_private_vendor_entry"))

    def test_coverage_is_honest(self):
        self.assertEqual(100, providers.coverage([], []))
        self.assertEqual(
            100,
            providers.coverage(["/System/Library/Frameworks/OpenGLES.framework/OpenGLES"], ["_glClear"]),
        )
        self.assertLess(
            providers.coverage(["/System/Library/Frameworks/StoreKit.framework/StoreKit"], []), 100
        )
        self.assertEqual(0, providers.coverage(["/usr/lib/libUnknown.dylib"], []))

    def test_native_whitelist_covers_every_provided_library(self):
        for provider in providers.TABLE:
            if provider.kind != providers.KIND_LIBRARY or provider.status == providers.STATUS_BLOCKED:
                continue
            for library in re.findall(r"lib[\w+.-]*\.so", provider.android):
                self.assertIn(library, providers.NATIVE_LIBRARIES, library)

    def test_kotlin_table_matches(self):
        """The app and the CLI must never disagree about a provider."""
        source = KOTLIN.read_text()
        entries = re.findall(
            r'Provider\("([^"]+)",\s*"([^"]+)",\s*\n?\s*"([^"]*)"[^,]*,\s*\n?\s*(KIND_\w+),\s*(STATUS_\w+)',
            source,
        )
        kotlin = {name: (framework, android, kind, status) for name, framework, android, kind, status in entries}
        self.assertEqual(len(providers.TABLE), len(kotlin), "provider tables have different sizes")
        for provider in providers.TABLE:
            with self.subTest(install_name=provider.install_name):
                self.assertIn(provider.install_name, kotlin)
                framework, android, kind, status = kotlin[provider.install_name]
                self.assertEqual(provider.framework, framework)
                self.assertEqual(provider.android, android)
                self.assertEqual("KIND_" + {"native-library": "LIBRARY", "platform-api": "PLATFORM", "runtime": "RUNTIME"}[provider.kind], kind)
                self.assertEqual("STATUS_" + provider.status.upper(), status)

    def test_runtime_ndk_export_whitelists_match_between_kotlin_and_jni(self):
        kotlin = API_MAPPER_KOTLIN.read_text()
        cpp = NATIVE_JNI.read_text()
        kotlin_block = kotlin.split("private val ndkRuntimeLibraries = setOf(", 1)[1].split(")", 1)[0]
        cpp_block = cpp.split("kPublicNdkLibraries[] = {", 1)[1].split("};", 1)[0]
        kotlin_libraries = set(re.findall(r'"(lib[^"]+\.so)"', kotlin_block))
        cpp_libraries = set(re.findall(r'"(lib[^"]+\.so)"', cpp_block))
        self.assertEqual(kotlin_libraries, cpp_libraries)
        self.assertTrue({"libnativewindow.so", "libneuralnetworks.so", "libsync.so"}.issubset(kotlin_libraries))

    def test_analysis_edges_carry_provider_fields(self):
        edge = providers.classify("/System/Library/Frameworks/UIKit.framework/UIKit")
        self.assertEqual(
            {"framework", "provider", "providerKind", "status", "reason"}, set(edge)
        )


if __name__ == "__main__":
    unittest.main()
