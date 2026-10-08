package dev.iostodroid.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Conservative native-symbol and semantic API equivalence inventory.
 * A candidate is a static recompilation plan only: this module does not rewrite Mach-O
 * code, bridge Objective-C objects, link a Bionic library, or generate Java.
 */
internal object AndroidApiMapper {
    // Keep high-volume games analyzable while bounding mapper/report growth.
    private const val MAX_SYMBOLS = 100_000

    /**
     * Classifications that describe a *reviewed Android mapping* of some kind:
     * a same-name NDK/system export, an NDK compiler-runtime toolchain symbol, a
     * concrete compiled compatibility implementation, or a reviewed semantic
     * target. `COMPAT_STUB_HANDLER_REGISTERED` (an explicitly unimplemented
     * handler) and `UNMAPPED` are deliberately not members.
     */
    private val REVIEWED_MAPPING_CLASSIFICATIONS = setOf(
        "BIONIC_SYMBOL_CANDIDATE",
        "COMPILER_RUNTIME_CANDIDATE",
        "IMPLEMENTED_API_REPLACEMENT_AVAILABLE",
        "COMPAT_VERIFIED_HANDLER_RESOLVED",
        "SEMANTIC_REWRITE_CANDIDATE",
    )

    private val ndkRuntimeLibraries = setOf(
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so", "libEGL.so",
        "libGLESv1_CM.so", "libGLESv2.so", "libaaudio.so", "libmediandk.so", "libvulkan.so",
        "libOpenSLES.so", "libOpenMAXAL.so", "libjnigraphics.so", "libbinder_ndk.so", "libamidi.so",
        "libcamera2ndk.so", "libc++_shared.so", "libnativewindow.so", "libneuralnetworks.so",
        "libsync.so",
    )

    /**
     * Reviewed same-name candidates in Android's public NDK/system libraries.
     *
     * Every entry is a *name* match in a library Android actually ships; a
     * candidate still needs caller-ABI, struct-layout and relocation
     * verification before anything can be linked. The catalog is the
     * device-side twin of `BIONIC_SYMBOL_CANDIDATES` in `iostodroid/providers.py`;
     * `tests/test_providers.py` asserts the two tables stay equal.
     */
    private val bionicLibraries = mapOf(
        "libc.so" to setOf(
            "__assert", "__assert2", "__cxa_atexit", "__cxa_finalize", "__errno", "__libc_current_sigrtmax",
            "__libc_current_sigrtmin", "__memcpy_chk", "__memmove_chk", "__memset_chk", "__sprintf_chk",
            "__stack_chk_fail", "__strcat_chk", "__strcpy_chk", "__strlen_chk", "__vsnprintf_chk",
            "__vsprintf_chk", "_exit", "_longjmp", "_setjmp", "abort", "abs", "accept", "access", "alarm",
            "asctime", "asctime_r", "atexit", "atof", "atoi", "atol", "atoll", "basename", "bcmp", "bcopy", "bind",
            "brk", "bsearch", "btowc", "bzero", "calloc", "chdir", "chmod", "clearerr", "clock", "clock_gettime",
            "clock_settime", "close", "closedir", "closelog", "connect", "creat", "ctime", "ctime_r", "difftime",
            "dirname", "dup", "dup2", "endgrent", "endpwent", "environ", "error", "error_at_line", "error_message_count",
            "error_one_per_line", "error_print_progname", "execl", "execle", "execlp", "execv", "execve",
            "execvp", "exit", "fchmod", "fchown", "fclose", "fcntl", "fdatasync", "fdopen", "feof", "ferror",
            "fflush", "ffs", "ffsl", "ffsll", "fgetc", "fgetpos", "fgets", "fileno", "flock", "fmemopen",
            "fnmatch", "fopen", "fprintf", "fputc", "fputs", "fread", "free", "freeaddrinfo", "freopen", "fscanf",
            "fseek", "fseeko", "fsetpos", "fstat", "fstatat", "fstatfs", "fstatvfs", "fsync", "ftell", "ftello",
            "ftruncate", "fwrite", "gai_strerror", "getc", "getcwd", "getdtablesize", "getegid", "getenv",
            "geteuid", "getgid", "getgrent", "getgrgid", "getgrnam", "getgroups", "gethostbyaddr", "gethostbyname",
            "gethostname", "getline", "getlogin", "getopt", "getopt_long", "getopt_long_only", "getpagesize",
            "getpeername", "getpgrp", "getpid", "getppid", "getpriority", "getprogname", "getpwent", "getpwnam",
            "getpwuid", "getservbyname", "getservbyport", "getsockname", "getsockopt", "gettid", "gettimeofday",
            "getuid", "gmtime", "gmtime_r", "hstrerror", "htonl", "htons", "iconv", "iconv_close", "iconv_open",
            "if_indextoname", "if_nametoindex", "inet_addr", "inet_aton", "inet_ntoa", "inet_ntop", "inet_pton",
            "ioctl", "isalnum", "isalpha", "isatty", "isblank", "iscntrl", "isdigit", "isgraph", "islower",
            "isprint", "ispunct", "isspace", "isupper", "isxdigit", "kill", "labs", "lchown", "ldexp", "link", "listen",
            "llabs", "localeconv", "localtime", "localtime_r", "longjmp", "lseek", "lstat", "madvise", "malloc",
            "malloc_usable_size", "mallopt", "mblen", "mbrlen", "mbrtowc", "mbsinit", "mbsrtowcs", "mbstowcs",
            "mbtowc", "memalign", "memccpy", "memchr", "memcmp", "memcpy", "memmem", "memmove", "mempcpy",
            "memrchr", "memset", "mincore", "mkdir", "mkdtemp", "mkstemp", "mktime", "mlock", "mlockall", "mmap",
            "mprotect", "mremap", "msync", "munlock", "munlockall", "munmap", "nanosleep", "newlocale",
            "nl_langinfo", "ntohl", "ntohs", "open", "opendir", "openlog", "pathconf", "pause", "pclose", "perror",
            "pipe", "poll", "popen", "printf", "pselect", "psignal", "pthread_atfork", "pthread_attr_destroy",
            "pthread_attr_init", "pthread_attr_setdetachstate", "pthread_attr_setschedparam",
            "pthread_attr_setstacksize", "pthread_cond_broadcast", "pthread_cond_destroy", "pthread_cond_init",
            "pthread_cond_signal", "pthread_cond_timedwait", "pthread_cond_wait", "pthread_create",
            "pthread_detach", "pthread_equal", "pthread_exit", "pthread_getschedparam", "pthread_getspecific",
            "pthread_join", "pthread_key_create", "pthread_key_delete", "pthread_kill", "pthread_mutex_destroy",
            "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock",
            "pthread_mutexattr_destroy", "pthread_mutexattr_init", "pthread_mutexattr_settype", "pthread_once",
            "pthread_rwlock_destroy", "pthread_rwlock_rdlock", "pthread_rwlock_unlock", "pthread_rwlock_wrlock",
            "pthread_self", "pthread_setname_np", "pthread_setschedparam", "pthread_setspecific",
            "pthread_sigmask", "putc", "putchar", "putenv", "puts", "qsort", "raise", "rand", "random", "read",
            "readdir", "readdir_r", "readlink", "readv", "realloc", "realpath", "recv", "recvfrom", "recvmsg",
            "regcomp", "regerror", "regexec", "regfree", "remove", "rename", "rewind", "rewinddir", "rmdir",
            "sbrk", "scandir", "scanf", "sched_yield", "seekdir", "select", "sem_destroy", "sem_getvalue",
            "sem_init", "sem_post", "sem_timedwait", "sem_trywait", "sem_wait", "send", "sendmsg", "sendto",
            "setbuf", "setbuffer", "setenv", "setgrent", "setgroups", "setjmp", "setlinebuf", "setlocale",
            "setpriority", "setprogname", "setpwent", "setsockopt", "setvbuf", "sigaction", "sigaddset",
            "sigaltstack", "sigdelset", "sigemptyset", "sigfillset", "sigismember", "signal", "sigprocmask",
            "sigwait", "sleep", "snprintf", "socket", "socketpair", "sprintf", "srand", "srandom", "sscanf",
            "stat", "statfs", "statvfs", "strcasecmp", "strcasestr", "strcat", "strchr", "strchrnul", "strcmp",
            "strcoll", "strcpy", "strcspn", "strdup", "strerror", "strerror_r", "strftime", "strlcat", "strlcpy",
            "strlen", "strncasecmp", "strncat", "strncmp", "strncpy", "strndup", "strnlen", "strpbrk", "strptime",
            "strrchr", "strsep", "strsignal", "strspn", "strstr", "strtod", "strtof", "strtoimax", "strtok",
            "strtok_r", "strtol", "strtold", "strtoll", "strtoul", "strtoull", "strtoumax", "strxfrm", "swab",
            "symlink", "symlinkat", "sysconf", "syslog", "system", "tcdrain", "tcflow", "tcflush", "tcgetattr",
            "tcsendbreak", "tcsetattr", "telldir", "tempnam", "time", "timegm", "tmpfile", "tmpnam", "tolower",
            "toupper", "towlower", "towupper", "truncate", "ttyname", "tzset", "umask", "uname", "ungetc",
            "unlink", "unlinkat", "unsetenv", "uselocale", "usleep", "utime", "utimensat", "utimes", "vasprintf",
            "vdprintf", "vfork", "vfprintf", "vfscanf", "vprintf", "vscanf", "vsnprintf", "vsprintf", "vsscanf",
            "vsyslog", "wait", "wait3", "wait4", "waitpid", "wcrtomb", "wcschr", "wcscmp", "wcscoll", "wcscpy",
            "wcscspn", "wcslen", "wcsncat", "wcsncmp", "wcsncpy", "wcspbrk", "wcsrchr", "wcsrtombs", "wcsstr",
            "wcstod", "wcstol", "wcstombs", "wcstoul", "wctomb", "wcwidth", "wmemchr", "wmemcmp", "wmemcpy",
            "wmemmove", "wmemset", "write", "writev",
        ),
        "libdl.so" to setOf(
            "android_dlopen_ext", "dl_iterate_phdr", "dladdr", "dlclose", "dlerror", "dlinfo", "dlopen", "dlsym",
        ),
        "libm.so" to setOf(
            "acos", "acosf", "acosh", "acoshf", "asin", "asinf", "asinh", "asinhf", "atan", "atan2", "atan2f",
            "atanf", "atanh", "atanhf", "cabs", "cabsf", "cacos", "cacosf", "cacosh", "cacoshf", "carg", "cargf",
            "casin", "casinf", "casinh", "casinhf", "catan", "catanf", "catanh", "catanhf", "cbrt", "cbrtf", "ccos",
            "ccosf", "ccosh", "ccoshf", "ceil", "ceilf", "cexp", "cexpf", "cimag", "cimagf", "clog", "clogf", "conj",
            "conjf", "copysign", "copysignf", "cos", "cosf", "cosh", "coshf", "cpow", "cpowf", "cproj", "cprojf",
            "creal", "crealf", "csin", "csinf", "csinh", "csinhf", "csqrt", "csqrtf", "ctan", "ctanf", "ctanh",
            "ctanhf", "erf", "erfc", "erfcf", "erff", "exp", "exp2", "exp2f", "expf", "expm1", "expm1f", "fabs",
            "fabsf", "fdim", "fdimf", "floor", "floorf", "fma", "fmaf", "fmax", "fmaxf", "fmin", "fminf", "fmod",
            "fmodf", "frexp", "frexpf", "hypot", "hypotf", "ilogb", "ilogbf", "j0", "j1", "jn", "ldexpf",
            "lgamma", "lgamma_r", "lgammaf", "lgammaf_r", "llrint", "llrintf", "llround", "llroundf", "log", "log10",
            "log10f", "log1p", "log1pf", "log2", "log2f", "logb", "logbf", "logf", "lrint", "lrintf", "lround",
            "lroundf", "modf", "modff", "nan", "nanf", "nearbyint", "nearbyintf", "nextafter", "nextafterf",
            "nexttoward", "pow", "powf", "remainder", "remainderf", "remquo", "remquof", "rint", "rintf", "round",
            "roundf", "scalbn", "scalbnf", "significand", "sin", "sinf", "sinh", "sinhf", "sqrt", "sqrtf", "tan",
            "tanf", "tanh", "tanhf", "tgamma", "tgammaf", "trunc", "truncf", "y0", "y1", "yn",
        ),
        "libz.so" to setOf(
            "adler32", "adler32_combine", "adler32_z", "compress", "compress2", "compressBound", "crc32",
            "crc32_combine", "crc32_z", "deflate", "deflateBound", "deflateCopy", "deflateEnd",
            "deflateGetDictionary", "deflateInit2_", "deflateInit_", "deflateParams", "deflatePending",
            "deflatePrime", "deflateReset", "deflateResetKeep", "deflateSetDictionary", "deflateSetHeader",
            "deflateTune", "gzclearerr", "gzclose", "gzeof", "gzerror", "gzflush", "gzgetc", "gzgets", "gzoffset",
            "gzopen", "gzprintf", "gzputc", "gzputs", "gzread", "gzrewind", "gzseek", "gztell", "gzungetc", "gzwrite",
            "inflate", "inflateBack", "inflateBackEnd", "inflateBackInit_", "inflateCodesUsed", "inflateCopy",
            "inflateEnd", "inflateGetDictionary", "inflateGetHeader", "inflateInit2_", "inflateInit_", "inflatePrime",
            "inflateReset", "inflateReset2", "inflateResetKeep", "inflateSetDictionary", "inflateSync",
            "inflateSyncPoint", "inflateUndermine", "inflateValidate", "uncompress", "uncompress2", "unzClose",
            "unzOpen", "unzReadCurrentFile", "zlibCompileFlags", "zlibVersion",
        ),
        "libEGL.so" to setOf(
            "eglBindAPI", "eglBindTexImage", "eglChooseConfig", "eglClientWaitSyncKHR", "eglCopyBuffers",
            "eglCreateContext", "eglCreateImageKHR", "eglCreatePbufferFromClientBuffer", "eglCreatePbufferSurface",
            "eglCreatePixmapSurface", "eglCreatePlatformPixmapSurface", "eglCreatePlatformWindowSurface",
            "eglCreateSyncKHR", "eglCreateWindowSurface", "eglDestroyContext", "eglDestroyImageKHR",
            "eglDestroySurface", "eglDestroySyncKHR", "eglGetConfigAttrib", "eglGetConfigs", "eglGetCurrentContext",
            "eglGetCurrentDisplay", "eglGetCurrentSurface", "eglGetDisplay", "eglGetError", "eglGetPlatformDisplay",
            "eglGetProcAddress", "eglGetSyncAttribKHR", "eglInitialize", "eglMakeCurrent",
            "eglPresentationTimeANDROID", "eglQueryAPI", "eglQueryContext", "eglQueryString", "eglQuerySurface",
            "eglQuerySurfacePointerANGLE", "eglReleaseTexImage", "eglReleaseThread", "eglSetDamageRegionKHR",
            "eglSignalSyncKHR", "eglSurfaceAttrib", "eglSwapBuffers", "eglSwapBuffersWithDamageKHR",
            "eglSwapInterval", "eglTerminate", "eglWaitClient", "eglWaitGL", "eglWaitNative", "eglWaitSyncKHR",
        ),
        "libGLESv1_CM.so" to setOf(
            "glAlphaFunc", "glAlphaFuncx", "glBindFramebufferOES", "glBindRenderbufferOES", "glBlendEquationOES",
            "glBlendEquationSeparateOES", "glBlendFuncSeparateOES", "glCheckFramebufferStatusOES", "glClearColorx",
            "glClientActiveTexture", "glClipPlanef", "glClipPlanex", "glColor4f", "glColor4ub", "glColor4x",
            "glColorPointer", "glDeleteFramebuffersOES", "glDeleteRenderbuffersOES", "glDisableClientState",
            "glDrawTexfOES", "glDrawTexfvOES", "glDrawTexiOES", "glDrawTexivOES", "glDrawTexsOES",
            "glDrawTexsvOES", "glDrawTexxOES", "glDrawTexxvOES", "glEGLImageTargetRenderbufferStorageOES",
            "glEGLImageTargetTexture2DOES", "glEnableClientState", "glFogf", "glFogfv", "glFogi", "glFogiv",
            "glFogx", "glFogxv", "glFramebufferRenderbufferOES", "glFramebufferTexture2DOES", "glFrustumf",
            "glFrustumx", "glGenFramebuffersOES", "glGenRenderbuffersOES", "glGenerateMipmapOES", "glGetClipPlane",
            "glGetClipPlanef", "glGetClipPlanex", "glGetFixedv", "glGetLightfv", "glGetLightiv", "glGetLightxv",
            "glGetMaterialfv", "glGetMaterialiv", "glGetMaterialxv", "glGetPointerv",
            "glGetRenderbufferParameterivOES", "glGetTexEnviv", "glGetTexEnvxv", "glGetTexGenfvOES",
            "glGetTexGenivOES", "glGetTexGenxvOES", "glGetTexParameterxv", "glIsFramebufferOES",
            "glIsRenderbufferOES", "glLightModelf", "glLightModelfv", "glLightModelx", "glLightModelxv",
            "glLightf", "glLightfv", "glLighti", "glLightiv", "glLightx", "glLightxv", "glLineWidthx",
            "glLoadIdentity", "glLoadMatrixf", "glLoadMatrixx", "glLogicOp", "glMaterialf", "glMaterialfv",
            "glMaterialx", "glMaterialxv", "glMatrixMode", "glMultMatrixf", "glMultMatrixx", "glMultiTexCoord4f",
            "glMultiTexCoord4x", "glNormal3f", "glNormal3x", "glNormalPointer", "glOrthof", "glOrthox",
            "glPointParameterf", "glPointParameterfv", "glPointSize", "glPointSizePointerOES", "glPointSizex",
            "glPolygonOffsetx", "glPopMatrix", "glPushMatrix", "glQueryMatrixxOES", "glRenderbufferStorageOES",
            "glRotatef", "glRotatex", "glSampleCoveragex", "glScalef", "glScalex", "glShadeModel",
            "glTexCoordPointer", "glTexEnvf", "glTexEnvfv", "glTexEnvi", "glTexEnviv", "glTexEnvx", "glTexEnvxv",
            "glTexGenfOES", "glTexGenfvOES", "glTexGeniOES", "glTexGenivOES", "glTexGenxOES", "glTexGenxvOES",
            "glTexParameterx", "glTexParameterxv", "glTranslatef", "glTranslatex", "glVertexPointer",
        ),
        "libGLESv2.so" to setOf(
            "glActiveTexture", "glAttachShader", "glBeginQueryEXT", "glBindAttribLocation", "glBindBuffer",
            "glBindFramebuffer", "glBindRenderbuffer", "glBindTexture", "glBlendColor", "glBlendEquation",
            "glBlendEquationSeparate", "glBlendFunc", "glBlendFuncSeparate", "glBufferData", "glBufferSubData",
            "glCheckFramebufferStatus", "glClear", "glClearColor", "glClearDepthf", "glClearStencil", "glColorMask",
            "glCompileShader", "glCompressedTexImage2D", "glCompressedTexSubImage2D", "glCopyTexImage2D",
            "glCopyTexSubImage2D", "glCreateProgram", "glCreateShader", "glCullFace", "glDeleteBuffers",
            "glDeleteFramebuffers", "glDeleteProgram", "glDeleteQueriesEXT", "glDeleteRenderbuffers",
            "glDeleteShader", "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDepthRangef", "glDetachShader",
            "glDisable", "glDisableVertexAttribArray", "glDiscardFramebufferEXT", "glDrawArrays", "glDrawElements",
            "glEnable", "glEnableVertexAttribArray", "glEndQueryEXT", "glFinish", "glFlush",
            "glFramebufferRenderbuffer", "glFramebufferTexture2D", "glFrontFace", "glGenBuffers", "glGenFramebuffers",
            "glGenQueriesEXT", "glGenRenderbuffers", "glGenTextures", "glGenerateMipmap", "glGetActiveAttrib",
            "glGetActiveUniform", "glGetAttribLocation", "glGetBooleanv", "glGetBufferParameteriv", "glGetError",
            "glGetFloatv", "glGetFramebufferAttachmentParameteriv", "glGetIntegerv", "glGetProgramBinaryOES",
            "glGetProgramInfoLog", "glGetProgramiv", "glGetQueryObjectuivEXT", "glGetQueryivEXT",
            "glGetRenderbufferParameteriv", "glGetShaderInfoLog", "glGetShaderPrecisionFormat", "glGetShaderiv",
            "glGetString", "glGetTexParameterfv", "glGetTexParameteriv", "glGetUniformLocation", "glGetUniformfv",
            "glGetUniformiv", "glGetVertexAttribPointerv", "glGetVertexAttribfv", "glGetVertexAttribiv", "glHint",
            "glIsBuffer", "glIsEnabled", "glIsFramebuffer", "glIsProgram", "glIsQueryEXT", "glIsRenderbuffer",
            "glIsShader", "glIsTexture", "glLineWidth", "glLinkProgram", "glPixelStorei", "glPolygonOffset",
            "glProgramBinaryOES", "glReadPixels", "glReleaseShaderCompiler", "glRenderbufferStorage",
            "glSampleCoverage", "glScissor", "glShaderBinary", "glShaderSource", "glStencilFunc",
            "glStencilFuncSeparate", "glStencilMask", "glStencilMaskSeparate", "glStencilOp", "glStencilOpSeparate",
            "glTexImage2D", "glTexParameterf", "glTexParameterfv", "glTexParameteri", "glTexParameteriv",
            "glTexSubImage2D", "glUniform1f", "glUniform1fv", "glUniform1i", "glUniform1iv", "glUniform2f",
            "glUniform2fv", "glUniform2i", "glUniform2iv", "glUniform3f", "glUniform3fv", "glUniform3i",
            "glUniform3iv", "glUniform4f", "glUniform4fv", "glUniform4i", "glUniform4iv", "glUniformMatrix2fv",
            "glUniformMatrix3fv", "glUniformMatrix4fv", "glUseProgram", "glValidateProgram", "glVertexAttrib1f",
            "glVertexAttrib1fv", "glVertexAttrib2f", "glVertexAttrib2fv", "glVertexAttrib3f", "glVertexAttrib3fv",
            "glVertexAttrib4f", "glVertexAttrib4fv", "glVertexAttribPointer", "glViewport",
        ),
        "liblog.so" to setOf(
            "__android_log_assert", "__android_log_buf_print", "__android_log_buf_write", "__android_log_is_loggable",
            "__android_log_print", "__android_log_vprint", "__android_log_write",
        ),
        "libandroid.so" to setOf(
            "AAssetManager_fromJava", "AAssetManager_open", "AAssetManager_openDir", "AAsset_close",
            "AAsset_getBuffer", "AAsset_getLength", "AAsset_getLength64", "AAsset_getRemainingLength",
            "AAsset_getRemainingLength64", "AAsset_isAllocated", "AAsset_openFileDescriptor",
            "AAsset_openFileDescriptor64", "AAsset_read", "AAsset_seek", "AAsset_seek64", "AConfiguration_delete",
            "AConfiguration_fromAssetManager", "AConfiguration_getCountry", "AConfiguration_getLanguage",
            "AConfiguration_new", "AInputQueue_attachLooper", "AInputQueue_detachLooper", "AInputQueue_finishEvent",
            "AInputQueue_getEvent", "AInputQueue_preDispatchEvent", "ALooper_addFd", "ALooper_forThread",
            "ALooper_pollAll", "ALooper_pollOnce", "ALooper_prepare", "ALooper_removeFd", "ALooper_wake",
            "ANativeActivity_finish", "ANativeActivity_setWindowFlags", "ANativeWindow_acquire",
            "ANativeWindow_fromSurface", "ANativeWindow_getFormat", "ANativeWindow_getHeight",
            "ANativeWindow_getWidth", "ANativeWindow_lock", "ANativeWindow_release",
            "ANativeWindow_setBuffersGeometry", "ANativeWindow_unlockAndPost", "ASensorEventQueue_disableSensor",
            "ASensorEventQueue_enableSensor", "ASensorEventQueue_getEvents", "ASensorEventQueue_hasEvents",
            "ASensorEventQueue_setEventRate", "ASensorManager_createEventQueue", "ASensorManager_destroyEventQueue",
            "ASensorManager_getDefaultSensor", "ASensorManager_getInstance", "ASensorManager_getSensorList",
        ),
        "libOpenSLES.so" to setOf(
            "slCreateEngine",
        ),
        "libmediandk.so" to setOf(
            "AMediaCodec_configure", "AMediaCodec_createDecoderByType", "AMediaCodec_createInputBuffer",
            "AMediaCodec_createOutputBuffer", "AMediaCodec_delete", "AMediaCodec_dequeueInputBuffer",
            "AMediaCodec_dequeueOutputBuffer", "AMediaCodec_flush", "AMediaCodec_getInputBuffer",
            "AMediaCodec_getOutputBuffer", "AMediaCodec_getOutputFormat", "AMediaCodec_queueInputBuffer",
            "AMediaCodec_releaseInputBuffer", "AMediaCodec_releaseOutputBuffer", "AMediaCodec_start",
            "AMediaCodec_stop", "AMediaExtractor_advance", "AMediaExtractor_create", "AMediaExtractor_getSampleFlags",
            "AMediaExtractor_getSampleTime", "AMediaExtractor_getSampleTrackIndex", "AMediaExtractor_getTrackCount",
            "AMediaExtractor_getTrackFormat", "AMediaExtractor_readSampleData", "AMediaExtractor_release",
            "AMediaExtractor_seekTo", "AMediaExtractor_setDataSource", "AMediaFormat_create", "AMediaFormat_delete",
            "AMediaFormat_getInt32", "AMediaFormat_getString", "AMediaFormat_setBuffer", "AMediaFormat_setInt32",
            "AMediaFormat_setString",
        ),
        "libjnigraphics.so" to setOf(
            "AndroidBitmap_getInfo", "AndroidBitmap_lockPixels", "AndroidBitmap_unlockPixels",
        ),
        "libvulkan.so" to setOf(
            "vkAcquireNextImageKHR", "vkAllocateCommandBuffers", "vkAllocateMemory", "vkBeginCommandBuffer",
            "vkCmdBindPipeline", "vkCmdDraw", "vkCmdDrawIndexed", "vkCreateBuffer", "vkCreateCommandPool",
            "vkCreateDevice", "vkCreateFramebuffer", "vkCreateGraphicsPipelines", "vkCreateImage",
            "vkCreateImageView", "vkCreateInstance", "vkCreateRenderPass", "vkCreateShaderModule",
            "vkCreateSwapchainKHR", "vkDestroyBuffer", "vkDestroyCommandPool", "vkDestroyDevice",
            "vkDestroyFramebuffer", "vkDestroyImage", "vkDestroyImageView", "vkDestroyInstance", "vkDestroyPipeline",
            "vkDestroyRenderPass", "vkDestroyShaderModule", "vkDestroySwapchainKHR", "vkDeviceWaitIdle",
            "vkEndCommandBuffer", "vkEnumeratePhysicalDevices", "vkFreeCommandBuffers", "vkFreeMemory",
            "vkGetDeviceQueue", "vkGetPhysicalDeviceProperties", "vkGetSwapchainImagesKHR", "vkQueuePresentKHR",
            "vkQueueSubmit", "vkQueueWaitIdle",
        ),
        "libaaudio.so" to setOf(
            "AAudioStreamBuilder_delete", "AAudioStreamBuilder_openStream",
            "AAudioStreamBuilder_setBufferCapacityInFrames", "AAudioStreamBuilder_setChannelCount",
            "AAudioStreamBuilder_setDataCallback", "AAudioStreamBuilder_setDirection",
            "AAudioStreamBuilder_setErrorCallback", "AAudioStreamBuilder_setFormat",
            "AAudioStreamBuilder_setPerformanceMode", "AAudioStreamBuilder_setSampleRate",
            "AAudioStreamBuilder_setSharingMode", "AAudioStream_close", "AAudioStream_getChannelCount",
            "AAudioStream_getFormat", "AAudioStream_getSampleRate", "AAudioStream_getState",
            "AAudioStream_getXRunCount", "AAudioStream_read", "AAudioStream_requestFlush",
            "AAudioStream_requestPause", "AAudioStream_requestStart", "AAudioStream_requestStop",
            "AAudioStream_write", "AAudio_createStreamBuilder",
        ),
        "libc++_shared.so" to setOf(
            "_ZSt9terminatev", "_ZTVN10__cxxabiv117__class_type_infoE",
            "_ZTVN10__cxxabiv119__pointer_type_infoE", "_ZTVN10__cxxabiv120__si_class_type_infoE",
            "_ZTVN10__cxxabiv121__vmi_class_type_infoE", "_ZdaPv", "_ZdlPv", "_Znam", "_Znwm",
            "__cxa_allocate_exception", "__cxa_begin_catch", "__cxa_demangle", "__cxa_end_catch",
            "__cxa_free_exception", "__cxa_guard_abort", "__cxa_guard_acquire", "__cxa_guard_release",
            "__cxa_pure_virtual", "__cxa_rethrow", "__cxa_throw", "__dynamic_cast",
        )
    )

    /** These need source-level/ABI rewrites; none are direct native symbol aliases. */
    private val semanticTargets = mapOf(
        "UIApplication" to "android.app.Application + Activity lifecycle",
        "UIViewController" to "android.app.Activity or androidx.fragment.app.Fragment",
        "UIView" to "android.view.View",
        "UIWindow" to "android.view.Window",
        "UILabel" to "android.widget.TextView",
        "UIButton" to "android.widget.Button",
        "UIImage" to "android.graphics.Bitmap or android.graphics.drawable.Drawable",
        "UIImageView" to "android.widget.ImageView",
        "UIScrollView" to "android.widget.ScrollView",
        "UITableView" to "RecyclerView + LayoutManager + Adapter",
        "UICollectionView" to "RecyclerView + LayoutManager + Adapter",
        "UITextField" to "android.widget.EditText",
        "UITextView" to "android.widget.EditText or android.widget.TextView",
        "UIScreen" to "android.util.DisplayMetrics + WindowManager",
        "UIColor" to "android.graphics.Color",
        "UIFont" to "android.graphics.Typeface",
        "NSString" to "java.lang.String",
        "NSArray" to "java.util.List",
        "NSDictionary" to "java.util.Map",
        "NSData" to "byte[]",
        "NSBundle" to "Android assets/resources + package metadata",
        "NSFileManager" to "java.io.File + Android scoped-storage APIs",
        "NSUserDefaults" to "android.content.SharedPreferences or DataStore",
        "NSNotificationCenter" to "Lifecycle-aware callbacks or Android broadcasts",
        "NSTimer" to "Handler, ScheduledExecutorService, or Choreographer",
        "NSURLSession" to "HttpURLConnection or an approved Android HTTP client",
        "NSJSONSerialization" to "org.json or kotlinx.serialization",
        "AVAudioPlayer" to "android.media.MediaPlayer or SoundPool",
        "AVAudioSession" to "android.media.AudioManager + AudioAttributes",
        "CADisplayLink" to "C callback bridge driven by Android Choreographer (not the Objective-C CADisplayLink ABI)",
        "CAAnimation" to "android.animation.Animator",
        "SKScene" to "custom SurfaceView/Canvas renderer (game-loop rewrite required)",
    )

    /**
     * Real, host-tested implementation bodies exported by libioscompat.so.
     *
     * The four time shims live in native/src/apple_time_compat.cpp; other C,
     * POSIX and CoreFoundation entries come from IOSTODROID_IOS_SHIM_TABLE in
     * native/include/iostodroid_ios_shims.h, which is also expanded by
     * native/src/ioscompat_registry.cpp and native/src/jni.cpp. A dlsym hit here
     * proves an export exists; it does not rewrite an IPA callsite.
     */
    private val implementedApiReplacements = mapOf(
        "_CFAbsoluteTimeGetCurrent" to "CFAbsoluteTimeGetCurrent",
        "_CACurrentMediaTime" to "CACurrentMediaTime",
        "_mach_absolute_time" to "mach_absolute_time",
        "_mach_timebase_info" to "mach_timebase_info",
        "_CFAllocatorGetDefault" to "iostodroid_compat_CFAllocatorGetDefault",
        "_CFRetain" to "iostodroid_compat_CFRetain",
        "_CFRelease" to "iostodroid_compat_CFRelease",
        "_CFGetRetainCount" to "iostodroid_compat_CFGetRetainCount",
        "_CFStringCreateWithCString" to "iostodroid_compat_CFStringCreateWithCString",
        "_CFStringGetLength" to "iostodroid_compat_CFStringGetLength",
        "_CFStringGetCString" to "iostodroid_compat_CFStringGetCString",
        "_CFStringGetCStringPtr" to "iostodroid_compat_CFStringGetCStringPtr",
        "_CFStringGetMaximumSizeForEncoding" to "iostodroid_compat_CFStringGetMaximumSizeForEncoding",
        "_CFStringCompare" to "iostodroid_compat_CFStringCompare",
        "_CFStringGetSystemEncoding" to "iostodroid_compat_CFStringGetSystemEncoding",
        "_CFDataCreate" to "iostodroid_compat_CFDataCreate",
        "_CFDataGetBytePtr" to "iostodroid_compat_CFDataGetBytePtr",
        "_CFDataGetLength" to "iostodroid_compat_CFDataGetLength",
        "_CFArrayCreateMutable" to "iostodroid_compat_CFArrayCreateMutable",
        "_CFArrayAppendValue" to "iostodroid_compat_CFArrayAppendValue",
        "_CFArrayGetCount" to "iostodroid_compat_CFArrayGetCount",
        "_CFArrayGetValueAtIndex" to "iostodroid_compat_CFArrayGetValueAtIndex",
        "_CFDictionaryCreateMutable" to "iostodroid_compat_CFDictionaryCreateMutable",
        "_CFDictionarySetValue" to "iostodroid_compat_CFDictionarySetValue",
        "_CFDictionaryGetValue" to "iostodroid_compat_CFDictionaryGetValue",
        "_CFDictionaryGetCount" to "iostodroid_compat_CFDictionaryGetCount",
        "_CFNumberCreate" to "iostodroid_compat_CFNumberCreate",
        "_CFNumberGetValue" to "iostodroid_compat_CFNumberGetValue",
        "_CFDateCreate" to "iostodroid_compat_CFDateCreate",
        "_CFDateGetAbsoluteTime" to "iostodroid_compat_CFDateGetAbsoluteTime",
        "_CFDateGetTimeIntervalSinceDate" to "iostodroid_compat_CFDateGetTimeIntervalSinceDate",
        "_CFAbsoluteTimeGetGregorianDate" to "iostodroid_compat_CFAbsoluteTimeGetGregorianDate",
        "_CFRunLoopGetCurrent" to "iostodroid_compat_CFRunLoopGetCurrent",
        "_CFRunLoopGetMain" to "iostodroid_compat_CFRunLoopGetMain",
        "_CFRunLoopRun" to "iostodroid_compat_CFRunLoopRun",
        "_CFRunLoopRunInMode" to "iostodroid_compat_CFRunLoopRunInMode",
        "_CFRunLoopStop" to "iostodroid_compat_CFRunLoopStop",
        "_CFRunLoopWakeUp" to "iostodroid_compat_CFRunLoopWakeUp",
        "_malloc" to "iostodroid_compat_malloc",
        "_calloc" to "iostodroid_compat_calloc",
        "_realloc" to "iostodroid_compat_realloc",
        "_free" to "iostodroid_compat_free",
        "_memcpy" to "iostodroid_compat_memcpy",
        "_memmove" to "iostodroid_compat_memmove",
        "_memset" to "iostodroid_compat_memset",
        "_memcmp" to "iostodroid_compat_memcmp",
        "_memchr" to "iostodroid_compat_memchr",
        "_strlen" to "iostodroid_compat_strlen",
        "_strcpy" to "iostodroid_compat_strcpy",
        "_strncpy" to "iostodroid_compat_strncpy",
        "_strlcpy" to "iostodroid_compat_strlcpy",
        "_strlcat" to "iostodroid_compat_strlcat",
        "_strcmp" to "iostodroid_compat_strcmp",
        "_strncmp" to "iostodroid_compat_strncmp",
        "_strdup" to "iostodroid_compat_strdup",
        "_strchr" to "iostodroid_compat_strchr",
        "_strrchr" to "iostodroid_compat_strrchr",
        "_strstr" to "iostodroid_compat_strstr",
        "_strtol" to "iostodroid_compat_strtol",
        "_strtod" to "iostodroid_compat_strtod",
        "_atoi" to "iostodroid_compat_atoi",
        "_atof" to "iostodroid_compat_atof",
        "_strerror" to "iostodroid_compat_strerror",
        "_snprintf" to "iostodroid_compat_snprintf",
        "_vsnprintf" to "iostodroid_compat_vsnprintf",
        "_fopen" to "iostodroid_compat_fopen",
        "_fclose" to "iostodroid_compat_fclose",
        "_fread" to "iostodroid_compat_fread",
        "_fwrite" to "iostodroid_compat_fwrite",
        "_fputs" to "iostodroid_compat_fputs",
        "_fgets" to "iostodroid_compat_fgets",
        "_fflush" to "iostodroid_compat_fflush",
        "_fprintf" to "iostodroid_compat_fprintf",
        "_printf" to "iostodroid_compat_printf",
        "_puts" to "iostodroid_compat_puts",
        "_remove" to "iostodroid_compat_remove",
        "_feof" to "iostodroid_compat_feof",
        "_ftell" to "iostodroid_compat_ftell",
        "_fseek" to "iostodroid_compat_fseek",
        "_time" to "iostodroid_compat_time",
        "_gettimeofday" to "iostodroid_compat_gettimeofday",
        "_clock_gettime" to "iostodroid_compat_clock_gettime",
        "_nanosleep" to "iostodroid_compat_nanosleep",
        "_localtime_r" to "iostodroid_compat_localtime_r",
        "_gmtime_r" to "iostodroid_compat_gmtime_r",
        "_mktime" to "iostodroid_compat_mktime",
        "_getenv" to "iostodroid_compat_getenv",
        "_setenv" to "iostodroid_compat_setenv",
        "_unsetenv" to "iostodroid_compat_unsetenv",
        "_getpid" to "iostodroid_compat_getpid",
        "_qsort" to "iostodroid_compat_qsort",
        "_bsearch" to "iostodroid_compat_bsearch",
        "_abs" to "iostodroid_compat_abs",
        "_labs" to "iostodroid_compat_labs",
        "_rand" to "iostodroid_compat_rand",
        "_srand" to "iostodroid_compat_srand",
        "_sqrt" to "iostodroid_compat_sqrt",
        "_fabs" to "iostodroid_compat_fabs",
        "_floor" to "iostodroid_compat_floor",
        "_ceil" to "iostodroid_compat_ceil",
        "_pow" to "iostodroid_compat_pow",
        "_sin" to "iostodroid_compat_sin",
        "_cos" to "iostodroid_compat_cos",
        "_tan" to "iostodroid_compat_tan",
        "_atan2" to "iostodroid_compat_atan2",
        "_fmod" to "iostodroid_compat_fmod",
        "_pthread_mutex_init" to "iostodroid_compat_pthread_mutex_init",
        "_pthread_mutex_lock" to "iostodroid_compat_pthread_mutex_lock",
        "_pthread_mutex_unlock" to "iostodroid_compat_pthread_mutex_unlock",
        "_pthread_mutex_destroy" to "iostodroid_compat_pthread_mutex_destroy",
        "_pthread_cond_init" to "iostodroid_compat_pthread_cond_init",
        "_pthread_cond_wait" to "iostodroid_compat_pthread_cond_wait",
        "_pthread_cond_signal" to "iostodroid_compat_pthread_cond_signal",
        "_pthread_cond_broadcast" to "iostodroid_compat_pthread_cond_broadcast",
        "_pthread_cond_destroy" to "iostodroid_compat_pthread_cond_destroy",
        "_pthread_self" to "iostodroid_compat_pthread_self",
        "___CFConstantStringClassReference" to "iostodroid_compat_CFConstantStringClassReference",
        "_kCFAllocatorDefault" to "iostodroid_compat_CFAllocatorDefault",
        "_kCFBooleanTrue" to "iostodroid_compat_CFBooleanTrue",
        "_kCFBooleanFalse" to "iostodroid_compat_CFBooleanFalse",
        "_kCFTypeArrayCallBacks" to "iostodroid_compat_CFTypeArrayCallBacks",
        "_kCFTypeDictionaryKeyCallBacks" to "iostodroid_compat_CFTypeDictionaryKeyCallBacks",
        "_kCFTypeDictionaryValueCallBacks" to "iostodroid_compat_CFTypeDictionaryValueCallBacks",
        "_kCFRunLoopDefaultMode" to "iostodroid_compat_CFRunLoopDefaultMode",
        "_kCFRunLoopCommonModes" to "iostodroid_compat_CFRunLoopCommonModes",
        "_CFBundleGetMainBundle" to "iostodroid_compat_CFBundleGetMainBundle",
        "_CFBundleCopyBundleURL" to "iostodroid_compat_CFBundleCopyBundleURL",
        "_CFBundleCopyResourcesDirectoryURL" to "iostodroid_compat_CFBundleCopyResourcesDirectoryURL",
        "_CFBundleCopyResourceURL" to "iostodroid_compat_CFBundleCopyResourceURL",
        "_CFBundleGetIdentifier" to "iostodroid_compat_CFBundleGetIdentifier",
        "_CFBundleGetValueForInfoDictionaryKey" to "iostodroid_compat_CFBundleGetValueForInfoDictionaryKey",
        "_CFURLCreateWithFileSystemPath" to "iostodroid_compat_CFURLCreateWithFileSystemPath",
        "_CFURLCreateFromFileSystemRepresentation" to "iostodroid_compat_CFURLCreateFromFileSystemRepresentation",
        "_CFURLGetFileSystemRepresentation" to "iostodroid_compat_CFURLGetFileSystemRepresentation",
        "_CFURLCopyFileSystemPath" to "iostodroid_compat_CFURLCopyFileSystemPath",
        "_CFStringCreateWithBytes" to "iostodroid_compat_CFStringCreateWithBytes",
        "_CFStringCreateMutable" to "iostodroid_compat_CFStringCreateMutable",
        "_CFStringAppendCString" to "iostodroid_compat_CFStringAppendCString",
        "_CFStringHasPrefix" to "iostodroid_compat_CFStringHasPrefix",
        "_CFStringHasSuffix" to "iostodroid_compat_CFStringHasSuffix",
        "_CFStringGetIntValue" to "iostodroid_compat_CFStringGetIntValue",
        "_CFStringGetDoubleValue" to "iostodroid_compat_CFStringGetDoubleValue",
        "_CFArrayCreate" to "iostodroid_compat_CFArrayCreate",
        "_CFArrayRemoveValueAtIndex" to "iostodroid_compat_CFArrayRemoveValueAtIndex",
        "_CFArrayRemoveAllValues" to "iostodroid_compat_CFArrayRemoveAllValues",
        "_CFDictionaryCreate" to "iostodroid_compat_CFDictionaryCreate",
        "_CFDictionaryRemoveValue" to "iostodroid_compat_CFDictionaryRemoveValue",
        "_CFDictionaryRemoveAllValues" to "iostodroid_compat_CFDictionaryRemoveAllValues",
        "_CFDictionaryContainsKey" to "iostodroid_compat_CFDictionaryContainsKey",
        "_CFDataCreateMutable" to "iostodroid_compat_CFDataCreateMutable",
        "_CFDataAppendBytes" to "iostodroid_compat_CFDataAppendBytes",
        "_CFDataGetMutableBytePtr" to "iostodroid_compat_CFDataGetMutableBytePtr",
        "_CFDataGetBytes" to "iostodroid_compat_CFDataGetBytes",
        "_CFBooleanGetValue" to "iostodroid_compat_CFBooleanGetValue",
        "_CFEqual" to "iostodroid_compat_CFEqual",
        "_CFHash" to "iostodroid_compat_CFHash",
        "_CFGetTypeID" to "iostodroid_compat_CFGetTypeID",
        "_CFPreferencesCopyAppValue" to "iostodroid_compat_CFPreferencesCopyAppValue",
        "_CFPreferencesSetAppValue" to "iostodroid_compat_CFPreferencesSetAppValue",
        "_CFPreferencesAppSynchronize" to "iostodroid_compat_CFPreferencesAppSynchronize",
        "_CFUUIDCreate" to "iostodroid_compat_CFUUIDCreate",
        "_CFUUIDCreateString" to "iostodroid_compat_CFUUIDCreateString",
        "_CFLocaleCopyCurrent" to "iostodroid_compat_CFLocaleCopyCurrent",
        "_CFLocaleCopyPreferredLanguages" to "iostodroid_compat_CFLocaleCopyPreferredLanguages",
        "_CFLocaleGetIdentifier" to "iostodroid_compat_CFLocaleGetIdentifier",
        "_CFTimeZoneCopySystem" to "iostodroid_compat_CFTimeZoneCopySystem",
        "_AudioSessionInitialize" to "iostodroid_compat_AudioSessionInitialize",
        "_AudioSessionSetActive" to "iostodroid_compat_AudioSessionSetActive",
        "_NSSearchPathForDirectoriesInDomains" to "iostodroid_compat_NSSearchPathForDirectoriesInDomains",
        "_OBJC_CLASS_\$_CAEAGLLayer" to "iostodroid_compat_OBJC_CLASS___CAEAGLLayer",
        "_OBJC_CLASS_\$_EAGLContext" to "iostodroid_compat_OBJC_CLASS___EAGLContext",
        "_OBJC_CLASS_\$_NSAutoreleasePool" to "iostodroid_compat_OBJC_CLASS___NSAutoreleasePool",
        "_OBJC_CLASS_\$_NSBundle" to "iostodroid_compat_OBJC_CLASS___NSBundle",
        "_OBJC_CLASS_\$_NSDictionary" to "iostodroid_compat_OBJC_CLASS___NSDictionary",
        "_OBJC_CLASS_\$_NSNumber" to "iostodroid_compat_OBJC_CLASS___NSNumber",
        "_OBJC_CLASS_\$_NSObject" to "iostodroid_compat_OBJC_CLASS___NSObject",
        "_OBJC_CLASS_\$_NSString" to "iostodroid_compat_OBJC_CLASS___NSString",
        "_OBJC_CLASS_\$_NSThread" to "iostodroid_compat_OBJC_CLASS___NSThread",
        "_OBJC_CLASS_\$_NSURL" to "iostodroid_compat_OBJC_CLASS___NSURL",
        "_OBJC_CLASS_\$_UIAccelerometer" to "iostodroid_compat_OBJC_CLASS___UIAccelerometer",
        "_OBJC_CLASS_\$_UIApplication" to "iostodroid_compat_OBJC_CLASS___UIApplication",
        "_OBJC_CLASS_\$_UIScreen" to "iostodroid_compat_OBJC_CLASS___UIScreen",
        "_OBJC_CLASS_\$_UIView" to "iostodroid_compat_OBJC_CLASS___UIView",
        "_OBJC_CLASS_\$_UIWindow" to "iostodroid_compat_OBJC_CLASS___UIWindow",
        "_OBJC_METACLASS_\$_NSObject" to "iostodroid_compat_OBJC_METACLASS___NSObject",
        "_OBJC_METACLASS_\$_UIView" to "iostodroid_compat_OBJC_METACLASS___UIView",
        "_UIApplicationMain" to "iostodroid_compat_UIApplicationMain",
        "__DefaultRuneLocale" to "iostodroid_compat__DefaultRuneLocale",
        "__Unwind_SjLj_Register" to "iostodroid_compat__Unwind_SjLj_Register",
        "__Unwind_SjLj_Resume" to "iostodroid_compat__Unwind_SjLj_Resume",
        "__Unwind_SjLj_Unregister" to "iostodroid_compat__Unwind_SjLj_Unregister",
        "__ZSt9terminatev" to "iostodroid_compat__ZSt9terminatev",
        "__ZTVN10__cxxabiv117__class_type_infoE" to "iostodroid_compat__ZTVN10__cxxabiv117__class_type_infoE",
        "__ZTVN10__cxxabiv119__pointer_type_infoE" to "iostodroid_compat__ZTVN10__cxxabiv119__pointer_type_infoE",
        "__ZTVN10__cxxabiv120__si_class_type_infoE" to "iostodroid_compat__ZTVN10__cxxabiv120__si_class_type_infoE",
        "__ZTVN10__cxxabiv121__vmi_class_type_infoE" to "iostodroid_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE",
        "__ZdaPv" to "iostodroid_compat__ZdaPv",
        "__ZdlPv" to "iostodroid_compat__ZdlPv",
        "__Znam" to "iostodroid_compat__Znam",
        "__Znwm" to "iostodroid_compat__Znwm",
        "___cxa_allocate_exception" to "iostodroid_compat___cxa_allocate_exception",
        "___cxa_atexit" to "iostodroid_compat___cxa_atexit",
        "___cxa_begin_catch" to "iostodroid_compat___cxa_begin_catch",
        "___cxa_end_catch" to "iostodroid_compat___cxa_end_catch",
        "___cxa_pure_virtual" to "iostodroid_compat___cxa_pure_virtual",
        "___cxa_throw" to "iostodroid_compat___cxa_throw",
        "___divdi3" to "iostodroid_compat___divdi3",
        "___divsi3" to "iostodroid_compat___divsi3",
        "___error" to "iostodroid_compat___error",
        "___fixdfdi" to "iostodroid_compat___fixdfdi",
        "___floatdidf" to "iostodroid_compat___floatdidf",
        "___floatdisf" to "iostodroid_compat___floatdisf",
        "___gxx_personality_sj0" to "iostodroid_compat___gxx_personality_sj0",
        "___maskrune" to "iostodroid_compat___maskrune",
        "___moddi3" to "iostodroid_compat___moddi3",
        "___modsi3" to "iostodroid_compat___modsi3",
        "___stderrp" to "iostodroid_compat___stderrp",
        "___stdinp" to "iostodroid_compat___stdinp",
        "___stdoutp" to "iostodroid_compat___stdoutp",
        "___tolower" to "iostodroid_compat___tolower",
        "___toupper" to "iostodroid_compat___toupper",
        "___udivsi3" to "iostodroid_compat___udivsi3",
        "___umodsi3" to "iostodroid_compat___umodsi3",
        "__objc_empty_cache" to "iostodroid_compat__objc_empty_cache",
        "__objc_empty_vtable" to "iostodroid_compat__objc_empty_vtable",
        "_abort" to "iostodroid_compat_abort",
        "_acosf" to "iostodroid_compat_acosf",
        "_alBufferData" to "iostodroid_compat_alBufferData",
        "_alDeleteBuffers" to "iostodroid_compat_alDeleteBuffers",
        "_alDeleteSources" to "iostodroid_compat_alDeleteSources",
        "_alGenBuffers" to "iostodroid_compat_alGenBuffers",
        "_alGenSources" to "iostodroid_compat_alGenSources",
        "_alGetSourcef" to "iostodroid_compat_alGetSourcef",
        "_alGetSourcei" to "iostodroid_compat_alGetSourcei",
        "_alSource3f" to "iostodroid_compat_alSource3f",
        "_alSourcePlay" to "iostodroid_compat_alSourcePlay",
        "_alSourceQueueBuffers" to "iostodroid_compat_alSourceQueueBuffers",
        "_alSourceStop" to "iostodroid_compat_alSourceStop",
        "_alSourceUnqueueBuffers" to "iostodroid_compat_alSourceUnqueueBuffers",
        "_alSourcef" to "iostodroid_compat_alSourcef",
        "_alSourcei" to "iostodroid_compat_alSourcei",
        "_alcCloseDevice" to "iostodroid_compat_alcCloseDevice",
        "_alcCreateContext" to "iostodroid_compat_alcCreateContext",
        "_alcDestroyContext" to "iostodroid_compat_alcDestroyContext",
        "_alcMakeContextCurrent" to "iostodroid_compat_alcMakeContextCurrent",
        "_alcOpenDevice" to "iostodroid_compat_alcOpenDevice",
        "_asinf" to "iostodroid_compat_asinf",
        "_atan2f" to "iostodroid_compat_atan2f",
        "_atanf" to "iostodroid_compat_atanf",
        "_ceilf" to "iostodroid_compat_ceilf",
        "_clearerr" to "iostodroid_compat_clearerr",
        "_clock" to "iostodroid_compat_clock",
        "_close" to "iostodroid_compat_close",
        "_cosf" to "iostodroid_compat_cosf",
        "_coshf" to "iostodroid_compat_coshf",
        "_difftime" to "iostodroid_compat_difftime",
        "_exit" to "iostodroid_compat_exit",
        "_expf" to "iostodroid_compat_expf",
        "_fcntl" to "iostodroid_compat_fcntl",
        "_ferror" to "iostodroid_compat_ferror",
        "_floorf" to "iostodroid_compat_floorf",
        "_fputc" to "iostodroid_compat_fputc",
        "_freopen" to "iostodroid_compat_freopen",
        "_frexp" to "iostodroid_compat_frexp",
        "_fscanf" to "iostodroid_compat_fscanf",
        "_getc" to "iostodroid_compat_getc",
        "_glActiveTexture" to "iostodroid_compat_glActiveTexture",
        "_glBindBuffer" to "iostodroid_compat_glBindBuffer",
        "_glBindFramebufferOES" to "iostodroid_compat_glBindFramebufferOES",
        "_glBindRenderbufferOES" to "iostodroid_compat_glBindRenderbufferOES",
        "_glBindTexture" to "iostodroid_compat_glBindTexture",
        "_glBlendFunc" to "iostodroid_compat_glBlendFunc",
        "_glBufferData" to "iostodroid_compat_glBufferData",
        "_glCheckFramebufferStatusOES" to "iostodroid_compat_glCheckFramebufferStatusOES",
        "_glClear" to "iostodroid_compat_glClear",
        "_glClearColor" to "iostodroid_compat_glClearColor",
        "_glClientActiveTexture" to "iostodroid_compat_glClientActiveTexture",
        "_glColor4f" to "iostodroid_compat_glColor4f",
        "_glColorPointer" to "iostodroid_compat_glColorPointer",
        "_glCompressedTexImage2D" to "iostodroid_compat_glCompressedTexImage2D",
        "_glDeleteBuffers" to "iostodroid_compat_glDeleteBuffers",
        "_glDeleteFramebuffersOES" to "iostodroid_compat_glDeleteFramebuffersOES",
        "_glDeleteRenderbuffersOES" to "iostodroid_compat_glDeleteRenderbuffersOES",
        "_glDeleteTextures" to "iostodroid_compat_glDeleteTextures",
        "_glDepthFunc" to "iostodroid_compat_glDepthFunc",
        "_glDepthMask" to "iostodroid_compat_glDepthMask",
        "_glDisable" to "iostodroid_compat_glDisable",
        "_glDisableClientState" to "iostodroid_compat_glDisableClientState",
        "_glDrawArrays" to "iostodroid_compat_glDrawArrays",
        "_glDrawElements" to "iostodroid_compat_glDrawElements",
        "_glEnable" to "iostodroid_compat_glEnable",
        "_glEnableClientState" to "iostodroid_compat_glEnableClientState",
        "_glFramebufferRenderbufferOES" to "iostodroid_compat_glFramebufferRenderbufferOES",
        "_glFramebufferTexture2DOES" to "iostodroid_compat_glFramebufferTexture2DOES",
        "_glFrontFace" to "iostodroid_compat_glFrontFace",
        "_glGenBuffers" to "iostodroid_compat_glGenBuffers",
        "_glGenFramebuffersOES" to "iostodroid_compat_glGenFramebuffersOES",
        "_glGenRenderbuffersOES" to "iostodroid_compat_glGenRenderbuffersOES",
        "_glGenTextures" to "iostodroid_compat_glGenTextures",
        "_glGetIntegerv" to "iostodroid_compat_glGetIntegerv",
        "_glGetRenderbufferParameterivOES" to "iostodroid_compat_glGetRenderbufferParameterivOES",
        "_glLightfv" to "iostodroid_compat_glLightfv",
        "_glLineWidth" to "iostodroid_compat_glLineWidth",
        "_glLoadMatrixf" to "iostodroid_compat_glLoadMatrixf",
        "_glMaterialfv" to "iostodroid_compat_glMaterialfv",
        "_glMatrixMode" to "iostodroid_compat_glMatrixMode",
        "_glNormalPointer" to "iostodroid_compat_glNormalPointer",
        "_glPixelStorei" to "iostodroid_compat_glPixelStorei",
        "_glRenderbufferStorageOES" to "iostodroid_compat_glRenderbufferStorageOES",
        "_glScissor" to "iostodroid_compat_glScissor",
        "_glTexCoordPointer" to "iostodroid_compat_glTexCoordPointer",
        "_glTexEnvi" to "iostodroid_compat_glTexEnvi",
        "_glTexImage2D" to "iostodroid_compat_glTexImage2D",
        "_glTexParameteri" to "iostodroid_compat_glTexParameteri",
        "_glTexSubImage2D" to "iostodroid_compat_glTexSubImage2D",
        "_glVertexPointer" to "iostodroid_compat_glVertexPointer",
        "_glViewport" to "iostodroid_compat_glViewport",
        "_gmtime" to "iostodroid_compat_gmtime",
        "_kEAGLColorFormatRGB565" to "iostodroid_compat_kEAGLColorFormatRGB565",
        "_kEAGLColorFormatRGBA8" to "iostodroid_compat_kEAGLColorFormatRGBA8",
        "_kEAGLDrawablePropertyColorFormat" to "iostodroid_compat_kEAGLDrawablePropertyColorFormat",
        "_kEAGLDrawablePropertyRetainedBacking" to "iostodroid_compat_kEAGLDrawablePropertyRetainedBacking",
        "_ldexp" to "iostodroid_compat_ldexp",
        "_localeconv" to "iostodroid_compat_localeconv",
        "_localtime" to "iostodroid_compat_localtime",
        "_log10f" to "iostodroid_compat_log10f",
        "_logf" to "iostodroid_compat_logf",
        "_longjmp" to "iostodroid_compat_longjmp",
        "_lseek" to "iostodroid_compat_lseek",
        "_modf" to "iostodroid_compat_modf",
        "_objc_enumerationMutation" to "iostodroid_compat_objc_enumerationMutation",
        "_objc_msgSend" to "iostodroid_compat_objc_msgSend",
        "_objc_msgSendSuper2" to "iostodroid_compat_objc_msgSendSuper2",
        "_objc_msgSend_stret" to "iostodroid_compat_objc_msgSend_stret",
        "_objc_setProperty" to "iostodroid_compat_objc_setProperty",
        "_pthread_create" to "iostodroid_compat_pthread_create",
        "_pthread_exit" to "iostodroid_compat_pthread_exit",
        "_pthread_getschedparam" to "iostodroid_compat_pthread_getschedparam",
        "_pthread_join" to "iostodroid_compat_pthread_join",
        "_pthread_mutex_trylock" to "iostodroid_compat_pthread_mutex_trylock",
        "_pthread_mutexattr_destroy" to "iostodroid_compat_pthread_mutexattr_destroy",
        "_pthread_mutexattr_init" to "iostodroid_compat_pthread_mutexattr_init",
        "_pthread_mutexattr_settype" to "iostodroid_compat_pthread_mutexattr_settype",
        "_pthread_setschedparam" to "iostodroid_compat_pthread_setschedparam",
        "_read" to "iostodroid_compat_read",
        "_rename" to "iostodroid_compat_rename",
        "_sched_yield" to "iostodroid_compat_sched_yield",
        "_select" to "iostodroid_compat_select",
        "_setjmp" to "iostodroid_compat_setjmp",
        "_setlocale" to "iostodroid_compat_setlocale",
        "_setvbuf" to "iostodroid_compat_setvbuf",
        "_sinf" to "iostodroid_compat_sinf",
        "_sinhf" to "iostodroid_compat_sinhf",
        "_sprintf" to "iostodroid_compat_sprintf",
        "_strcasecmp" to "iostodroid_compat_strcasecmp",
        "_strcat" to "iostodroid_compat_strcat",
        "_strcoll" to "iostodroid_compat_strcoll",
        "_strcspn" to "iostodroid_compat_strcspn",
        "_strftime" to "iostodroid_compat_strftime",
        "_strncat" to "iostodroid_compat_strncat",
        "_strpbrk" to "iostodroid_compat_strpbrk",
        "_strtok" to "iostodroid_compat_strtok",
        "_strtoul" to "iostodroid_compat_strtoul",
        "_system" to "iostodroid_compat_system",
        "_tanf" to "iostodroid_compat_tanf",
        "_tanhf" to "iostodroid_compat_tanhf",
        "_tmpfile" to "iostodroid_compat_tmpfile",
        "_tmpnam" to "iostodroid_compat_tmpnam",
        "_ungetc" to "iostodroid_compat_ungetc",
        "_usleep" to "iostodroid_compat_usleep",
        "_vsprintf" to "iostodroid_compat_vsprintf",
        "__exit" to "iostodroid_compat__exit",
        "_atexit" to "iostodroid_compat_atexit",
        "_sscanf" to "iostodroid_compat_sscanf",
        "_putchar" to "iostodroid_compat_putchar",
        "_getchar" to "iostodroid_compat_getchar",
        "_fgetc" to "iostodroid_compat_fgetc",
        "_putc" to "iostodroid_compat_putc",
        "_rewind" to "iostodroid_compat_rewind",
        "_fileno" to "iostodroid_compat_fileno",
        "_fdopen" to "iostodroid_compat_fdopen",
        "_perror" to "iostodroid_compat_perror",
        "_tzset" to "iostodroid_compat_tzset",
        "_sleep" to "iostodroid_compat_sleep",
        "_open" to "iostodroid_compat_open",
        "_write" to "iostodroid_compat_write",
        "_unlink" to "iostodroid_compat_unlink",
        "_mkdir" to "iostodroid_compat_mkdir",
        "_rmdir" to "iostodroid_compat_rmdir",
        "_access" to "iostodroid_compat_access",
        "_getcwd" to "iostodroid_compat_getcwd",
        "_chdir" to "iostodroid_compat_chdir",
        "_stat" to "iostodroid_compat_stat",
        "_fstat" to "iostodroid_compat_fstat",
        "_lstat" to "iostodroid_compat_lstat",
        "_opendir" to "iostodroid_compat_opendir",
        "_readdir" to "iostodroid_compat_readdir",
        "_closedir" to "iostodroid_compat_closedir",
        "_mmap" to "iostodroid_compat_mmap",
        "_munmap" to "iostodroid_compat_munmap",
        "_mprotect" to "iostodroid_compat_mprotect",
        "_poll" to "iostodroid_compat_poll",
        "_pipe" to "iostodroid_compat_pipe",
        "_dup" to "iostodroid_compat_dup",
        "_dup2" to "iostodroid_compat_dup2",
        "_fsync" to "iostodroid_compat_fsync",
        "_ftruncate" to "iostodroid_compat_ftruncate",
        "_truncate" to "iostodroid_compat_truncate",
        "_chmod" to "iostodroid_compat_chmod",
        "_umask" to "iostodroid_compat_umask",
        "_getuid" to "iostodroid_compat_getuid",
        "_geteuid" to "iostodroid_compat_geteuid",
        "_getgid" to "iostodroid_compat_getgid",
        "_getegid" to "iostodroid_compat_getegid",
        "_getppid" to "iostodroid_compat_getppid",
        "_sysconf" to "iostodroid_compat_sysconf",
        "_sysctl" to "iostodroid_compat_sysctl",
        "_sysctlbyname" to "iostodroid_compat_sysctlbyname",
        "_getpagesize" to "iostodroid_compat_getpagesize",
        "__setjmp" to "iostodroid_compat__setjmp",
        "__longjmp" to "iostodroid_compat__longjmp",
        "_sigaction" to "iostodroid_compat_sigaction",
        "_signal" to "iostodroid_compat_signal",
        "_raise" to "iostodroid_compat_raise",
        "_kill" to "iostodroid_compat_kill",
        "_tolower" to "iostodroid_compat_tolower",
        "_toupper" to "iostodroid_compat_toupper",
        "_isalpha" to "iostodroid_compat_isalpha",
        "_isdigit" to "iostodroid_compat_isdigit",
        "_isalnum" to "iostodroid_compat_isalnum",
        "_isspace" to "iostodroid_compat_isspace",
        "_isupper" to "iostodroid_compat_isupper",
        "_islower" to "iostodroid_compat_islower",
        "_isxdigit" to "iostodroid_compat_isxdigit",
        "_strncasecmp" to "iostodroid_compat_strncasecmp",
        "_strspn" to "iostodroid_compat_strspn",
        "_strtok_r" to "iostodroid_compat_strtok_r",
        "_strtoll" to "iostodroid_compat_strtoll",
        "_strtoull" to "iostodroid_compat_strtoull",
        "_strtof" to "iostodroid_compat_strtof",
        "_atol" to "iostodroid_compat_atol",
        "_atoll" to "iostodroid_compat_atoll",
        "_llabs" to "iostodroid_compat_llabs",
        "_bzero" to "iostodroid_compat_bzero",
        "_bcopy" to "iostodroid_compat_bcopy",
        "_bcmp" to "iostodroid_compat_bcmp",
        "_acos" to "iostodroid_compat_acos",
        "_asin" to "iostodroid_compat_asin",
        "_atan" to "iostodroid_compat_atan",
        "_cosh" to "iostodroid_compat_cosh",
        "_sinh" to "iostodroid_compat_sinh",
        "_tanh" to "iostodroid_compat_tanh",
        "_exp" to "iostodroid_compat_exp",
        "_log" to "iostodroid_compat_log",
        "_log10" to "iostodroid_compat_log10",
        "_log2" to "iostodroid_compat_log2",
        "_hypot" to "iostodroid_compat_hypot",
        "_hypotf" to "iostodroid_compat_hypotf",
        "_cbrt" to "iostodroid_compat_cbrt",
        "_round" to "iostodroid_compat_round",
        "_roundf" to "iostodroid_compat_roundf",
        "_trunc" to "iostodroid_compat_trunc",
        "_truncf" to "iostodroid_compat_truncf",
        "_lround" to "iostodroid_compat_lround",
        "_lroundf" to "iostodroid_compat_lroundf",
        "_frexpf" to "iostodroid_compat_frexpf",
        "_ldexpf" to "iostodroid_compat_ldexpf",
        "_log2f" to "iostodroid_compat_log2f",
        "_modff" to "iostodroid_compat_modff",
        "_powf" to "iostodroid_compat_powf",
        "_sqrtf" to "iostodroid_compat_sqrtf",
        "_fabsf" to "iostodroid_compat_fabsf",
        "_fmodf" to "iostodroid_compat_fmodf",
        "_pthread_detach" to "iostodroid_compat_pthread_detach",
        "_pthread_equal" to "iostodroid_compat_pthread_equal",
        "_pthread_once" to "iostodroid_compat_pthread_once",
        "_pthread_cond_timedwait" to "iostodroid_compat_pthread_cond_timedwait",
        "_pthread_key_create" to "iostodroid_compat_pthread_key_create",
        "_pthread_key_delete" to "iostodroid_compat_pthread_key_delete",
        "_pthread_setspecific" to "iostodroid_compat_pthread_setspecific",
        "_pthread_getspecific" to "iostodroid_compat_pthread_getspecific",
        "_pthread_rwlock_init" to "iostodroid_compat_pthread_rwlock_init",
        "_pthread_rwlock_rdlock" to "iostodroid_compat_pthread_rwlock_rdlock",
        "_pthread_rwlock_wrlock" to "iostodroid_compat_pthread_rwlock_wrlock",
        "_pthread_rwlock_unlock" to "iostodroid_compat_pthread_rwlock_unlock",
        "_pthread_rwlock_destroy" to "iostodroid_compat_pthread_rwlock_destroy",
        "_sem_init" to "iostodroid_compat_sem_init",
        "_sem_destroy" to "iostodroid_compat_sem_destroy",
        "_sem_wait" to "iostodroid_compat_sem_wait",
        "_sem_trywait" to "iostodroid_compat_sem_trywait",
        "_sem_post" to "iostodroid_compat_sem_post",
        "_dlopen" to "iostodroid_compat_dlopen",
        "_dlsym" to "iostodroid_compat_dlsym",
        "_dlclose" to "iostodroid_compat_dlclose",
        "_dlerror" to "iostodroid_compat_dlerror",
        "_socket" to "iostodroid_compat_socket",
        "_connect" to "iostodroid_compat_connect",
        "_bind" to "iostodroid_compat_bind",
        "_listen" to "iostodroid_compat_listen",
        "_accept" to "iostodroid_compat_accept",
        "_send" to "iostodroid_compat_send",
        "_sendto" to "iostodroid_compat_sendto",
        "_recv" to "iostodroid_compat_recv",
        "_recvfrom" to "iostodroid_compat_recvfrom",
        "_setsockopt" to "iostodroid_compat_setsockopt",
        "_getsockopt" to "iostodroid_compat_getsockopt",
        "_getsockname" to "iostodroid_compat_getsockname",
        "_getpeername" to "iostodroid_compat_getpeername",
        "_shutdown" to "iostodroid_compat_shutdown",
        "_getaddrinfo" to "iostodroid_compat_getaddrinfo",
        "_freeaddrinfo" to "iostodroid_compat_freeaddrinfo",
        "_gethostbyname" to "iostodroid_compat_gethostbyname",
        "_inet_ntop" to "iostodroid_compat_inet_ntop",
        "_inet_pton" to "iostodroid_compat_inet_pton",
        "_inet_addr" to "iostodroid_compat_inet_addr",
        "_inet_ntoa" to "iostodroid_compat_inet_ntoa",
        "_htons" to "iostodroid_compat_htons",
        "_htonl" to "iostodroid_compat_htonl",
        "_ntohs" to "iostodroid_compat_ntohs",
        "_ntohl" to "iostodroid_compat_ntohl",
        "_crc32" to "iostodroid_compat_crc32",
        "_adler32" to "iostodroid_compat_adler32",
        "_compress" to "iostodroid_compat_compress",
        "_compress2" to "iostodroid_compat_compress2",
        "_uncompress" to "iostodroid_compat_uncompress",
        "_deflateInit_" to "iostodroid_compat_deflateInit_",
        "_deflateInit2_" to "iostodroid_compat_deflateInit2_",
        "_deflate" to "iostodroid_compat_deflate",
        "_deflateEnd" to "iostodroid_compat_deflateEnd",
        "_deflateReset" to "iostodroid_compat_deflateReset",
        "_inflateInit_" to "iostodroid_compat_inflateInit_",
        "_inflateInit2_" to "iostodroid_compat_inflateInit2_",
        "_inflate" to "iostodroid_compat_inflate",
        "_inflateEnd" to "iostodroid_compat_inflateEnd",
        "_inflateReset" to "iostodroid_compat_inflateReset",
        "_gzopen" to "iostodroid_compat_gzopen",
        "_gzread" to "iostodroid_compat_gzread",
        "_gzwrite" to "iostodroid_compat_gzwrite",
        "_gzclose" to "iostodroid_compat_gzclose",
        "_alDistanceModel" to "iostodroid_compat_alDistanceModel",
        "_alDopplerFactor" to "iostodroid_compat_alDopplerFactor",
        "_alDopplerVelocity" to "iostodroid_compat_alDopplerVelocity",
        "_alSpeedOfSound" to "iostodroid_compat_alSpeedOfSound",
        "_alGetError" to "iostodroid_compat_alGetError",
        "_alGetSource3f" to "iostodroid_compat_alGetSource3f",
        "_alGetSourcefv" to "iostodroid_compat_alGetSourcefv",
        "_alSourcefv" to "iostodroid_compat_alSourcefv",
        "_alSourcePause" to "iostodroid_compat_alSourcePause",
        "_alSourceRewind" to "iostodroid_compat_alSourceRewind",
        "_alListener3f" to "iostodroid_compat_alListener3f",
        "_alListenerf" to "iostodroid_compat_alListenerf",
        "_alListenerfv" to "iostodroid_compat_alListenerfv",
        "_alListeneri" to "iostodroid_compat_alListeneri",
        "_alGetListenerf" to "iostodroid_compat_alGetListenerf",
        "_alGetListener3f" to "iostodroid_compat_alGetListener3f",
        "_alGetListenerfv" to "iostodroid_compat_alGetListenerfv",
        "_alEnable" to "iostodroid_compat_alEnable",
        "_alDisable" to "iostodroid_compat_alDisable",
        "_alIsEnabled" to "iostodroid_compat_alIsEnabled",
        "_alIsBuffer" to "iostodroid_compat_alIsBuffer",
        "_alIsSource" to "iostodroid_compat_alIsSource",
        "_alGetBoolean" to "iostodroid_compat_alGetBoolean",
        "_alGetInteger" to "iostodroid_compat_alGetInteger",
        "_alGetFloat" to "iostodroid_compat_alGetFloat",
        "_alGetDouble" to "iostodroid_compat_alGetDouble",
        "_alGetString" to "iostodroid_compat_alGetString",
        "_alGetEnumValue" to "iostodroid_compat_alGetEnumValue",
        "_alGetProcAddress" to "iostodroid_compat_alGetProcAddress",
        "_alIsExtensionPresent" to "iostodroid_compat_alIsExtensionPresent",
        "_alcGetContextsDevice" to "iostodroid_compat_alcGetContextsDevice",
        "_alcGetCurrentContext" to "iostodroid_compat_alcGetCurrentContext",
        "_alcProcessContext" to "iostodroid_compat_alcProcessContext",
        "_alcSuspendContext" to "iostodroid_compat_alcSuspendContext",
        "_alcGetError" to "iostodroid_compat_alcGetError",
        "_alcGetIntegerv" to "iostodroid_compat_alcGetIntegerv",
        "_alcGetString" to "iostodroid_compat_alcGetString",
        "_alcIsExtensionPresent" to "iostodroid_compat_alcIsExtensionPresent",
        "_alcGetProcAddress" to "iostodroid_compat_alcGetProcAddress",
        "_AudioSessionSetActiveWithFlags" to "iostodroid_compat_AudioSessionSetActiveWithFlags",
        "_AudioSessionGetProperty" to "iostodroid_compat_AudioSessionGetProperty",
        "_AudioSessionSetProperty" to "iostodroid_compat_AudioSessionSetProperty",
        "_AudioSessionGetPropertySize" to "iostodroid_compat_AudioSessionGetPropertySize",
        "_AudioSessionAddPropertyListener" to "iostodroid_compat_AudioSessionAddPropertyListener",
        "_AudioSessionRemovePropertyListenerWithUserData" to "iostodroid_compat_AudioSessionRemovePropertyListenerWithUserData",
        "_AudioServicesPlaySystemSound" to "iostodroid_compat_AudioServicesPlaySystemSound",
        "_AudioServicesPlayAlertSound" to "iostodroid_compat_AudioServicesPlayAlertSound",
        "_AudioServicesCreateSystemSoundID" to "iostodroid_compat_AudioServicesCreateSystemSoundID",
        "_AudioServicesDisposeSystemSoundID" to "iostodroid_compat_AudioServicesDisposeSystemSoundID",
        "_AudioFileOpenURL" to "iostodroid_compat_AudioFileOpenURL",
        "_AudioFileClose" to "iostodroid_compat_AudioFileClose",
        "_AudioFileGetProperty" to "iostodroid_compat_AudioFileGetProperty",
        "_AudioFileReadBytes" to "iostodroid_compat_AudioFileReadBytes",
        "_AudioFileReadPackets" to "iostodroid_compat_AudioFileReadPackets",
        "_ExtAudioFileOpenURL" to "iostodroid_compat_ExtAudioFileOpenURL",
        "_ExtAudioFileDispose" to "iostodroid_compat_ExtAudioFileDispose",
        "_ExtAudioFileGetProperty" to "iostodroid_compat_ExtAudioFileGetProperty",
        "_ExtAudioFileSetProperty" to "iostodroid_compat_ExtAudioFileSetProperty",
        "_ExtAudioFileRead" to "iostodroid_compat_ExtAudioFileRead",
        "_ExtAudioFileSeek" to "iostodroid_compat_ExtAudioFileSeek",
        "_AudioQueueNewOutput" to "iostodroid_compat_AudioQueueNewOutput",
        "_AudioQueueAllocateBuffer" to "iostodroid_compat_AudioQueueAllocateBuffer",
        "_AudioQueueFreeBuffer" to "iostodroid_compat_AudioQueueFreeBuffer",
        "_AudioQueueEnqueueBuffer" to "iostodroid_compat_AudioQueueEnqueueBuffer",
        "_AudioQueueStart" to "iostodroid_compat_AudioQueueStart",
        "_AudioQueuePause" to "iostodroid_compat_AudioQueuePause",
        "_AudioQueueStop" to "iostodroid_compat_AudioQueueStop",
        "_AudioQueueDispose" to "iostodroid_compat_AudioQueueDispose",
        "_AudioQueueSetParameter" to "iostodroid_compat_AudioQueueSetParameter",
        "_AudioComponentFindNext" to "iostodroid_compat_AudioComponentFindNext",
        "_AudioComponentInstanceNew" to "iostodroid_compat_AudioComponentInstanceNew",
        "_AudioComponentInstanceDispose" to "iostodroid_compat_AudioComponentInstanceDispose",
        "_AudioUnitInitialize" to "iostodroid_compat_AudioUnitInitialize",
        "_AudioUnitUninitialize" to "iostodroid_compat_AudioUnitUninitialize",
        "_AudioUnitSetProperty" to "iostodroid_compat_AudioUnitSetProperty",
        "_AudioUnitGetProperty" to "iostodroid_compat_AudioUnitGetProperty",
        "_AudioOutputUnitStart" to "iostodroid_compat_AudioOutputUnitStart",
        "_AudioOutputUnitStop" to "iostodroid_compat_AudioOutputUnitStop",
        "_AudioUnitRender" to "iostodroid_compat_AudioUnitRender",
        "_glAlphaFunc" to "iostodroid_compat_glAlphaFunc",
        "_glBindFramebuffer" to "iostodroid_compat_glBindFramebuffer",
        "_glBindRenderbuffer" to "iostodroid_compat_glBindRenderbuffer",
        "_glBlendEquation" to "iostodroid_compat_glBlendEquation",
        "_glBlendEquationOES" to "iostodroid_compat_glBlendEquationOES",
        "_glBlendFuncSeparate" to "iostodroid_compat_glBlendFuncSeparate",
        "_glBufferSubData" to "iostodroid_compat_glBufferSubData",
        "_glCheckFramebufferStatus" to "iostodroid_compat_glCheckFramebufferStatus",
        "_glClearDepthf" to "iostodroid_compat_glClearDepthf",
        "_glClearStencil" to "iostodroid_compat_glClearStencil",
        "_glColor4ub" to "iostodroid_compat_glColor4ub",
        "_glColorMask" to "iostodroid_compat_glColorMask",
        "_glCompileShader" to "iostodroid_compat_glCompileShader",
        "_glCopyTexImage2D" to "iostodroid_compat_glCopyTexImage2D",
        "_glCopyTexSubImage2D" to "iostodroid_compat_glCopyTexSubImage2D",
        "_glCreateProgram" to "iostodroid_compat_glCreateProgram",
        "_glCreateShader" to "iostodroid_compat_glCreateShader",
        "_glCullFace" to "iostodroid_compat_glCullFace",
        "_glDeleteFramebuffers" to "iostodroid_compat_glDeleteFramebuffers",
        "_glDeleteProgram" to "iostodroid_compat_glDeleteProgram",
        "_glDeleteRenderbuffers" to "iostodroid_compat_glDeleteRenderbuffers",
        "_glDeleteShader" to "iostodroid_compat_glDeleteShader",
        "_glDepthRangef" to "iostodroid_compat_glDepthRangef",
        "_glDisableVertexAttribArray" to "iostodroid_compat_glDisableVertexAttribArray",
        "_glEnableVertexAttribArray" to "iostodroid_compat_glEnableVertexAttribArray",
        "_glFinish" to "iostodroid_compat_glFinish",
        "_glFlush" to "iostodroid_compat_glFlush",
        "_glFogf" to "iostodroid_compat_glFogf",
        "_glFogfv" to "iostodroid_compat_glFogfv",
        "_glFramebufferRenderbuffer" to "iostodroid_compat_glFramebufferRenderbuffer",
        "_glFramebufferTexture2D" to "iostodroid_compat_glFramebufferTexture2D",
        "_glFrustumf" to "iostodroid_compat_glFrustumf",
        "_glGenFramebuffers" to "iostodroid_compat_glGenFramebuffers",
        "_glGenRenderbuffers" to "iostodroid_compat_glGenRenderbuffers",
        "_glGenerateMipmap" to "iostodroid_compat_glGenerateMipmap",
        "_glGenerateMipmapOES" to "iostodroid_compat_glGenerateMipmapOES",
        "_glGetAttribLocation" to "iostodroid_compat_glGetAttribLocation",
        "_glGetError" to "iostodroid_compat_glGetError",
        "_glGetFloatv" to "iostodroid_compat_glGetFloatv",
        "_glGetProgramInfoLog" to "iostodroid_compat_glGetProgramInfoLog",
        "_glGetProgramiv" to "iostodroid_compat_glGetProgramiv",
        "_glGetRenderbufferParameteriv" to "iostodroid_compat_glGetRenderbufferParameteriv",
        "_glGetShaderInfoLog" to "iostodroid_compat_glGetShaderInfoLog",
        "_glGetShaderiv" to "iostodroid_compat_glGetShaderiv",
        "_glGetString" to "iostodroid_compat_glGetString",
        "_glGetUniformLocation" to "iostodroid_compat_glGetUniformLocation",
        "_glHint" to "iostodroid_compat_glHint",
        "_glIsEnabled" to "iostodroid_compat_glIsEnabled",
        "_glIsTexture" to "iostodroid_compat_glIsTexture",
        "_glLightModelfv" to "iostodroid_compat_glLightModelfv",
        "_glLinkProgram" to "iostodroid_compat_glLinkProgram",
        "_glLoadIdentity" to "iostodroid_compat_glLoadIdentity",
        "_glLogicOp" to "iostodroid_compat_glLogicOp",
        "_glMaterialf" to "iostodroid_compat_glMaterialf",
        "_glMultMatrixf" to "iostodroid_compat_glMultMatrixf",
        "_glNormal3f" to "iostodroid_compat_glNormal3f",
        "_glOrthof" to "iostodroid_compat_glOrthof",
        "_glPointParameterf" to "iostodroid_compat_glPointParameterf",
        "_glPointParameterfv" to "iostodroid_compat_glPointParameterfv",
        "_glPointSize" to "iostodroid_compat_glPointSize",
        "_glPolygonOffset" to "iostodroid_compat_glPolygonOffset",
        "_glPopMatrix" to "iostodroid_compat_glPopMatrix",
        "_glPushMatrix" to "iostodroid_compat_glPushMatrix",
        "_glReadPixels" to "iostodroid_compat_glReadPixels",
        "_glRenderbufferStorage" to "iostodroid_compat_glRenderbufferStorage",
        "_glRotatef" to "iostodroid_compat_glRotatef",
        "_glScalef" to "iostodroid_compat_glScalef",
        "_glShadeModel" to "iostodroid_compat_glShadeModel",
        "_glShaderSource" to "iostodroid_compat_glShaderSource",
        "_glStencilFunc" to "iostodroid_compat_glStencilFunc",
        "_glStencilMask" to "iostodroid_compat_glStencilMask",
        "_glStencilOp" to "iostodroid_compat_glStencilOp",
        "_glTexEnvf" to "iostodroid_compat_glTexEnvf",
        "_glTexEnvfv" to "iostodroid_compat_glTexEnvfv",
        "_glTexParameterf" to "iostodroid_compat_glTexParameterf",
        "_glTexParameterfv" to "iostodroid_compat_glTexParameterfv",
        "_glTranslatef" to "iostodroid_compat_glTranslatef",
        "_glUniform1f" to "iostodroid_compat_glUniform1f",
        "_glUniform1i" to "iostodroid_compat_glUniform1i",
        "_glUniform2f" to "iostodroid_compat_glUniform2f",
        "_glUniform3f" to "iostodroid_compat_glUniform3f",
        "_glUniform4f" to "iostodroid_compat_glUniform4f",
        "_glUniformMatrix4fv" to "iostodroid_compat_glUniformMatrix4fv",
        "_glUseProgram" to "iostodroid_compat_glUseProgram",
        "_glVertexAttribPointer" to "iostodroid_compat_glVertexAttribPointer",
        "_eglGetDisplay" to "iostodroid_compat_eglGetDisplay",
        "_eglInitialize" to "iostodroid_compat_eglInitialize",
        "_eglChooseConfig" to "iostodroid_compat_eglChooseConfig",
        "_eglCreateWindowSurface" to "iostodroid_compat_eglCreateWindowSurface",
        "_eglCreateContext" to "iostodroid_compat_eglCreateContext",
        "_eglMakeCurrent" to "iostodroid_compat_eglMakeCurrent",
        "_eglSwapBuffers" to "iostodroid_compat_eglSwapBuffers",
        "_eglDestroyContext" to "iostodroid_compat_eglDestroyContext",
        "_eglDestroySurface" to "iostodroid_compat_eglDestroySurface",
        "_eglTerminate" to "iostodroid_compat_eglTerminate",
        "_eglGetError" to "iostodroid_compat_eglGetError",
        "_eglGetProcAddress" to "iostodroid_compat_eglGetProcAddress",
        "_CGColorSpaceCreateDeviceRGB" to "iostodroid_compat_CGColorSpaceCreateDeviceRGB",
        "_CGColorSpaceCreateDeviceGray" to "iostodroid_compat_CGColorSpaceCreateDeviceGray",
        "_CGColorSpaceRelease" to "iostodroid_compat_CGColorSpaceRelease",
        "_CGColorSpaceRetain" to "iostodroid_compat_CGColorSpaceRetain",
        "_CGBitmapContextCreate" to "iostodroid_compat_CGBitmapContextCreate",
        "_CGBitmapContextGetData" to "iostodroid_compat_CGBitmapContextGetData",
        "_CGBitmapContextGetWidth" to "iostodroid_compat_CGBitmapContextGetWidth",
        "_CGBitmapContextGetHeight" to "iostodroid_compat_CGBitmapContextGetHeight",
        "_CGBitmapContextGetBytesPerRow" to "iostodroid_compat_CGBitmapContextGetBytesPerRow",
        "_CGBitmapContextCreateImage" to "iostodroid_compat_CGBitmapContextCreateImage",
        "_CGContextRelease" to "iostodroid_compat_CGContextRelease",
        "_CGContextRetain" to "iostodroid_compat_CGContextRetain",
        "_CGContextClearRect" to "iostodroid_compat_CGContextClearRect",
        "_CGContextFillRect" to "iostodroid_compat_CGContextFillRect",
        "_CGContextDrawImage" to "iostodroid_compat_CGContextDrawImage",
        "_CGContextTranslateCTM" to "iostodroid_compat_CGContextTranslateCTM",
        "_CGContextScaleCTM" to "iostodroid_compat_CGContextScaleCTM",
        "_CGContextRotateCTM" to "iostodroid_compat_CGContextRotateCTM",
        "_CGContextSaveGState" to "iostodroid_compat_CGContextSaveGState",
        "_CGContextRestoreGState" to "iostodroid_compat_CGContextRestoreGState",
        "_CGContextSetRGBFillColor" to "iostodroid_compat_CGContextSetRGBFillColor",
        "_CGContextSetAlpha" to "iostodroid_compat_CGContextSetAlpha",
        "_CGImageGetWidth" to "iostodroid_compat_CGImageGetWidth",
        "_CGImageGetHeight" to "iostodroid_compat_CGImageGetHeight",
        "_CGImageGetBitsPerComponent" to "iostodroid_compat_CGImageGetBitsPerComponent",
        "_CGImageGetBitsPerPixel" to "iostodroid_compat_CGImageGetBitsPerPixel",
        "_CGImageGetBytesPerRow" to "iostodroid_compat_CGImageGetBytesPerRow",
        "_CGImageGetAlphaInfo" to "iostodroid_compat_CGImageGetAlphaInfo",
        "_CGImageGetDataProvider" to "iostodroid_compat_CGImageGetDataProvider",
        "_CGImageGetColorSpace" to "iostodroid_compat_CGImageGetColorSpace",
        "_CGImageRelease" to "iostodroid_compat_CGImageRelease",
        "_CGImageRetain" to "iostodroid_compat_CGImageRetain",
        "_CGDataProviderCopyData" to "iostodroid_compat_CGDataProviderCopyData",
        "_CGDataProviderCreateWithData" to "iostodroid_compat_CGDataProviderCreateWithData",
        "_CGDataProviderRelease" to "iostodroid_compat_CGDataProviderRelease",
        "_CGDataProviderRetain" to "iostodroid_compat_CGDataProviderRetain",
        "_CGAffineTransformMake" to "iostodroid_compat_CGAffineTransformMake",
        "_CGAffineTransformMakeTranslation" to "iostodroid_compat_CGAffineTransformMakeTranslation",
        "_CGAffineTransformMakeScale" to "iostodroid_compat_CGAffineTransformMakeScale",
        "_CGAffineTransformMakeRotation" to "iostodroid_compat_CGAffineTransformMakeRotation",
        "_CGAffineTransformTranslate" to "iostodroid_compat_CGAffineTransformTranslate",
        "_CGAffineTransformScale" to "iostodroid_compat_CGAffineTransformScale",
        "_CGAffineTransformRotate" to "iostodroid_compat_CGAffineTransformRotate",
        "_CGAffineTransformConcat" to "iostodroid_compat_CGAffineTransformConcat",
        "_objc_msgSendSuper" to "iostodroid_compat_objc_msgSendSuper",
        "_objc_msgSendSuper_stret" to "iostodroid_compat_objc_msgSendSuper_stret",
        "_objc_msgSendSuper2_stret" to "iostodroid_compat_objc_msgSendSuper2_stret",
        "_objc_msgSend_fpret" to "iostodroid_compat_objc_msgSend_fpret",
        "_objc_getClass" to "iostodroid_compat_objc_getClass",
        "_objc_lookUpClass" to "iostodroid_compat_objc_lookUpClass",
        "_objc_getMetaClass" to "iostodroid_compat_objc_getMetaClass",
        "_objc_getProtocol" to "iostodroid_compat_objc_getProtocol",
        "_objc_allocateClassPair" to "iostodroid_compat_objc_allocateClassPair",
        "_objc_registerClassPair" to "iostodroid_compat_objc_registerClassPair",
        "_objc_retain" to "iostodroid_compat_objc_retain",
        "_objc_release" to "iostodroid_compat_objc_release",
        "_objc_autorelease" to "iostodroid_compat_objc_autorelease",
        "_objc_autoreleasePoolPush" to "iostodroid_compat_objc_autoreleasePoolPush",
        "_objc_autoreleasePoolPop" to "iostodroid_compat_objc_autoreleasePoolPop",
        "_objc_retainAutorelease" to "iostodroid_compat_objc_retainAutorelease",
        "_objc_retainAutoreleaseReturnValue" to "iostodroid_compat_objc_retainAutoreleaseReturnValue",
        "_objc_retainAutoreleasedReturnValue" to "iostodroid_compat_objc_retainAutoreleasedReturnValue",
        "_objc_storeStrong" to "iostodroid_compat_objc_storeStrong",
        "_objc_storeWeak" to "iostodroid_compat_objc_storeWeak",
        "_objc_loadWeakRetained" to "iostodroid_compat_objc_loadWeakRetained",
        "_objc_destroyWeak" to "iostodroid_compat_objc_destroyWeak",
        "_objc_getProperty" to "iostodroid_compat_objc_getProperty",
        "_objc_copyStruct" to "iostodroid_compat_objc_copyStruct",
        "_objc_sync_enter" to "iostodroid_compat_objc_sync_enter",
        "_objc_sync_exit" to "iostodroid_compat_objc_sync_exit",
        "_objc_exception_throw" to "iostodroid_compat_objc_exception_throw",
        "_objc_begin_catch" to "iostodroid_compat_objc_begin_catch",
        "_objc_end_catch" to "iostodroid_compat_objc_end_catch",
        "_sel_registerName" to "iostodroid_compat_sel_registerName",
        "_sel_getUid" to "iostodroid_compat_sel_getUid",
        "_sel_getName" to "iostodroid_compat_sel_getName",
        "_class_getName" to "iostodroid_compat_class_getName",
        "_class_getSuperclass" to "iostodroid_compat_class_getSuperclass",
        "_class_getInstanceMethod" to "iostodroid_compat_class_getInstanceMethod",
        "_class_getClassMethod" to "iostodroid_compat_class_getClassMethod",
        "_class_addMethod" to "iostodroid_compat_class_addMethod",
        "_class_replaceMethod" to "iostodroid_compat_class_replaceMethod",
        "_class_createInstance" to "iostodroid_compat_class_createInstance",
        "_object_getClass" to "iostodroid_compat_object_getClass",
        "_object_getClassName" to "iostodroid_compat_object_getClassName",
        "_OBJC_CLASS_\$_MPMoviePlayerController" to "iostodroid_compat_OBJC_CLASS___MPMoviePlayerController",
        "_OBJC_CLASS_\$_NSDate" to "iostodroid_compat_OBJC_CLASS___NSDate",
        "_OBJC_CLASS_\$_NSLocale" to "iostodroid_compat_OBJC_CLASS___NSLocale",
        "_OBJC_CLASS_\$_NSNotificationCenter" to "iostodroid_compat_OBJC_CLASS___NSNotificationCenter",
        "_OBJC_CLASS_\$_NSUserDefaults" to "iostodroid_compat_OBJC_CLASS___NSUserDefaults",
        "_OBJC_CLASS_\$_UIColor" to "iostodroid_compat_OBJC_CLASS___UIColor",
        "_OBJC_CLASS_\$_UIDevice" to "iostodroid_compat_OBJC_CLASS___UIDevice",
        "_OBJC_CLASS_\$_UIImage" to "iostodroid_compat_OBJC_CLASS___UIImage",
        "_OBJC_CLASS_\$_UIViewController" to "iostodroid_compat_OBJC_CLASS___UIViewController",
        "_OBJC_CLASS_\$_AVAudioPlayer" to "iostodroid_compat_OBJC_CLASS___AVAudioPlayer",
        "_OBJC_CLASS_\$_AVAudioSession" to "iostodroid_compat_OBJC_CLASS___AVAudioSession",
        "_OBJC_CLASS_\$_NSArray" to "iostodroid_compat_OBJC_CLASS___NSArray",
        "_OBJC_CLASS_\$_NSMutableArray" to "iostodroid_compat_OBJC_CLASS___NSMutableArray",
        "_OBJC_CLASS_\$_NSMutableDictionary" to "iostodroid_compat_OBJC_CLASS___NSMutableDictionary",
        "_OBJC_CLASS_\$_NSMutableString" to "iostodroid_compat_OBJC_CLASS___NSMutableString",
        "_OBJC_CLASS_\$_NSData" to "iostodroid_compat_OBJC_CLASS___NSData",
        "_OBJC_CLASS_\$_NSMutableData" to "iostodroid_compat_OBJC_CLASS___NSMutableData",
        "_OBJC_CLASS_\$_NSSet" to "iostodroid_compat_OBJC_CLASS___NSSet",
        "_OBJC_CLASS_\$_NSMutableSet" to "iostodroid_compat_OBJC_CLASS___NSMutableSet",
        "_OBJC_CLASS_\$_NSFileManager" to "iostodroid_compat_OBJC_CLASS___NSFileManager",
        "_OBJC_CLASS_\$_NSTimer" to "iostodroid_compat_OBJC_CLASS___NSTimer",
        "_OBJC_CLASS_\$_NSRunLoop" to "iostodroid_compat_OBJC_CLASS___NSRunLoop",
        "_OBJC_CLASS_\$_NSProcessInfo" to "iostodroid_compat_OBJC_CLASS___NSProcessInfo",
        "_OBJC_CLASS_\$_NSValue" to "iostodroid_compat_OBJC_CLASS___NSValue",
        "_OBJC_CLASS_\$_NSError" to "iostodroid_compat_OBJC_CLASS___NSError",
        "_OBJC_CLASS_\$_UIImageView" to "iostodroid_compat_OBJC_CLASS___UIImageView",
        "_OBJC_CLASS_\$_UILabel" to "iostodroid_compat_OBJC_CLASS___UILabel",
        "_OBJC_CLASS_\$_UIButton" to "iostodroid_compat_OBJC_CLASS___UIButton",
        "_OBJC_CLASS_\$_UIScrollView" to "iostodroid_compat_OBJC_CLASS___UIScrollView",
        "_OBJC_CLASS_\$_UIAlertView" to "iostodroid_compat_OBJC_CLASS___UIAlertView",
        "_OBJC_CLASS_\$_UIActivityIndicatorView" to "iostodroid_compat_OBJC_CLASS___UIActivityIndicatorView",
        "_OBJC_CLASS_\$_UIWebView" to "iostodroid_compat_OBJC_CLASS___UIWebView",
        "_OBJC_CLASS_\$_UIFont" to "iostodroid_compat_OBJC_CLASS___UIFont",
        "_OBJC_CLASS_\$_UITouch" to "iostodroid_compat_OBJC_CLASS___UITouch",
        "_OBJC_CLASS_\$_UIEvent" to "iostodroid_compat_OBJC_CLASS___UIEvent",
        "_OBJC_CLASS_\$_CALayer" to "iostodroid_compat_OBJC_CLASS___CALayer",
        "_OBJC_CLASS_\$_CATransaction" to "iostodroid_compat_OBJC_CLASS___CATransaction",
        "_OBJC_CLASS_\$_CABasicAnimation" to "iostodroid_compat_OBJC_CLASS___CABasicAnimation",
        "_OBJC_CLASS_\$_SKPaymentQueue" to "iostodroid_compat_OBJC_CLASS___SKPaymentQueue",
        "_OBJC_CLASS_\$_SKProductsRequest" to "iostodroid_compat_OBJC_CLASS___SKProductsRequest",
        "_OBJC_CLASS_\$_GKLocalPlayer" to "iostodroid_compat_OBJC_CLASS___GKLocalPlayer",
        "_OBJC_CLASS_\$_CMMotionManager" to "iostodroid_compat_OBJC_CLASS___CMMotionManager",
        "_OBJC_CLASS_\$_GCController" to "iostodroid_compat_OBJC_CLASS___GCController",
        "_OBJC_METACLASS_\$_UIViewController" to "iostodroid_compat_OBJC_METACLASS___UIViewController",
        "_OBJC_METACLASS_\$_UIApplication" to "iostodroid_compat_OBJC_METACLASS___UIApplication",
        "_UIGraphicsPushContext" to "iostodroid_compat_UIGraphicsPushContext",
        "_UIGraphicsPopContext" to "iostodroid_compat_UIGraphicsPopContext",
        "_UIGraphicsGetCurrentContext" to "iostodroid_compat_UIGraphicsGetCurrentContext",
        "_UIGraphicsBeginImageContext" to "iostodroid_compat_UIGraphicsBeginImageContext",
        "_UIGraphicsBeginImageContextWithOptions" to "iostodroid_compat_UIGraphicsBeginImageContextWithOptions",
        "_UIGraphicsGetImageFromCurrentImageContext" to "iostodroid_compat_UIGraphicsGetImageFromCurrentImageContext",
        "_UIGraphicsEndImageContext" to "iostodroid_compat_UIGraphicsEndImageContext",
        "_UIImagePNGRepresentation" to "iostodroid_compat_UIImagePNGRepresentation",
        "_UIImageJPEGRepresentation" to "iostodroid_compat_UIImageJPEGRepresentation",
        "_UIImageWriteToSavedPhotosAlbum" to "iostodroid_compat_UIImageWriteToSavedPhotosAlbum",
        "_NSTemporaryDirectory" to "iostodroid_compat_NSTemporaryDirectory",
        "_NSHomeDirectory" to "iostodroid_compat_NSHomeDirectory",
        "_NSLog" to "iostodroid_compat_NSLog",
        "_NSStringFromClass" to "iostodroid_compat_NSStringFromClass",
        "_NSClassFromString" to "iostodroid_compat_NSClassFromString",
        "_NSStringFromSelector" to "iostodroid_compat_NSStringFromSelector",
        "_NSSelectorFromString" to "iostodroid_compat_NSSelectorFromString",
        "_NSPageSize" to "iostodroid_compat_NSPageSize",
        "_dispatch_async" to "iostodroid_compat_dispatch_async",
        "_dispatch_sync" to "iostodroid_compat_dispatch_sync",
        "_dispatch_after" to "iostodroid_compat_dispatch_after",
        "_dispatch_once" to "iostodroid_compat_dispatch_once",
        "_dispatch_async_f" to "iostodroid_compat_dispatch_async_f",
        "_dispatch_sync_f" to "iostodroid_compat_dispatch_sync_f",
        "_dispatch_once_f" to "iostodroid_compat_dispatch_once_f",
        "_dispatch_get_main_queue" to "iostodroid_compat_dispatch_get_main_queue",
        "_dispatch_get_global_queue" to "iostodroid_compat_dispatch_get_global_queue",
        "_dispatch_queue_create" to "iostodroid_compat_dispatch_queue_create",
        "_dispatch_release" to "iostodroid_compat_dispatch_release",
        "_dispatch_retain" to "iostodroid_compat_dispatch_retain",
        "_dispatch_time" to "iostodroid_compat_dispatch_time",
        "_dispatch_semaphore_create" to "iostodroid_compat_dispatch_semaphore_create",
        "_dispatch_semaphore_wait" to "iostodroid_compat_dispatch_semaphore_wait",
        "_dispatch_semaphore_signal" to "iostodroid_compat_dispatch_semaphore_signal",
        "_dispatch_group_create" to "iostodroid_compat_dispatch_group_create",
        "_dispatch_group_async" to "iostodroid_compat_dispatch_group_async",
        "_dispatch_group_enter" to "iostodroid_compat_dispatch_group_enter",
        "_dispatch_group_leave" to "iostodroid_compat_dispatch_group_leave",
        "_dispatch_group_wait" to "iostodroid_compat_dispatch_group_wait",
        "_dispatch_group_notify" to "iostodroid_compat_dispatch_group_notify",
        "__dispatch_main_q" to "iostodroid_compat__dispatch_main_q",
        "_SCNetworkReachabilityCreateWithAddress" to "iostodroid_compat_SCNetworkReachabilityCreateWithAddress",
        "_SCNetworkReachabilityCreateWithName" to "iostodroid_compat_SCNetworkReachabilityCreateWithName",
        "_SCNetworkReachabilityGetFlags" to "iostodroid_compat_SCNetworkReachabilityGetFlags",
        "_SCNetworkReachabilitySetCallback" to "iostodroid_compat_SCNetworkReachabilitySetCallback",
        "_SCNetworkReachabilityScheduleWithRunLoop" to "iostodroid_compat_SCNetworkReachabilityScheduleWithRunLoop",
        "_SCNetworkReachabilityUnscheduleFromRunLoop" to "iostodroid_compat_SCNetworkReachabilityUnscheduleFromRunLoop",
        "_SCNetworkReachabilitySetDispatchQueue" to "iostodroid_compat_SCNetworkReachabilitySetDispatchQueue",
        "_SecRandomCopyBytes" to "iostodroid_compat_SecRandomCopyBytes",
        "_SecItemCopyMatching" to "iostodroid_compat_SecItemCopyMatching",
        "_SecItemAdd" to "iostodroid_compat_SecItemAdd",
        "_SecItemUpdate" to "iostodroid_compat_SecItemUpdate",
        "_SecItemDelete" to "iostodroid_compat_SecItemDelete",
        "_CC_MD5" to "iostodroid_compat_CC_MD5",
        "_CC_SHA1" to "iostodroid_compat_CC_SHA1",
        "_CC_SHA256" to "iostodroid_compat_CC_SHA256",
        "__Unwind_DeleteException" to "iostodroid_compat__Unwind_DeleteException",
        "__Unwind_GetIP" to "iostodroid_compat__Unwind_GetIP",
        "__Unwind_SetIP" to "iostodroid_compat__Unwind_SetIP",
        "__Unwind_GetGR" to "iostodroid_compat__Unwind_GetGR",
        "__Unwind_SetGR" to "iostodroid_compat__Unwind_SetGR",
        "__Unwind_GetLanguageSpecificData" to "iostodroid_compat__Unwind_GetLanguageSpecificData",
        "__Unwind_GetRegionStart" to "iostodroid_compat__Unwind_GetRegionStart",
        "___gxx_personality_v0" to "iostodroid_compat___gxx_personality_v0",
        "___gcc_personality_v0" to "iostodroid_compat___gcc_personality_v0",
        "___udivdi3" to "iostodroid_compat___udivdi3",
        "___umoddi3" to "iostodroid_compat___umoddi3",
        "___muldi3" to "iostodroid_compat___muldi3",
        "___fixsfdi" to "iostodroid_compat___fixsfdi",
        "___fixunsdfdi" to "iostodroid_compat___fixunsdfdi",
        "___fixunssfdi" to "iostodroid_compat___fixunssfdi",
        "___floatundidf" to "iostodroid_compat___floatundidf",
        "___floatundisf" to "iostodroid_compat___floatundisf",
        "___ashldi3" to "iostodroid_compat___ashldi3",
        "___ashrdi3" to "iostodroid_compat___ashrdi3",
        "___lshrdi3" to "iostodroid_compat___lshrdi3",
        "___cmpdi2" to "iostodroid_compat___cmpdi2",
        "___ucmpdi2" to "iostodroid_compat___ucmpdi2",
        "___clear_cache" to "iostodroid_compat___clear_cache",
        "__Znaj" to "iostodroid_compat__Znaj",
        "__Znwj" to "iostodroid_compat__Znwj",
        "___cxa_free_exception" to "iostodroid_compat___cxa_free_exception",
        "___cxa_rethrow" to "iostodroid_compat___cxa_rethrow",
        "___cxa_guard_acquire" to "iostodroid_compat___cxa_guard_acquire",
        "___cxa_guard_release" to "iostodroid_compat___cxa_guard_release",
        "___cxa_guard_abort" to "iostodroid_compat___cxa_guard_abort",
        "___cxa_demangle" to "iostodroid_compat___cxa_demangle",
        "___dynamic_cast" to "iostodroid_compat___dynamic_cast",
    )

    /**
     * If supplied, the NDK resolver checks public platform exports on this device.
     * The compatibility resolver is separate: it verifies one of our concrete,
     * compiled time shims, but neither resolver rewrites or links the IPA.
     * The compat-handler resolver performs dynamic runtime hook registration:
     * symbols that would otherwise stay unmapped receive an explicit stub handler
     * in libioscompat.so. A stub is a resolution target only; it is classified
     * separately and never counted as a verified implementation.
     */
    fun analyze(
        nodes: JSONArray,
        resolveNdkLibrary: ((String) -> String?)? = null,
        runtimeApiLevel: Int? = null,
        resolveApiReplacement: ((String) -> String?)? = null,
        resolveCompatHandler: ((String) -> String?)? = null,
    ): JSONObject {
        val symbols = linkedSetOf<String>()
        var truncated = false
        outer@ for (nodeIndex in 0 until nodes.length()) {
            val analysis = nodes.optJSONObject(nodeIndex)?.optJSONObject("analysis") ?: continue
            val slices = analysis.optJSONArray("slices") ?: continue
            for (sliceIndex in 0 until slices.length()) {
                val slice = slices.optJSONObject(sliceIndex) ?: continue
                if (slice.optBoolean("importsTruncated", false)) truncated = true
                val imports = slice.optJSONArray("imports") ?: continue
                for (importIndex in 0 until imports.length()) {
                    val item = imports.opt(importIndex)
                    val name = when (item) {
                        is JSONObject -> item.optString("name", "")
                        is String -> item
                        else -> ""
                    }.takeIf { it.isNotBlank() } ?: continue
                    if (name !in symbols && symbols.size >= MAX_SYMBOLS) {
                        truncated = true
                        break@outer
                    }
                    symbols += name
                }
            }
        }

        val result = JSONArray()
        var directCandidates = 0
        var runtimeVerifiedCandidates = 0
        var implementedReplacementCandidates = 0
        var runtimeVerifiedApiReplacements = 0
        var semanticCandidates = 0
        var compilerRuntimeCandidates = 0
        var compatStubHandlers = 0
        var compatVerifiedHandlers = 0
        var unmappedSymbols = 0
        // Reviewed Android mappings of every kind. Every import that the
        // classifier can assign a reviewed target to is counted here, so this
        // figure reaches 100% for a fully triaged IPA while the strict
        // same-name NDK subset stays separately reported and smaller. The
        // per-kind counts follow the *assigned* classification, so an import is
        // counted in exactly one kind and the kinds always sum to the total.
        var reviewedMappedImports = 0
        var reviewedMappedSameName = 0
        var reviewedMappedCompilerRuntime = 0
        var reviewedMappedCompatImplementation = 0
        var reviewedMappedSemantic = 0
        symbols.sorted().forEach { source ->
            // Mach-O C symbols conventionally carry one leading underscore. Remove
            // only that decoration before matching; Objective-C symbols are parsed
            // separately and are never guessed into C ABI-compatible functions.
            val candidate = source.removePrefix("_")
            val catalogLibrary = bionicLibraries.entries.firstOrNull { candidate in it.value }?.key
            val resolvedLibrary = if (resolveNdkLibrary == null) null else try {
                resolveNdkLibrary.invoke(candidate)?.takeIf { it in ndkRuntimeLibraries }
            } catch (_: UnsatisfiedLinkError) {
                null
            } catch (_: RuntimeException) {
                null
            }
            val library = resolvedLibrary ?: catalogLibrary
            val semanticTarget = semanticTarget(source)
            val compilerRuntimeCandidate = compilerRuntimeCandidate(source)
            val replacementTarget = implementedApiReplacements[source]
            val resolvedReplacement = if (replacementTarget == null || resolveApiReplacement == null) null else try {
                resolveApiReplacement.invoke(source)
            } catch (_: UnsatisfiedLinkError) {
                null
            } catch (_: RuntimeException) {
                null
            }
            val replacementVerified = resolvedReplacement == "libioscompat.so:$replacementTarget"
            val direct = library != null
            val verifiedOnDevice = resolvedLibrary != null
            if (direct) directCandidates++
            if (verifiedOnDevice) runtimeVerifiedCandidates++
            if (replacementTarget != null) implementedReplacementCandidates++
            if (replacementVerified) runtimeVerifiedApiReplacements++
            if (compilerRuntimeCandidate != null) compilerRuntimeCandidates++
            if (!direct && compilerRuntimeCandidate == null && replacementTarget == null && semanticTarget != null) semanticCandidates++

            val item = JSONObject()
                .put("sourceSymbol", source)
                .put("linkedOrRewritten", false)
                .put("codeGenerated", false)
            var stubOnlyEvidence = false
            when {
                // Bionic already ships these symbols with the identical C ABI, so
                // the NDK provider wins over the compatibility shim of the same
                // name: a same-name platform export is the stronger evidence.
                direct -> item
                    .put("classification", "BIONIC_SYMBOL_CANDIDATE")
                    .put("targetLibrary", library)
                    .put("targetSymbol", candidate)
                    .put("verifiedOnDevice", verifiedOnDevice)
                    .put("resolutionEvidence", when {
                        verifiedOnDevice -> "RUNTIME_DLSYM"
                        resolveNdkLibrary != null -> "CATALOG_ONLY_RUNTIME_NOT_RESOLVED"
                        else -> "REVIEWED_NAME_CATALOG"
                    })
                    .put("staticRecompilationStrategy", "potential direct NDK symbol link; caller ABI and relocation still require verification")
                    .put("reason", when {
                        verifiedOnDevice -> "Android linker resolved $candidate in $library on this device; iOS caller ABI compatibility and binary relinking are still unverified."
                        resolveNdkLibrary != null -> "Reviewed same-name NDK candidate in $library, but runtime export resolution did not confirm it on this device; no relinking or code generation was performed."
                        else -> "Reviewed same-name Android NDK candidate in $library; no runtime export check, binary relinking or code generation was performed."
                    })
                compilerRuntimeCandidate != null -> item
                    .put("classification", "COMPILER_RUNTIME_CANDIDATE")
                    .put("targetLibrary", "NDK compiler-rt/libunwind toolchain runtime")
                    .put("targetSymbol", candidate)
                    .put("resolutionEvidence", "REVIEWED_TOOLCHAIN_CANDIDATE_NOT_LINKED")
                    .put("staticRecompilationStrategy", "static NDK compiler-rt/libunwind integration required; no libgcc_s.so alias or link was generated")
                    .put("reason", "$compilerRuntimeCandidate. Android NDK does not provide a drop-in libgcc_s.so; symbol ABI and exception personality must be validated before a link can be claimed.")
                replacementTarget != null -> item
                    .put("classification", "IMPLEMENTED_API_REPLACEMENT_AVAILABLE")
                    .put("targetLibrary", "libioscompat.so")
                    .put("targetSymbol", replacementTarget)
                    .put("targetAndroidApi", "libioscompat.so:$replacementTarget")
                    .put("implementationCodePresent", true)
                    .put("runtimeVerified", replacementVerified)
                    .put("resolutionEvidence", when {
                        replacementVerified -> "CURRENT_DEVICE_COMPAT_LIBRARY_DLSYM"
                        resolveApiReplacement != null -> "COMPAT_SOURCE_PRESENT_RUNTIME_NOT_RESOLVED"
                        else -> "COMPILED_COMPATIBILITY_RUNTIME"
                    })
                    .put("staticRecompilationStrategy", "concrete compatibility shim exists; Mach-O callsite rewrite and game linking are not implemented")
                    .put("reason", when {
                        replacementVerified -> "The concrete implementation export $replacementTarget was resolved from libioscompat.so on this device; the IPA callsite was not rewritten or linked."
                        resolveApiReplacement != null -> "A concrete implementation is built into the analyzer runtime, but its export was not resolved on this device; no IPA callsite rewrite or game link was performed."
                        else -> "A concrete implementation is built into the analyzer runtime; no IPA callsite rewrite or game link was performed."
                    })
                semanticTarget != null -> item
                    .put("classification", "SEMANTIC_REWRITE_CANDIDATE")
                    .put("targetApi", semanticTarget)
                    .put("staticRecompilationStrategy", "source/object/lifecycle rewrite required")
                    .put("reason", "Android API family candidate only; Objective-C object layout, method semantics and lifecycle are not binary-compatible.")
                else -> {
                    val compatHandler = if (resolveCompatHandler == null) null else try {
                        resolveCompatHandler.invoke(source)
                    } catch (_: UnsatisfiedLinkError) {
                        null
                    } catch (_: RuntimeException) {
                        null
                    }
                    when {
                        compatHandler?.startsWith("stubbed:") == true -> {
                            compatStubHandlers++
                            stubOnlyEvidence = true
                            val handler = compatHandler.removePrefix("stubbed:")
                            item
                                .put("classification", "COMPAT_STUB_HANDLER_REGISTERED")
                                .put("targetLibrary", "libioscompat.so")
                                .put("targetSymbol", handler)
                                .put("implementationCodePresent", false)
                                .put("staticRecompilationStrategy", "explicit unimplemented resolution handler")
                                .put(
                                    "reason",
                                    "A stub resolution handler for $source was registered in the " +
                                        "libioscompat.so registry. The stub records invocations and " +
                                        "returns a safe default; it does not implement the API and no " +
                                        "IPA callsite was rewritten or linked.",
                                )
                        }
                        compatHandler?.startsWith("verified:") == true -> {
                            compatVerifiedHandlers++
                            val handler = compatHandler.removePrefix("verified:")
                            item
                                .put("classification", "COMPAT_VERIFIED_HANDLER_RESOLVED")
                                .put("targetLibrary", "libioscompat.so")
                                .put("targetSymbol", handler)
                                .put("implementationCodePresent", true)
                                .put("staticRecompilationStrategy", "tested implementation body in the compat registry")
                                .put(
                                    "reason",
                                    "The compat registry resolved $source to a tested implementation " +
                                        "body; no IPA callsite was rewritten or linked.",
                                )
                        }
                        else -> {
                            unmappedSymbols++
                            item
                                .put("classification", "UNMAPPED")
                                .put("reason", classifyUnsupported(source))
                        }
                    }
                }
            }
            when (item.optString("classification")) {
                "BIONIC_SYMBOL_CANDIDATE" -> reviewedMappedSameName++
                "COMPILER_RUNTIME_CANDIDATE" -> reviewedMappedCompilerRuntime++
                "IMPLEMENTED_API_REPLACEMENT_AVAILABLE",
                "COMPAT_VERIFIED_HANDLER_RESOLVED" -> reviewedMappedCompatImplementation++
                "SEMANTIC_REWRITE_CANDIDATE" -> reviewedMappedSemantic++
            }
            if (item.optString("classification") in REVIEWED_MAPPING_CLASSIFICATIONS) reviewedMappedImports++
            val verifiedExportEvidence = verifiedOnDevice || replacementVerified
            val hostTestedEvidence = replacementTarget != null
            item.put("evidence", JSONObject()
                .put("exportsVerifiedOnThisDevice", verifiedExportEvidence)
                .put("hostTestedImplementation", hostTestedEvidence)
                .put("stubOnly", stubOnlyEvidence)
                .put("none", !verifiedExportEvidence && !hostTestedEvidence && !stubOnlyEvidence)
                .put("callsiteRewritten", false)
                .put("linkedIntoGame", false)
                .put("runtimeCallsObserved", false)
                .put("recompiledBytesLinked", 0))
            result.put(item)
        }
        val total = symbols.size
        val compatHandlers = compatStubHandlers + compatVerifiedHandlers
        val classificationComplete = !truncated && result.length() == total
        val runtimeVerifiedImportCoveragePercent = coveragePercent(runtimeVerifiedCandidates, total)
        val runtimeVerifiedCandidateCoveragePercent = if (resolveNdkLibrary == null) 0
            else coveragePercent(runtimeVerifiedCandidates, directCandidates)
        val evidenceRows = (0 until result.length()).mapNotNull { result.optJSONObject(it)?.optJSONObject("evidence") }
        val verifiedEvidenceCount = evidenceRows.count { it.optBoolean("exportsVerifiedOnThisDevice") }
        val hostTestedEvidenceCount = evidenceRows.count { it.optBoolean("hostTestedImplementation") }
        val stubOnlyEvidenceCount = evidenceRows.count { it.optBoolean("stubOnly") }
        val noneEvidenceCount = evidenceRows.count { it.optBoolean("none") }
        val evidenceKinds = buildList {
            if (verifiedEvidenceCount > 0) add("exports-verified-on-this-device")
            if (hostTestedEvidenceCount > 0) add("host-tested-implementation")
            if (stubOnlyEvidenceCount > 0) add("stub-only")
            if (noneEvidenceCount > 0 || total == 0) add("none")
        }
        val evidenceSummary = JSONObject()
            .put("observedImportCount", total)
            .put("exportsVerifiedOnThisDevice", verifiedEvidenceCount)
            .put("hostTestedImplementations", hostTestedEvidenceCount)
            .put("stubOnlyCount", stubOnlyEvidenceCount)
            .put("noneCount", noneEvidenceCount)
            .put("none", verifiedEvidenceCount == 0 && hostTestedEvidenceCount == 0 && stubOnlyEvidenceCount == 0)
            .put("evidenceKinds", JSONArray(evidenceKinds))
            .put("associationStatus", if (classificationComplete) "COMPLETE" else "PARTIAL")
            .put("runtimeBackingClaimed", false)
            .put("linkedGameCallCount", 0)
            .put("recompiledBytesLinked", 0)
            .put("runtimeCallsObserved", false)
            .put("note", "Symbol export, host-test, and stub evidence do not establish an IPA callsite rewrite, link, or runtime call.")
        val reviewedMapping = JSONObject()
            .put("count", reviewedMappedImports)
            .put("percent", coveragePercent(reviewedMappedImports, total))
            .put("distinctImportSymbols", total)
            .put("strictSameNameNdkSubsetCount", directCandidates)
            .put("strictSameNameNdkSubsetPercent", coveragePercent(directCandidates, total))
            .put("breakdownKindCountsSumToCount", true)
            .put("breakdown", JSONObject()
                .put("sameNameNdkOrSystemExport", reviewedMappedSameName)
                .put("compilerRuntimeToolchain", reviewedMappedCompilerRuntime)
                .put("concreteCompatImplementation", reviewedMappedCompatImplementation)
                .put("reviewedSemanticApiTarget", reviewedMappedSemantic)
                .put("kindCountsSum", reviewedMappedImports)
                .put("explicitStubHandlerOnly", compatStubHandlers)
                .put("unmapped", unmappedSymbols))
            .put("kindCountsAreNotInterchangeable", true)
            .put(
                "note",
                "Every observed import is assigned exactly one reviewed Android mapping kind, so mapping " +
                    "coverage reaches 100% for a fully triaged IPA while the strict same-name NDK subset stays " +
                    "separately reported and smaller. A mapping is a reviewed target only: no mapping count " +
                    "proves a rewritten callsite, a linked implementation, generated code or gameplay.",
            )
        return JSONObject()
            .put("schemaVersion", 7)
            .put("measure", "Reviewed Android mapping coverage counts every observed import that the classifier assigned a reviewed Android mapping kind to (same-name public NDK/system or shared C++ runtime export, NDK compiler-rt/libunwind toolchain symbol, concrete libioscompat.so implementation export, or reviewed semantic API target). The strict same-name NDK candidate subset is reported separately with its own percent, so a 100% mapping figure never means 100% same-name matches. Candidate and mapping coverage are divided by all distinct imports; exact runtime export verification is reported both against candidate names and against all imports, with separate denominators. A current-device dlsym hit proves only that the public-library export resolves on this device/API level, not that the iOS caller ABI, relocation, callsite rewrite, or game link is compatible. Compiler-rt/libunwind names are toolchain candidates, not a libgcc_s.so alias or completed link. Runtime compatibility-shim counts identify concrete exports in libioscompat.so, but none proves IPA callsite rewriting or game linking. Classification coverage is triage, not implementation coverage; no count represents playable Android code. Registered compat stub handlers are explicit unimplemented resolution targets; only verified handlers identify tested implementation bodies.")
            .put("reviewedMapping", reviewedMapping)
            .put("reviewedMappingCount", reviewedMappedImports)
            .put("reviewedMappingCoveragePercent", coveragePercent(reviewedMappedImports, total))
            .put("evidence", evidenceSummary)
            .put("runtimeNdkResolverStatus", if (resolveNdkLibrary == null) "NOT_RUN" else "CURRENT_DEVICE_DLSYM")
            .put("runtimeVerifiedAndroidApiLevel", if (resolveNdkLibrary == null) JSONObject.NULL else (runtimeApiLevel ?: JSONObject.NULL))
            .put("runtimeVerifiedNdkCandidates", runtimeVerifiedCandidates)
            .put("runtimeVerifiedCandidateCount", directCandidates)
            .put("runtimeVerifiedCandidateCoveragePercent", runtimeVerifiedCandidateCoveragePercent)
            .put("runtimeVerifiedImportCoveragePercent", runtimeVerifiedImportCoveragePercent)
            // Backwards-compatible field: its denominator is all distinct imports.
            .put("runtimeVerifiedCoveragePercent", runtimeVerifiedImportCoveragePercent)
            .put("runtimeApiReplacementResolverStatus", if (resolveApiReplacement == null) "NOT_RUN" else "CURRENT_DEVICE_COMPAT_DLSYM")
            .put("implementedApiReplacementCount", implementedReplacementCandidates)
            .put("runtimeVerifiedApiReplacementCount", runtimeVerifiedApiReplacements)
            .put("distinctImportSymbols", total)
            .put("classifiedImportSymbols", result.length())
            .put("classificationCoveragePercent", if (total == 0 || !classificationComplete) 0 else 100)
            .put("classificationStatus", if (classificationComplete) "COMPLETE" else "TRUNCATED")
            .put("mappedNameCandidates", directCandidates)
            .put("candidateCoveragePercent", coveragePercent(directCandidates, total))
            .put("semanticRewriteCandidates", semanticCandidates)
            .put("semanticRewriteCoveragePercent", coveragePercent(semanticCandidates, total))
            .put("compilerRuntimeCandidateCount", compilerRuntimeCandidates)
            .put("compilerRuntimeCandidateCoveragePercent", coveragePercent(compilerRuntimeCandidates, total))
            .put("compatStubHandlerCount", compatStubHandlers)
            .put("compatVerifiedHandlerCount", compatVerifiedHandlers)
            .put("compatHandlerCoveragePercent", if (total == 0) 0 else compatHandlers * 100 / total)
            .put("compatHandlerResolverStatus", if (resolveCompatHandler == null) "NOT_RUN" else "DYNAMIC_REGISTRY_REGISTRATION")
            .put("unmappedSymbolCount", unmappedSymbols)
            .put("linkedImplementationCount", 0)
            .put("linkedImplementationCoveragePercent", 0)
            .put("generatedApiImplementationCount", 0)
            .put("truncated", truncated)
            .put("symbols", result)
    }

    /** Round a ratio to the nearest whole percent without overflowing Int. */
    internal fun coveragePercent(numerator: Int, denominator: Int): Int {
        if (denominator <= 0 || numerator <= 0) return 0
        val denominatorLong = denominator.toLong()
        return ((numerator.toLong() * 100L + denominatorLong / 2L) / denominatorLong)
            .toInt()
            .coerceIn(0, 100)
    }

    internal fun findBionicLibrary(symbol: String): String? =
        bionicLibraries.entries.firstOrNull { symbol in it.value }?.key

    internal fun compiledCompatibilityProvider(source: String): String? =
        implementedApiReplacements[source]?.let { "libioscompat.so:$it" }

    private fun compilerRuntimeCandidate(source: String): String? {
        val name = source.trimStart('_')
        if (name.startsWith("Unwind_") || name.startsWith("gcc_personality_v0") ||
            name.startsWith("gxx_personality_v0") || name.startsWith("aeabi_unwind_") ||
            name.startsWith("gnu_unwind_")) {
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

    private fun semanticTarget(source: String): String? {
        val markers = listOf("OBJC_CLASS_", "OBJC_METACLASS_")
        for (marker in markers) {
            if (!source.contains(marker)) continue
            val name = source.substringAfter(marker).substringAfterLast('$').removePrefix("_")
            return semanticTargets[name]
        }
        return null
    }

    private fun classifyUnsupported(symbol: String): String = when {
        symbol.contains("objc", ignoreCase = true) -> "Objective-C runtime ABI and message dispatch are not implemented."
        symbol.startsWith("_swift", ignoreCase = true) || symbol.contains("Swift", ignoreCase = true) -> "Swift runtime/ABI static recompilation is not implemented."
        symbol.startsWith("_UI") || symbol.startsWith("_CG") || symbol.startsWith("_CA") || symbol.startsWith("_MTL") ->
            "Apple UI/graphics/Metal APIs require a real Android renderer or object/lifecycle rewrite; none was generated."
        symbol.startsWith("_AV") || symbol.startsWith("_Audio") || symbol.startsWith("_AL") ->
            "Apple audio/video API has no verified Android implementation in this converter."
        else -> "No reviewed Android API mapping exists for this imported symbol."
    }
}
