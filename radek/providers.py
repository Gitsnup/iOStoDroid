"""iOS install names and imported symbols mapped to real Android providers.

This module is the host-side twin of ``app/src/main/java/dev/radek/conventor/
Providers.kt``; ``tests/test_providers.py`` asserts the two tables agree, so the
CLI report and the on-device report never diverge.

This inventory separates reviewed Android system ABIs, bounded tested compatibility subsets,
semantic/API candidates, and missing execution paths. A provider/candidate entry is not proof that a
converted game has been rewritten or linked to it. ``STATUS_PROVIDED`` denotes an Android system
library only; framework targets without an implemented ABI adapter are ``candidate`` rather than
``compatibility``.

* ``native-library`` - Android system libraries with reviewed C APIs.
* ``platform-api`` - Android platform targets; semantic adaptation may still be required.
* ``runtime`` - compatibility-runtime targets, not an Apple ABI claim.
"""

from __future__ import annotations

from .api_implementations import _SUPPORTED as _COMPILED_COMPAT_IMPORTS

KIND_LIBRARY = "native-library"
KIND_PLATFORM = "platform-api"
KIND_RUNTIME = "runtime"

STATUS_PROVIDED = "provided"
STATUS_COMPATIBILITY = "compatibility"
STATUS_CANDIDATE = "candidate"
STATUS_NO_EXECUTION_PATH_YET = "no-execution-path-yet"
# Backwards-compatible name for callers that previously treated a missing path as blocked.
STATUS_BLOCKED = STATUS_NO_EXECUTION_PATH_YET


def evidence_summary(
    observed_symbols=(),
    *,
    verified_device_symbols=(),
    host_tested_symbols=(),
    stub_only_symbols=(),
    association_complete=True,
) -> dict:
    """Return disjoint per-import evidence counts without implying game linkage.

    Device export verification, host tests, and stub registrations are independent
    facts, so a symbol may appear in more than one evidence set. ``noneCount`` is
    the number of observed imports with none of those facts. No entry in this
    report proves a game callsite was rewritten, linked, or executed.
    """
    observed = {str(value) for value in observed_symbols if isinstance(value, str) and value}
    verified = observed & set(verified_device_symbols)
    host_tested = observed & set(host_tested_symbols)
    stub_only = observed & set(stub_only_symbols)
    evidenced = verified | host_tested | stub_only
    kinds = []
    if verified:
        kinds.append("exports-verified-on-this-device")
    if host_tested:
        kinds.append("host-tested-implementation")
    if stub_only:
        kinds.append("stub-only")
    no_evidence = not evidenced
    no_evidence_count = len(observed - evidenced)
    if no_evidence or no_evidence_count:
        kinds.append("none")
    return {
        "observedImportCount": len(observed),
        "exportsVerifiedOnThisDevice": len(verified),
        "hostTestedImplementations": len(host_tested),
        "stubOnlyCount": len(stub_only),
        "noneCount": no_evidence_count,
        "none": no_evidence,
        "evidenceKinds": kinds,
        "verifiedExportSymbols": sorted(verified)[:12],
        "hostTestedSymbols": sorted(host_tested)[:12],
        "stubOnlySymbols": sorted(stub_only)[:12],
        "noEvidenceSymbols": sorted(observed - evidenced)[:12],
        "associationStatus": "COMPLETE" if association_complete else "PARTIAL",
        "runtimeBackingClaimed": False,
        "linkedGameCallCount": 0,
        "runtimeCallsObserved": False,
        "recompiledBytesLinked": 0,
        "note": "Evidence is about exports, host tests, or explicit stubs only; no IPA callsite is claimed linked or running.",
    }


#: Native libraries a converted image may legitimately depend on. This extends
#: the whitelist ``radek/apk.py:validate_apk`` checks against.
NATIVE_LIBRARIES = (
    "libc.so",
    "libm.so",
    "libdl.so",
    "liblog.so",
    "libandroid.so",
    "libz.so",
    "libGLESv1_CM.so",
    "libGLESv2.so",
    "libGLESv3.so",
    "libEGL.so",
    "libaaudio.so",
    "libmediandk.so",
    "libsqlite.so",
    "libc++_shared.so",
    "libioscompat.so",
)


class Provider:
    __slots__ = ("install_name", "framework", "android", "kind", "status", "detail")

    def __init__(self, install_name, framework, android, kind, status, detail):
        self.install_name = install_name
        self.framework = framework
        self.android = android
        self.kind = kind
        self.status = status
        self.detail = detail

    def as_dict(self) -> dict:
        return {
            "framework": self.framework,
            "provider": self.android,
            "providerKind": self.kind,
            "kind": self.kind,
            "status": self.status,
            "classification": self.status,
            "reason": self.detail,
            "evidence": evidence_summary(),
        }


TABLE: tuple[Provider, ...] = (
    Provider("OpenGLES.framework/OpenGLES", "OpenGLES",
             "libGLESv2.so · libGLESv3.so · libEGL.so · libGLESv1_CM.so", KIND_LIBRARY, STATUS_PROVIDED,
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
             "libioscompat.so time/C frame-link subset · Android Choreographer (not Apple QuartzCore ABI)",
             KIND_PLATFORM, STATUS_COMPATIBILITY,
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
             "Media NDK / MediaFormat (possible targets)", KIND_PLATFORM,
             STATUS_CANDIDATE,
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
             KIND_PLATFORM, STATUS_BLOCKED,
             "Location requires a runtime permission flow the converted app has not requested."),
    Provider("CoreBluetooth.framework/CoreBluetooth", "CoreBluetooth", "none",
             KIND_PLATFORM, STATUS_BLOCKED, "Bluetooth LE needs its own permission and GATT stack binding."),
)

#: Exact compiled compatibility exports. The canonical symbol list is shared with
#: the host source generator and mirrored by the Android mapper.
IMPLEMENTED_C_API_SHIMS = {
    symbol: f"libioscompat.so:{implementation}"
    for symbol, (implementation, _selection_macro) in _COMPILED_COMPAT_IMPORTS.items()
}

#: Exact reviewed libc/libm/libdl name candidates. A same-name candidate is not
#: proof that the Darwin ABI or its behavior can be linked safely.
BIONIC_SYMBOL_CANDIDATES = frozenset(
    """
    __stack_chk_fail _exit abort abs accept access acos acosf acosh acoshf adler32 adler32_combine alarm asctime
    asctime_r asin asinf asinh asinhf atan atan2 atan2f atanf atanh atanhf atexit atof atoi atol atoll bind bsearch
    calloc cbrt cbrtf ceil ceilf chdir chmod clearerr clock clock_gettime clock_settime close closedir compress
    compress2 compressBound connect copysign copysignf cos cosf cosh coshf crc32 crc32_combine creat ctime ctime_r
    deflate deflateBound deflateCopy deflateEnd deflateInit2_ deflateInit_ deflateParams deflatePrime deflateReset
    deflateSetDictionary deflateSetHeader deflateTune difftime dladdr dlclose dlerror dlinfo dlopen dlsym dup dup2
    eglBindAPI eglBindTexImage eglChooseConfig eglCopyBuffers eglCreateContext eglCreatePbufferFromClientBuffer
    eglCreatePbufferSurface eglCreatePixmapSurface eglCreateWindowSurface eglDestroyContext eglDestroySurface
    eglGetConfigAttrib eglGetConfigs eglGetCurrentContext eglGetCurrentDisplay eglGetCurrentSurface eglGetDisplay
    eglGetError eglGetPlatformDisplay eglGetProcAddress eglInitialize eglMakeCurrent eglQueryAPI eglQueryContext
    eglQueryString eglQuerySurface eglReleaseTexImage eglReleaseThread eglSurfaceAttrib eglSwapBuffers
    eglSwapInterval eglTerminate eglWaitClient eglWaitGL eglWaitNative endgrent endpwent erf erfc erfcf erff execl
    execle execlp execv execve execvp exit exp exp2 exp2f expf expm1 expm1f fabs fabsf fchmod fchown fclose
    fdatasync fdim fdimf fdopen feof ferror fflush fgetc fgetpos fgets fileno flock floor floorf fma fmaf fmax fmaxf
    fmin fminf fmod fmodf fopen fputc fputs fread free freeaddrinfo freopen frexp frexpf fscanf fseek fseeko fsetpos
    fsync ftell ftello ftruncate fwrite gai_strerror getc getcwd getegid getenv geteuid getgid getgrent getgrgid
    getgrnam getgroups gethostbyaddr gethostbyname gethostname getlogin getpeername getpgrp getpid getppid getpwent
    getpwnam getpwuid getservbyname getservbyport getsockname getsockopt gettimeofday getuid glActiveTexture
    glAlphaFunc glAttachShader glBindAttribLocation glBindBuffer glBindFramebuffer glBindRenderbuffer glBindTexture
    glBlendColor glBlendEquation glBlendEquationSeparate glBlendFunc glBlendFuncSeparate glBufferData
    glBufferSubData glCheckFramebufferStatus glClear glClearColor glClearDepthf glClearStencil glClientActiveTexture
    glColor4f glColor4ub glColor4x glColorMask glColorPointer glCompileShader glCompressedTexImage2D
    glCompressedTexSubImage2D glCopyTexImage2D glCopyTexSubImage2D glCreateProgram glCreateShader glCullFace
    glDeleteBuffers glDeleteFramebuffers glDeleteProgram glDeleteRenderbuffers glDeleteShader glDeleteTextures
    glDepthFunc glDepthMask glDepthRangef glDetachShader glDisable glDisableClientState glDisableVertexAttribArray
    glDrawArrays glDrawElements glEnable glEnableClientState glEnableVertexAttribArray glFinish glFlush glFogf
    glFogfv glFogi glFogiv glFramebufferRenderbuffer glFramebufferTexture2D glFrontFace glFrustumf glGenBuffers
    glGenFramebuffers glGenRenderbuffers glGenTextures glGenerateMipmap glGetActiveAttrib glGetActiveUniform
    glGetAttribLocation glGetBooleanv glGetBufferParameteriv glGetClipPlane glGetError glGetFloatv
    glGetFramebufferAttachmentParameteriv glGetIntegerv glGetLightfv glGetLightiv glGetMaterialfv glGetMaterialiv
    glGetPointerv glGetProgramInfoLog glGetProgramiv glGetRenderbufferParameteriv glGetShaderInfoLog
    glGetShaderPrecisionFormat glGetShaderiv glGetString glGetTexEnviv glGetTexEnvxv glGetTexParameterfv
    glGetTexParameteriv glGetTexParameterxv glGetUniformLocation glGetUniformfv glGetUniformiv glGetVertexAttribfv
    glGetVertexAttribiv glHint glIsBuffer glIsEnabled glIsFramebuffer glIsProgram glIsRenderbuffer glIsShader
    glIsTexture glLightModelf glLightModelfv glLightf glLightfv glLighti glLightiv glLineWidth glLinkProgram
    glLoadIdentity glLoadMatrixf glLogicOp glMaterialf glMaterialfv glMatrixMode glMultMatrixf glMultiTexCoord4f
    glNormal3f glNormalPointer glOrthof glPixelStorei glPointSize glPolygonOffset glPopMatrix glPushMatrix
    glReadPixels glReleaseShaderCompiler glRenderbufferStorage glRotatef glSampleCoverage glScalef glScissor
    glShadeModel glShaderBinary glShaderSource glStencilFunc glStencilFuncSeparate glStencilMask
    glStencilMaskSeparate glStencilOp glStencilOpSeparate glTexCoordPointer glTexEnvf glTexEnvfv glTexEnvi
    glTexEnviv glTexEnvx glTexEnvxv glTexImage2D glTexParameterf glTexParameterfv glTexParameteri glTexParameteriv
    glTexParameterx glTexParameterxv glTexSubImage2D glTranslatef glUniform1f glUniform1fv glUniform1i glUniform1iv
    glUniform2f glUniform2fv glUniform2i glUniform2iv glUniform3f glUniform3fv glUniform3i glUniform3iv glUniform4f
    glUniform4fv glUniform4i glUniform4iv glUniformMatrix2fv glUniformMatrix3fv glUniformMatrix4fv glUseProgram
    glValidateProgram glVertexAttrib1f glVertexAttrib1fv glVertexAttrib2f glVertexAttrib2fv glVertexAttrib3f
    glVertexAttrib3fv glVertexAttrib4f glVertexAttrib4fv glVertexAttribPointer glVertexPointer glViewport gmtime
    gmtime_r gzclose gzopen gzread gzwrite hypot hypotf iconv iconv_close iconv_open ilogb ilogbf inet_addr
    inet_aton inet_ntoa inet_ntop inet_pton inflate inflateBack inflateBackEnd inflateBackInit_ inflateCopy
    inflateEnd inflateInit2_ inflateInit_ inflatePrime inflateReset inflateSetDictionary inflateSync
    inflateSyncPoint isatty isdigit islower isspace isupper isxdigit j0 j1 jn kill lchown ldexp ldexpf lgamma
    lgammaf link listen llrint llrintf llround llroundf localtime localtime_r log log10 log10f log1p log1pf log2
    log2f logb logbf logf lrint lrintf lround lroundf lseek malloc memchr memcmp memcpy memmem memmove memrchr
    memset mkdir mkdtemp mkstemp mktime mmap modf modff mprotect munmap nanosleep nearbyint nearbyintf nextafter
    nextafterf nexttoward open opendir pathconf pause pclose perror pipe popen pow powf printf pselect
    pthread_atfork pthread_attr_destroy pthread_attr_init pthread_attr_setdetachstate pthread_attr_setschedparam
    pthread_attr_setstacksize pthread_cond_broadcast pthread_cond_destroy pthread_cond_init pthread_cond_signal
    pthread_cond_timedwait pthread_cond_wait pthread_create pthread_detach pthread_equal pthread_exit
    pthread_getspecific pthread_join pthread_key_create pthread_key_delete pthread_kill pthread_mutex_destroy
    pthread_mutex_init pthread_mutex_lock pthread_mutex_trylock pthread_mutex_unlock pthread_mutexattr_destroy
    pthread_mutexattr_init pthread_mutexattr_settype pthread_once pthread_rwlock_destroy pthread_rwlock_rdlock
    pthread_rwlock_unlock pthread_rwlock_wrlock pthread_self pthread_setname_np pthread_setschedparam
    pthread_setspecific pthread_sigmask putc putchar putenv puts qsort raise rand random read readdir readdir_r
    readlink readv realloc realpath recv recvfrom recvmsg remainder remainderf remove remquo remquof rename
    rewinddir rint rintf rmdir round roundf scalbn scalbnf scanf sched_yield seekdir select send sendmsg sendto
    setbuf setenv setgrent setgroups setlocale setpwent setsockopt setvbuf sigaddset sigdelset sigemptyset
    sigfillset sigismember signal significand sin sinf sinh sinhf sleep snprintf socket socketpair sprintf sqrt
    sqrtf srand srandom sscanf strcasecmp strcat strchr strcmp strcoll strcpy strcspn strdup strerror strerror_r
    strftime strlen strncasecmp strncat strncmp strncpy strndup strnlen strpbrk strptime strrchr strsep strsignal
    strspn strstr strtod strtof strtoimax strtok strtok_r strtol strtold strtoll strtoul strtoull strtoumax strxfrm
    symlink sysconf system tan tanf tanh tanhf tcdrain tcflow tcflush tcgetattr tcsendbreak tcsetattr telldir tgamma
    tgammaf time timegm tmpfile tmpnam tolower toupper towlower towupper trunc truncate truncf ttyname tzset umask
    uname uncompress ungetc unlink unsetenv unzClose unzOpen unzReadCurrentFile usleep utime utimes vfork vfprintf
    vfscanf vprintf vscanf vsnprintf vsprintf vsscanf wait waitpid write writev y0 y1 yn zlibCompileFlags
    zlibVersion
    """.split()
)

#: Broad symbol-family triage hints only; these are not proof of replacement code.
SYMBOL_PROVIDERS: tuple[tuple[str, str], ...] = (
    ("gl", "OpenGL ES (libGLESv2.so/libGLESv3.so)"),
    ("egl", "EGL (libEGL.so)"),
    ("al", "OpenAL over AAudio"),
    ("alc", "OpenAL context over AAudio"),
    ("Audio", "AudioToolbox/CoreAudio over AAudio"),
    ("ExtAudio", "ExtAudioFile over MediaExtractor"),
    ("CG", "CoreGraphics over android.graphics"),
    ("CA", "QuartzCore; partial C frame-clock bridge only"),
    ("CM", "CoreMedia over MediaCodec"),
    ("CV", "CoreVideo over AHardwareBuffer"),
    ("CF", "CoreFoundation over the runtime"),
    ("NS", "Foundation over the runtime"),
    ("UI", "UIKit over android.view"),
    ("AV", "AVFoundation over android.media"),
    ("MP", "MediaPlayer over android.media"),
    ("iconv", "bionic iconv"),
    ("sqlite3", "libsqlite.so"),
    ("objc_", "libioscompat.so message dispatch"),
    ("dispatch_", "java.util.concurrent + pthreads"),
    ("pthread_", "bionic pthreads"),
    ("mach_absolute_time", "clock_gettime(CLOCK_MONOTONIC)"),
    ("Sec", "java.security/AndroidKeyStore"),
    ("inflate", "libz.so"),
    ("deflate", "libz.so"),
    ("uncompress", "libz.so"),
)


def compiler_runtime_candidate(symbol: str) -> str | None:
    """Classify known GCC/compiler-rt imports without claiming a dynamic alias.

    The NDK's builtins are primarily toolchain static archives; unwind exports
    also require an ABI check. This table is useful for planning the eventual
    link, not evidence that a callsite resolves today.
    """
    name = symbol.lstrip("_")
    if name.startswith(("Unwind_", "gcc_personality_v0", "gxx_personality_v0", "aeabi_unwind_", "gnu_unwind_")):
        return "NDK libunwind/libc++abi (unwind ABI candidate; not linked)"
    builtins = (
        "aeabi_", "divdi3", "udivdi3", "moddi3", "umoddi3", "muldi3", "ashldi3", "ashrdi3",
        "lshrdi3", "udivmoddi4", "divti3", "udivti3", "modti3", "umodti3", "multi3", "muloti4",
        "ashlti3", "ashrti3", "lshrti3", "addvti3", "subvti3", "absvti2", "cmpdi2", "ucmpdi2",
        "clear_cache", "register_frame", "deregister_frame", "fix", "float",
    )
    if name.startswith(builtins):
        return "NDK compiler-rt builtins (toolchain link candidate; not linked)"
    return None


def for_install_name(path: str) -> Provider | None:
    """Longest-suffix match, so ``UIKit.framework/UIKit`` wins over a prefix."""
    best: Provider | None = None
    for provider in TABLE:
        if path.endswith(provider.install_name) or ("/" + provider.install_name) in path:
            if best is None or len(provider.install_name) > len(best.install_name):
                best = provider
    return best


def for_symbol(symbol: str) -> str | None:
    """Return an exact shim or triage hint; unknown lower-case names stay unknown."""
    if symbol in IMPLEMENTED_C_API_SHIMS:
        return IMPLEMENTED_C_API_SHIMS[symbol]
    if runtime_candidate := compiler_runtime_candidate(symbol):
        return runtime_candidate
    name = symbol.lstrip("_")
    for prefix, provider in SYMBOL_PROVIDERS:
        if name.startswith(prefix):
            return provider
    if name in BIONIC_SYMBOL_CANDIDATES:
        return "bionic libc/libm/libdl (same-name candidate)"
    return None


def classify(install_name: str) -> dict:
    """Fields added to every dependency edge of a host report."""
    provider = for_install_name(install_name)
    if provider is None:
        return {
            "status": STATUS_NO_EXECUTION_PATH_YET,
            "classification": STATUS_NO_EXECUTION_PATH_YET,
            "provider": "",
            "providerKind": "",
            "kind": "",
            "reason": "No execution path yet: this Darwin image has no verified Android provider or ABI adapter.",
            "evidence": evidence_summary(),
        }
    return provider.as_dict()


def coverage(dependencies: list, imports: list) -> int:
    """Static provider/candidate triage share (0-100), not linked implementation coverage."""
    total = len(dependencies) + len(imports)
    if not total:
        return 100
    mapped = sum(
        1
        for name in dependencies
        if (provider := for_install_name(name)) is not None and provider.status != STATUS_BLOCKED
    )
    mapped += sum(1 for symbol in imports if for_symbol(symbol) is not None)
    return mapped * 100 // total
