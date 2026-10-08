#pragma once

/*
 * Broad, host-tested iOS/Darwin compatibility shims (part 2 of libioscompat).
 *
 * This header deliberately avoids Apple headers so the same implementation can
 * be built with the Android NDK and exercised by host unit tests. Every symbol
 * declared here is a REAL implementation with a tested body (see
 * native/tests/iostodroid_ios_shims.cpp); nothing in this file is a stub.
 *
 * Scope, stated exactly:
 *
 *  - Darwin libc / POSIX / pthread / math entry points that bionic provides
 *    with an identical C ABI. These are thin, tested forwards. Struct layouts
 *    that genuinely differ between Darwin and Linux/bionic are declared here
 *    in their Darwin shape (see iostodroid_darwin_timeval) instead of silently
 *    reusing the host layout.
 *  - A small, self-contained CoreFoundation object model (allocator, string,
 *    data, mutable array, mutable dictionary, number, date) with real
 *    retain/release, storage and accessors. It is an independent
 *    implementation of the documented behaviour of those types, NOT a
 *    reimplementation of CoreFoundation.
 *
 * Everything here is a resolution target only. Registering a symbol in the
 * compatibility registry does not rewrite an IPA callsite and does not make an
 * iOS app run on Android.
 *
 * Struct-layout caveats (documented, not hidden):
 *  - iostodroid_darwin_timeval matches Darwin's arm64 `struct timeval`
 *    (8-byte tv_sec, 4-byte tv_usec, 4 bytes padding), not Linux's.
 *  - `struct tm` is layout-compatible between Darwin and bionic, so
 *    localtime_r/gmtime_r/mktime forward directly.
 *  - pthread_mutex_t / pthread_cond_t are larger on Darwin than on bionic, so
 *    callers always over-allocate; every operation here touches only
 *    the leading bytes bionic owns.
 *  - pthread_t is an integer on bionic and a pointer on Darwin. Both are
 *    one 64-bit register wide, so values round-trip through calls unchanged;
 *    they are not interchangeable as opaque identifiers across platforms.
 *  - FILE* is always produced by the shims in this library, so it stays
 *    self-consistent regardless of the platform's stdio internals.
 */

#include <stdarg.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Darwin-shaped scalar and object types -------------------------------- */

typedef long iostodroid_CFIndex;
typedef unsigned long iostodroid_CFOptionFlags;
typedef unsigned long iostodroid_CFHashCode;
typedef uint32_t iostodroid_CFStringEncoding;
typedef double iostodroid_CFAbsoluteTime;
typedef double iostodroid_CFTimeInterval;
typedef unsigned char iostodroid_Boolean;
typedef int32_t iostodroid_CFComparisonResult;
typedef uint32_t iostodroid_CFNumberType;

/* Matches Darwin's arm64 struct timeval, which is NOT Linux's layout. */
typedef struct iostodroid_darwin_timeval {
    int64_t tv_sec;
    int32_t tv_usec;
    int32_t tv_pad;
} iostodroid_darwin_timeval;

/*
 * One opaque runtime type backs every CoreFoundation instance. The concrete
 * kind is recorded inside the object, exactly like CF's own _CFRuntimeBase.
 */
struct iostodroid_CFRuntime;
typedef const struct iostodroid_CFRuntime *iostodroid_CFTypeRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFStringRef;
typedef struct iostodroid_CFRuntime *iostodroid_CFMutableStringRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFAllocatorRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFDataRef;
typedef struct iostodroid_CFRuntime *iostodroid_CFMutableDataRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFArrayRef;
typedef struct iostodroid_CFRuntime *iostodroid_CFMutableArrayRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFDictionaryRef;
typedef struct iostodroid_CFRuntime *iostodroid_CFMutableDictionaryRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFNumberRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFDateRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFTimeZoneRef;
typedef const struct iostodroid_CFRuntime *iostodroid_CFRunLoopRef;
typedef int32_t iostodroid_CFRunLoopRunResult;
typedef void (*iostodroid_CFRunLoopPerformCallback)(void *context);

/* CFRunLoopRunInMode result values from CoreFoundation/CFRunLoop.h. */
#define IOSTODROID_KCFRUNLOOPRUNFINISHED ((uint32_t)1)
#define IOSTODROID_KCFRUNLOOPRUNSTOPPED ((uint32_t)2)
#define IOSTODROID_KCFRUNLOOPRUNTIMEDOUT ((uint32_t)3)
#define IOSTODROID_KCFRUNLOOPRUNHANDLEDSOURCE ((uint32_t)4)

/*
 * Collection callbacks are accepted and ignored: the collections implemented
 * here always apply kCFTypeArrayCallBacks / kCFTypeDictionaryCallBacks
 * semantics (retain on insert, release on replace and destroy).
 */
typedef const void *iostodroid_CFArrayCallBacksRef;
typedef const void *iostodroid_CFDictionaryKeyCallBacksRef;
typedef const void *iostodroid_CFDictionaryValueCallBacksRef;

typedef struct iostodroid_CFGregorianDate {
    int32_t year;
    int8_t month;
    int8_t day;
    int8_t hour;
    int8_t minute;
    double second;
} iostodroid_CFGregorianDate;

#define IOSTODROID_KCFSTRINGENCODINGUTF8 ((uint32_t)0x08000100)
#define IOSTODROID_KCFCOMPAREEQUALTO ((int32_t)0)
#define IOSTODROID_KCFCOMPARELESSTHAN ((int32_t)-1)
#define IOSTODROID_KCFCOMPAREGREATERTHAN ((int32_t)1)

/* kCFNumberType values (CoreFoundation CFNumber.h). */
#define IOSTODROID_KCFNUMBERSINT32TYPE ((uint32_t)3)
#define IOSTODROID_KCFNUMBERSINT64TYPE ((uint32_t)4)
#define IOSTODROID_KCFNUMBERFLOAT32TYPE ((uint32_t)5)
#define IOSTODROID_KCFNUMBERFLOAT64TYPE ((uint32_t)6)
#define IOSTODROID_KCFNUMBERINTTYPE ((uint32_t)9)
#define IOSTODROID_KCFNUMBERLONGTYPE ((uint32_t)10)
#define IOSTODROID_KCFNUMBERDOUBLETYPE ((uint32_t)13)

/* --- CoreFoundation object model ------------------------------------------ */

iostodroid_CFAllocatorRef iostodroid_compat_CFAllocatorGetDefault(void);
iostodroid_CFTypeRef iostodroid_compat_CFRetain(iostodroid_CFTypeRef object);
void iostodroid_compat_CFRelease(iostodroid_CFTypeRef object);
iostodroid_CFIndex iostodroid_compat_CFGetRetainCount(iostodroid_CFTypeRef object);

iostodroid_CFStringRef iostodroid_compat_CFStringCreateWithCString(iostodroid_CFAllocatorRef allocator,
                                                         const char *cString,
                                                         iostodroid_CFStringEncoding encoding);
iostodroid_CFIndex iostodroid_compat_CFStringGetLength(iostodroid_CFStringRef string);
iostodroid_Boolean iostodroid_compat_CFStringGetCString(iostodroid_CFStringRef string, char *buffer,
                                              iostodroid_CFIndex bufferSize, iostodroid_CFStringEncoding encoding);
const char *iostodroid_compat_CFStringGetCStringPtr(iostodroid_CFStringRef string, iostodroid_CFStringEncoding encoding);
iostodroid_CFIndex iostodroid_compat_CFStringGetMaximumSizeForEncoding(iostodroid_CFIndex length,
                                                             iostodroid_CFStringEncoding encoding);
iostodroid_CFComparisonResult iostodroid_compat_CFStringCompare(iostodroid_CFStringRef left, iostodroid_CFStringRef right,
                                                      iostodroid_CFOptionFlags options);
iostodroid_CFStringEncoding iostodroid_compat_CFStringGetSystemEncoding(void);

iostodroid_CFDataRef iostodroid_compat_CFDataCreate(iostodroid_CFAllocatorRef allocator, const uint8_t *bytes,
                                          iostodroid_CFIndex length);
const uint8_t *iostodroid_compat_CFDataGetBytePtr(iostodroid_CFDataRef data);
iostodroid_CFIndex iostodroid_compat_CFDataGetLength(iostodroid_CFDataRef data);

iostodroid_CFMutableArrayRef iostodroid_compat_CFArrayCreateMutable(iostodroid_CFAllocatorRef allocator,
                                                          iostodroid_CFIndex capacity,
                                                          iostodroid_CFArrayCallBacksRef callBacks);
void iostodroid_compat_CFArrayAppendValue(iostodroid_CFMutableArrayRef array, const void *value);
iostodroid_CFIndex iostodroid_compat_CFArrayGetCount(iostodroid_CFArrayRef array);
const void *iostodroid_compat_CFArrayGetValueAtIndex(iostodroid_CFArrayRef array, iostodroid_CFIndex index);

iostodroid_CFMutableDictionaryRef iostodroid_compat_CFDictionaryCreateMutable(
    iostodroid_CFAllocatorRef allocator, iostodroid_CFIndex capacity,
    iostodroid_CFDictionaryKeyCallBacksRef keyCallBacks,
    iostodroid_CFDictionaryValueCallBacksRef valueCallBacks);
void iostodroid_compat_CFDictionarySetValue(iostodroid_CFMutableDictionaryRef dictionary, const void *key,
                                       const void *value);
const void *iostodroid_compat_CFDictionaryGetValue(iostodroid_CFDictionaryRef dictionary, const void *key);
iostodroid_CFIndex iostodroid_compat_CFDictionaryGetCount(iostodroid_CFDictionaryRef dictionary);

iostodroid_CFNumberRef iostodroid_compat_CFNumberCreate(iostodroid_CFAllocatorRef allocator, iostodroid_CFNumberType type,
                                              const void *valuePointer);
iostodroid_Boolean iostodroid_compat_CFNumberGetValue(iostodroid_CFNumberRef number, iostodroid_CFNumberType type,
                                            void *valuePointer);

iostodroid_CFDateRef iostodroid_compat_CFDateCreate(iostodroid_CFAllocatorRef allocator, iostodroid_CFAbsoluteTime absoluteTime);
iostodroid_CFAbsoluteTime iostodroid_compat_CFDateGetAbsoluteTime(iostodroid_CFDateRef date);
iostodroid_CFTimeInterval iostodroid_compat_CFDateGetTimeIntervalSinceDate(iostodroid_CFDateRef date,
                                                                 iostodroid_CFDateRef other);
iostodroid_CFGregorianDate iostodroid_compat_CFAbsoluteTimeGetGregorianDate(iostodroid_CFAbsoluteTime absoluteTime,
                                                                  iostodroid_CFTimeZoneRef timeZone);

/*
 * CoreFoundation run-loop subset. GetCurrent/GetMain and the Run/RunInMode/
 * Stop/WakeUp exports have the Darwin C ABI. Perform is an explicit C callback
 * helper for runtimes that cannot carry Apple Blocks objects across the ABI.
 * This queue is thread-safe but is not a drop-in implementation of every Apple
 * run-loop source, observer, timer, or Objective-C block contract.
 */
iostodroid_CFRunLoopRef iostodroid_compat_CFRunLoopGetCurrent(void);
iostodroid_CFRunLoopRef iostodroid_compat_CFRunLoopGetMain(void);
void iostodroid_compat_CFRunLoopRun(void);
iostodroid_CFRunLoopRunResult iostodroid_compat_CFRunLoopRunInMode(iostodroid_CFStringRef mode,
                                                          iostodroid_CFTimeInterval seconds,
                                                          iostodroid_Boolean returnAfterSourceHandled);
void iostodroid_compat_CFRunLoopStop(iostodroid_CFRunLoopRef runLoop);
void iostodroid_compat_CFRunLoopWakeUp(iostodroid_CFRunLoopRef runLoop);
iostodroid_Boolean iostodroid_compat_CFRunLoopPerform(iostodroid_CFRunLoopRef runLoop,
                                             iostodroid_CFStringRef mode,
                                             iostodroid_CFRunLoopPerformCallback callback,
                                             void *context);

/* --- libc / POSIX forwards (identical C ABI on bionic) -------------------- */

void *iostodroid_compat_malloc(size_t size);
void *iostodroid_compat_calloc(size_t count, size_t size);
void *iostodroid_compat_realloc(void *pointer, size_t size);
void iostodroid_compat_free(void *pointer);
void *iostodroid_compat_memcpy(void *destination, const void *source, size_t size);
void *iostodroid_compat_memmove(void *destination, const void *source, size_t size);
void *iostodroid_compat_memset(void *destination, int value, size_t size);
int iostodroid_compat_memcmp(const void *left, const void *right, size_t size);
void *iostodroid_compat_memchr(const void *source, int value, size_t size);

size_t iostodroid_compat_strlen(const char *string);
char *iostodroid_compat_strcpy(char *destination, const char *source);
char *iostodroid_compat_strncpy(char *destination, const char *source, size_t size);
size_t iostodroid_compat_strlcpy(char *destination, const char *source, size_t size);
size_t iostodroid_compat_strlcat(char *destination, const char *source, size_t size);
int iostodroid_compat_strcmp(const char *left, const char *right);
int iostodroid_compat_strncmp(const char *left, const char *right, size_t size);
char *iostodroid_compat_strdup(const char *string);
char *iostodroid_compat_strchr(const char *string, int value);
char *iostodroid_compat_strrchr(const char *string, int value);
char *iostodroid_compat_strstr(const char *haystack, const char *needle);
long iostodroid_compat_strtol(const char *string, char **end, int base);
double iostodroid_compat_strtod(const char *string, char **end);
int iostodroid_compat_atoi(const char *string);
double iostodroid_compat_atof(const char *string);
char *iostodroid_compat_strerror(int code);
int iostodroid_compat_snprintf(char *buffer, size_t size, const char *format, ...);
int iostodroid_compat_vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);

FILE *iostodroid_compat_fopen(const char *path, const char *mode);
int iostodroid_compat_fclose(FILE *stream);
size_t iostodroid_compat_fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t iostodroid_compat_fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int iostodroid_compat_fputs(const char *string, FILE *stream);
char *iostodroid_compat_fgets(char *buffer, int size, FILE *stream);
int iostodroid_compat_fflush(FILE *stream);
int iostodroid_compat_fprintf(FILE *stream, const char *format, ...);
int iostodroid_compat_printf(const char *format, ...);
int iostodroid_compat_puts(const char *string);
int iostodroid_compat_remove(const char *path);
int iostodroid_compat_feof(FILE *stream);
long iostodroid_compat_ftell(FILE *stream);
int iostodroid_compat_fseek(FILE *stream, long offset, int origin);

time_t iostodroid_compat_time(time_t *result);
int iostodroid_compat_gettimeofday(iostodroid_darwin_timeval *result, void *timeZone);
int iostodroid_compat_clock_gettime(int clockIdentifier, struct timespec *result);
int iostodroid_compat_nanosleep(const struct timespec *request, struct timespec *remaining);
struct tm *iostodroid_compat_localtime_r(const time_t *clock, struct tm *result);
struct tm *iostodroid_compat_gmtime_r(const time_t *clock, struct tm *result);
time_t iostodroid_compat_mktime(struct tm *value);

char *iostodroid_compat_getenv(const char *name);
int iostodroid_compat_setenv(const char *name, const char *value, int overwrite);
int iostodroid_compat_unsetenv(const char *name);
long iostodroid_compat_getpid(void);

void iostodroid_compat_qsort(void *base, size_t count, size_t size,
                        int (*compare)(const void *, const void *));
void *iostodroid_compat_bsearch(const void *key, const void *base, size_t count, size_t size,
                           int (*compare)(const void *, const void *));
int iostodroid_compat_abs(int value);
long iostodroid_compat_labs(long value);
int iostodroid_compat_rand(void);
void iostodroid_compat_srand(unsigned int seed);

double iostodroid_compat_sqrt(double value);
double iostodroid_compat_fabs(double value);
double iostodroid_compat_floor(double value);
double iostodroid_compat_ceil(double value);
double iostodroid_compat_pow(double base, double exponent);
double iostodroid_compat_sin(double value);
double iostodroid_compat_cos(double value);
double iostodroid_compat_tan(double value);
double iostodroid_compat_atan2(double y, double x);
double iostodroid_compat_fmod(double numerator, double denominator);

/*
 * pthread forwards. bionic's pthread_mutex_t / pthread_cond_t are smaller than
 * Darwin's, and callers therefore always over-allocate; every
 * operation below touches only the leading bytes bionic owns. pthread_t is an
 * integer on bionic and a pointer on Darwin, but both live in one 64-bit
 * register, so values round-trip through a call unchanged.
 */
int iostodroid_compat_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes);
int iostodroid_compat_pthread_mutex_lock(pthread_mutex_t *mutex);
int iostodroid_compat_pthread_mutex_unlock(pthread_mutex_t *mutex);
int iostodroid_compat_pthread_mutex_destroy(pthread_mutex_t *mutex);
int iostodroid_compat_pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes);
int iostodroid_compat_pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
int iostodroid_compat_pthread_cond_signal(pthread_cond_t *condition);
int iostodroid_compat_pthread_cond_broadcast(pthread_cond_t *condition);
int iostodroid_compat_pthread_cond_destroy(pthread_cond_t *condition);
pthread_t iostodroid_compat_pthread_self(void);


/*
 * Canonical Darwin symbol -> implementation mapping for everything declared in
 * this header.
 *
 * native/src/ioscompat_registry.cpp (registry seeding) and native/src/jni.cpp
 * (on-device resolver) both expand this table, so the registry and the device
 * resolver can never disagree. iostodroid/api_implementations.py keeps a matching Python
 * table for host source generation, and tests/test_api_implementations.py asserts
 * the two stay identical.
 */
#ifdef __cplusplus
#define IOSTODROID_COMPAT_DEF0 = 0
#else
#define IOSTODROID_COMPAT_DEF0
#endif

/* --- Expanded CoreFoundation, CoreGraphics, OpenAL, AudioToolbox, OpenGL ES, --- */
/* --- Objective-C runtime, UIKit/Foundation, POSIX/libc/libm, and C++ ABI shims --- */
iostodroid_CFTypeRef iostodroid_compat_CFConstantStringClassReference(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFAllocatorDefault(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFBooleanTrue(void);
iostodroid_CFTypeRef iostodroid_compat_CFBooleanFalse(void);
iostodroid_CFTypeRef iostodroid_compat_CFTypeArrayCallBacks(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFTypeDictionaryKeyCallBacks(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFTypeDictionaryValueCallBacks(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFRunLoopDefaultMode(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFRunLoopCommonModes(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFBundleGetMainBundle(void);
iostodroid_CFTypeRef iostodroid_compat_CFBundleCopyBundleURL(iostodroid_CFTypeRef bundle);
iostodroid_CFTypeRef iostodroid_compat_CFBundleCopyResourcesDirectoryURL(iostodroid_CFTypeRef bundle);
iostodroid_CFTypeRef iostodroid_compat_CFBundleCopyResourceURL(iostodroid_CFTypeRef bundle, iostodroid_CFStringRef name, iostodroid_CFStringRef type, iostodroid_CFStringRef subDir);
iostodroid_CFStringRef iostodroid_compat_CFBundleGetIdentifier(iostodroid_CFTypeRef bundle);
iostodroid_CFTypeRef iostodroid_compat_CFBundleGetValueForInfoDictionaryKey(iostodroid_CFTypeRef bundle, iostodroid_CFStringRef key);
iostodroid_CFTypeRef iostodroid_compat_CFURLCreateWithFileSystemPath(iostodroid_CFAllocatorRef alloc, iostodroid_CFStringRef filePath, iostodroid_CFIndex pathStyle, unsigned char isDir);
iostodroid_CFTypeRef iostodroid_compat_CFURLCreateFromFileSystemRepresentation(iostodroid_CFAllocatorRef alloc, const uint8_t *buffer, iostodroid_CFIndex bufLen, unsigned char isDir);
unsigned char iostodroid_compat_CFURLGetFileSystemRepresentation(iostodroid_CFTypeRef url, unsigned char resolveAgainstBase, uint8_t *buffer, iostodroid_CFIndex maxBufLen);
iostodroid_CFStringRef iostodroid_compat_CFURLCopyFileSystemPath(iostodroid_CFTypeRef url, iostodroid_CFIndex pathStyle);
iostodroid_CFTypeRef iostodroid_compat_CFStringCreateWithBytes(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFStringCreateMutable(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFStringAppendCString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFStringHasPrefix(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFStringHasSuffix(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFStringGetIntValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFStringGetDoubleValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFArrayCreate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFArrayRemoveValueAtIndex(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFArrayRemoveAllValues(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDictionaryCreate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDictionaryRemoveValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDictionaryRemoveAllValues(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDictionaryContainsKey(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDataCreateMutable(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDataAppendBytes(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDataGetMutableBytePtr(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFDataGetBytes(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
unsigned char iostodroid_compat_CFBooleanGetValue(iostodroid_CFTypeRef booleanRef);
unsigned char iostodroid_compat_CFEqual(iostodroid_CFTypeRef a, iostodroid_CFTypeRef b);
unsigned long iostodroid_compat_CFHash(iostodroid_CFTypeRef cf);
iostodroid_CFTypeRef iostodroid_compat_CFGetTypeID(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFPreferencesCopyAppValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFPreferencesSetAppValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFPreferencesAppSynchronize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFUUIDCreate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFUUIDCreateString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFLocaleCopyCurrent(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFLocaleCopyPreferredLanguages(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFLocaleGetIdentifier(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
iostodroid_CFTypeRef iostodroid_compat_CFTimeZoneCopySystem(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionInitialize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionSetActive(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSSearchPathForDirectoriesInDomains(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___CAEAGLLayer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___EAGLContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSAutoreleasePool(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSBundle(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSDictionary(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSNumber(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSObject(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSThread(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSURL(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIAccelerometer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIApplication(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIScreen(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIView(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIWindow(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_METACLASS___NSObject(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_METACLASS___UIView(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIApplicationMain(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__DefaultRuneLocale(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_SjLj_Register(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_SjLj_Resume(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_SjLj_Unregister(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__ZSt9terminatev(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__ZTVN10__cxxabiv117__class_type_infoE(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__ZTVN10__cxxabiv119__pointer_type_infoE(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__ZTVN10__cxxabiv120__si_class_type_infoE(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
void iostodroid_compat__ZdaPv(void *ptr);
void iostodroid_compat__ZdlPv(void *ptr);
void *iostodroid_compat__Znam(size_t size);
void *iostodroid_compat__Znwm(size_t size);
uintptr_t iostodroid_compat___cxa_allocate_exception(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_atexit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_begin_catch(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_end_catch(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_pure_virtual(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_throw(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
int64_t iostodroid_compat___divdi3(int64_t a, int64_t b);
int32_t iostodroid_compat___divsi3(int32_t a, int32_t b);
int *iostodroid_compat___error(void);
int64_t iostodroid_compat___fixdfdi(double a);
double iostodroid_compat___floatdidf(int64_t a);
float iostodroid_compat___floatdisf(int64_t a);
uintptr_t iostodroid_compat___gxx_personality_sj0(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___maskrune(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
int64_t iostodroid_compat___moddi3(int64_t a, int64_t b);
int32_t iostodroid_compat___modsi3(int32_t a, int32_t b);
uintptr_t iostodroid_compat___stderrp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___stdinp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___stdoutp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___tolower(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___toupper(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uint32_t iostodroid_compat___udivsi3(uint32_t a, uint32_t b);
uint32_t iostodroid_compat___umodsi3(uint32_t a, uint32_t b);
uintptr_t iostodroid_compat__objc_empty_cache(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__objc_empty_vtable(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_abort(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_acosf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alBufferData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alDeleteBuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alDeleteSources(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
void iostodroid_compat_alGenBuffers(int n, unsigned int *buffers);
void iostodroid_compat_alGenSources(int n, unsigned int *sources);
uintptr_t iostodroid_compat_alGetSourcef(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetSourcei(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSource3f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourcePlay(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourceQueueBuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourceStop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourceUnqueueBuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourcef(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourcei(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcCloseDevice(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
void *iostodroid_compat_alcCreateContext(void *device, const int *attrlist);
uintptr_t iostodroid_compat_alcDestroyContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
char iostodroid_compat_alcMakeContextCurrent(void *context);
void *iostodroid_compat_alcOpenDevice(const char *devicename);
uintptr_t iostodroid_compat_asinf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_atan2f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_atanf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ceilf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_clearerr(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_clock(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_close(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_cosf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_coshf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_difftime(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_exit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_expf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fcntl(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ferror(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_floorf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fputc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_freopen(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_frexp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fscanf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glActiveTexture(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBindBuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBindFramebufferOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBindRenderbufferOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
void iostodroid_compat_glBindTexture(unsigned int target, unsigned int texture);
uintptr_t iostodroid_compat_glBlendFunc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBufferData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
unsigned int iostodroid_compat_glCheckFramebufferStatusOES(unsigned int target);
uintptr_t iostodroid_compat_glClear(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glClearColor(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glClientActiveTexture(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glColor4f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glColorPointer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glCompressedTexImage2D(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteBuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteFramebuffersOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteRenderbuffersOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteTextures(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDepthFunc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDepthMask(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDisable(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDisableClientState(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
void iostodroid_compat_glDrawArrays(unsigned int mode, int first, int count);
void iostodroid_compat_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices);
uintptr_t iostodroid_compat_glEnable(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glEnableClientState(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFramebufferRenderbufferOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFramebufferTexture2DOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
void iostodroid_compat_glFrontFace(unsigned int mode);
uintptr_t iostodroid_compat_glGenBuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGenFramebuffersOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGenRenderbuffersOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
void iostodroid_compat_glGenTextures(int n, unsigned int *textures);
uintptr_t iostodroid_compat_glGetIntegerv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetRenderbufferParameterivOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glLightfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glLineWidth(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glLoadMatrixf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glMaterialfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glMatrixMode(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glNormalPointer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glPixelStorei(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glRenderbufferStorageOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glScissor(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexCoordPointer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexEnvi(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexImage2D(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexParameteri(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexSubImage2D(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glVertexPointer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glViewport(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_gmtime(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_kEAGLColorFormatRGB565(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_kEAGLColorFormatRGBA8(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_kEAGLDrawablePropertyColorFormat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_kEAGLDrawablePropertyRetainedBacking(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ldexp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_localeconv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_localtime(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_log10f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_logf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_longjmp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_lseek(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_modf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_enumerationMutation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_msgSend(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_msgSendSuper2(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_msgSend_stret(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_setProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_create(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_exit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_getschedparam(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_join(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_mutex_trylock(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_mutexattr_destroy(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_mutexattr_init(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_mutexattr_settype(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_setschedparam(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_read(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_rename(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sched_yield(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_select(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_setjmp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_setlocale(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_setvbuf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sinf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sinhf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sprintf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strcasecmp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strcat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strcoll(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strcspn(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strftime(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strncat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strpbrk(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strtok(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strtoul(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_system(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_tanf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_tanhf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_tmpfile(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_tmpnam(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ungetc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_usleep(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_vsprintf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__exit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_atexit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sscanf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_putchar(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getchar(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fgetc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_putc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_rewind(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fileno(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fdopen(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_perror(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_tzset(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sleep(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_open(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_write(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_unlink(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_mkdir(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_rmdir(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_access(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getcwd(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_chdir(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_stat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fstat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_lstat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_opendir(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_readdir(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_closedir(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_mmap(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_munmap(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_mprotect(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_poll(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pipe(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dup(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dup2(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fsync(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ftruncate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_truncate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_chmod(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_umask(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getuid(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_geteuid(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getgid(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getegid(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getppid(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sysconf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sysctl(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sysctlbyname(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getpagesize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__setjmp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__longjmp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sigaction(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_signal(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_raise(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_kill(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_tolower(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_toupper(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_isalpha(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_isdigit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_isalnum(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_isspace(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_isupper(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_islower(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_isxdigit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strncasecmp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strspn(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strtok_r(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strtoll(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strtoull(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_strtof(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_atol(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_atoll(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_llabs(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_bzero(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_bcopy(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_bcmp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_acos(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_asin(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_atan(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_cosh(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sinh(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_tanh(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_exp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_log(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_log10(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_log2(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_hypot(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_hypotf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_cbrt(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_round(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_roundf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_trunc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_truncf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_lround(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_lroundf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_frexpf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ldexpf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_log2f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_modff(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_powf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sqrtf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fabsf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_fmodf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_detach(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_equal(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_once(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_cond_timedwait(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_key_create(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_key_delete(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_setspecific(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_getspecific(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_rwlock_init(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_rwlock_rdlock(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_rwlock_wrlock(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_rwlock_unlock(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_pthread_rwlock_destroy(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sem_init(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sem_destroy(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sem_wait(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sem_trywait(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sem_post(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dlopen(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dlsym(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dlclose(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dlerror(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_socket(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_connect(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_bind(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_listen(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_accept(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_send(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sendto(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_recv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_recvfrom(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_setsockopt(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getsockopt(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getsockname(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getpeername(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_shutdown(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_getaddrinfo(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_freeaddrinfo(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_gethostbyname(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inet_ntop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inet_pton(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inet_addr(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inet_ntoa(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_htons(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_htonl(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ntohs(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ntohl(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_crc32(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_adler32(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_compress(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_compress2(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_uncompress(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_deflateInit_(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_deflateInit2_(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_deflate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_deflateEnd(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_deflateReset(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inflateInit_(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inflateInit2_(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inflate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inflateEnd(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_inflateReset(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_gzopen(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_gzread(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_gzwrite(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_gzclose(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alDistanceModel(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alDopplerFactor(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alDopplerVelocity(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSpeedOfSound(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetError(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetSource3f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetSourcefv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourcefv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourcePause(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alSourceRewind(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alListener3f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alListenerf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alListenerfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alListeneri(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetListenerf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetListener3f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetListenerfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alEnable(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alDisable(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alIsEnabled(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alIsBuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alIsSource(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetBoolean(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetInteger(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetFloat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetDouble(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetEnumValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alGetProcAddress(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alIsExtensionPresent(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcGetContextsDevice(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcGetCurrentContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcProcessContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcSuspendContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcGetError(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcGetIntegerv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcGetString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcIsExtensionPresent(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_alcGetProcAddress(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionSetActiveWithFlags(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionGetProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionSetProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionGetPropertySize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionAddPropertyListener(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioSessionRemovePropertyListenerWithUserData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioServicesPlaySystemSound(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioServicesPlayAlertSound(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioServicesCreateSystemSoundID(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioServicesDisposeSystemSoundID(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioFileOpenURL(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioFileClose(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioFileGetProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioFileReadBytes(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioFileReadPackets(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ExtAudioFileOpenURL(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ExtAudioFileDispose(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ExtAudioFileGetProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ExtAudioFileSetProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ExtAudioFileRead(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_ExtAudioFileSeek(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueNewOutput(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueAllocateBuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueFreeBuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueEnqueueBuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueStart(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueuePause(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueStop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueDispose(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioQueueSetParameter(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioComponentFindNext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioComponentInstanceNew(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioComponentInstanceDispose(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioUnitInitialize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioUnitUninitialize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioUnitSetProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioUnitGetProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioOutputUnitStart(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioOutputUnitStop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_AudioUnitRender(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glAlphaFunc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBindFramebuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBindRenderbuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBlendEquation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBlendEquationOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBlendFuncSeparate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glBufferSubData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
unsigned int iostodroid_compat_glCheckFramebufferStatus(unsigned int target);
uintptr_t iostodroid_compat_glClearDepthf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glClearStencil(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glColor4ub(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glColorMask(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glCompileShader(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glCopyTexImage2D(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glCopyTexSubImage2D(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glCreateProgram(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glCreateShader(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glCullFace(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteFramebuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteProgram(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteRenderbuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDeleteShader(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDepthRangef(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glDisableVertexAttribArray(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glEnableVertexAttribArray(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFinish(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFlush(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFogf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFogfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFramebufferRenderbuffer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFramebufferTexture2D(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glFrustumf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGenFramebuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGenRenderbuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGenerateMipmap(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGenerateMipmapOES(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetAttribLocation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetError(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetFloatv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetProgramInfoLog(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetProgramiv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetRenderbufferParameteriv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetShaderInfoLog(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetShaderiv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glGetUniformLocation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glHint(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glIsEnabled(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glIsTexture(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glLightModelfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glLinkProgram(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glLoadIdentity(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glLogicOp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glMaterialf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glMultMatrixf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glNormal3f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glOrthof(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glPointParameterf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glPointParameterfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glPointSize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glPolygonOffset(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glPopMatrix(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glPushMatrix(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glReadPixels(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glRenderbufferStorage(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glRotatef(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glScalef(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glShadeModel(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glShaderSource(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glStencilFunc(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glStencilMask(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glStencilOp(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexEnvf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexEnvfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexParameterf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTexParameterfv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glTranslatef(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glUniform1f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glUniform1i(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glUniform2f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glUniform3f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glUniform4f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glUniformMatrix4fv(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glUseProgram(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_glVertexAttribPointer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglGetDisplay(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglInitialize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglChooseConfig(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglCreateWindowSurface(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglCreateContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglMakeCurrent(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglSwapBuffers(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglDestroyContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglDestroySurface(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglTerminate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglGetError(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_eglGetProcAddress(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGColorSpaceCreateDeviceRGB(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGColorSpaceCreateDeviceGray(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGColorSpaceRelease(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGColorSpaceRetain(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGBitmapContextCreate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGBitmapContextGetData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGBitmapContextGetWidth(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGBitmapContextGetHeight(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGBitmapContextGetBytesPerRow(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGBitmapContextCreateImage(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextRelease(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextRetain(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextClearRect(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextFillRect(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextDrawImage(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextTranslateCTM(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextScaleCTM(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextRotateCTM(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextSaveGState(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextRestoreGState(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextSetRGBFillColor(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGContextSetAlpha(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetWidth(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetHeight(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetBitsPerComponent(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetBitsPerPixel(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetBytesPerRow(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetAlphaInfo(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetDataProvider(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageGetColorSpace(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageRelease(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGImageRetain(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGDataProviderCopyData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGDataProviderCreateWithData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGDataProviderRelease(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGDataProviderRetain(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformMake(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformMakeTranslation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformMakeScale(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformMakeRotation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformTranslate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformScale(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformRotate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CGAffineTransformConcat(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_msgSendSuper(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_msgSendSuper_stret(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_msgSendSuper2_stret(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_msgSend_fpret(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_getClass(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_lookUpClass(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_getMetaClass(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_getProtocol(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_allocateClassPair(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_registerClassPair(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_retain(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_release(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_autorelease(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_autoreleasePoolPush(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_autoreleasePoolPop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_retainAutorelease(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_retainAutoreleaseReturnValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_retainAutoreleasedReturnValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_storeStrong(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_storeWeak(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_loadWeakRetained(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_destroyWeak(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_getProperty(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_copyStruct(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_sync_enter(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_sync_exit(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_exception_throw(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_begin_catch(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_objc_end_catch(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sel_registerName(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sel_getUid(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_sel_getName(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_class_getName(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_class_getSuperclass(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_class_getInstanceMethod(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_class_getClassMethod(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_class_addMethod(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_class_replaceMethod(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_class_createInstance(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_object_getClass(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_object_getClassName(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___MPMoviePlayerController(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSDate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSLocale(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSNotificationCenter(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSUserDefaults(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIColor(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIDevice(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIImage(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIViewController(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___AVAudioPlayer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___AVAudioSession(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSArray(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableArray(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableDictionary(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSSet(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableSet(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSFileManager(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSTimer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSRunLoop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSProcessInfo(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSValue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___NSError(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIImageView(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UILabel(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIButton(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIScrollView(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIAlertView(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIActivityIndicatorView(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIWebView(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIFont(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UITouch(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___UIEvent(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___CALayer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___CATransaction(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___CABasicAnimation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___SKPaymentQueue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___SKProductsRequest(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___GKLocalPlayer(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___CMMotionManager(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_CLASS___GCController(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_METACLASS___UIViewController(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_OBJC_METACLASS___UIApplication(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIGraphicsPushContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIGraphicsPopContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIGraphicsGetCurrentContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIGraphicsBeginImageContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIGraphicsBeginImageContextWithOptions(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIGraphicsGetImageFromCurrentImageContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIGraphicsEndImageContext(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIImagePNGRepresentation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIImageJPEGRepresentation(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_UIImageWriteToSavedPhotosAlbum(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSTemporaryDirectory(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSHomeDirectory(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSLog(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSStringFromClass(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSClassFromString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSStringFromSelector(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSSelectorFromString(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_NSPageSize(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_async(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_sync(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_after(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_once(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_async_f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_sync_f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_once_f(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_get_main_queue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_get_global_queue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_queue_create(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_release(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_retain(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_time(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_semaphore_create(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_semaphore_wait(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_semaphore_signal(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_group_create(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_group_async(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_group_enter(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_group_leave(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_group_wait(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_dispatch_group_notify(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__dispatch_main_q(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SCNetworkReachabilityCreateWithAddress(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SCNetworkReachabilityCreateWithName(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SCNetworkReachabilityGetFlags(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SCNetworkReachabilitySetCallback(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SCNetworkReachabilityScheduleWithRunLoop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SCNetworkReachabilityUnscheduleFromRunLoop(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SCNetworkReachabilitySetDispatchQueue(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SecRandomCopyBytes(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SecItemCopyMatching(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SecItemAdd(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SecItemUpdate(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_SecItemDelete(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CC_MD5(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CC_SHA1(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat_CC_SHA256(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_DeleteException(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_GetIP(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_SetIP(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_GetGR(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_SetGR(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_GetLanguageSpecificData(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Unwind_GetRegionStart(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___gxx_personality_v0(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___gcc_personality_v0(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uint64_t iostodroid_compat___udivdi3(uint64_t a, uint64_t b);
uint64_t iostodroid_compat___umoddi3(uint64_t a, uint64_t b);
uintptr_t iostodroid_compat___muldi3(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___fixsfdi(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___fixunsdfdi(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___fixunssfdi(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___floatundidf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___floatundisf(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___ashldi3(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___ashrdi3(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___lshrdi3(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cmpdi2(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___ucmpdi2(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___clear_cache(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Znaj(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat__Znwj(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_free_exception(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_rethrow(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_guard_acquire(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_guard_release(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_guard_abort(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___cxa_demangle(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);
uintptr_t iostodroid_compat___dynamic_cast(uintptr_t a0 IOSTODROID_COMPAT_DEF0, uintptr_t a1 IOSTODROID_COMPAT_DEF0, uintptr_t a2 IOSTODROID_COMPAT_DEF0, uintptr_t a3 IOSTODROID_COMPAT_DEF0);

#define IOSTODROID_IOS_SHIM_TABLE(X) \
    X("_CFAllocatorGetDefault", iostodroid_compat_CFAllocatorGetDefault) \
    X("_CFRetain", iostodroid_compat_CFRetain) \
    X("_CFRelease", iostodroid_compat_CFRelease) \
    X("_CFGetRetainCount", iostodroid_compat_CFGetRetainCount) \
    X("_CFStringCreateWithCString", iostodroid_compat_CFStringCreateWithCString) \
    X("_CFStringGetLength", iostodroid_compat_CFStringGetLength) \
    X("_CFStringGetCString", iostodroid_compat_CFStringGetCString) \
    X("_CFStringGetCStringPtr", iostodroid_compat_CFStringGetCStringPtr) \
    X("_CFStringGetMaximumSizeForEncoding", iostodroid_compat_CFStringGetMaximumSizeForEncoding) \
    X("_CFStringCompare", iostodroid_compat_CFStringCompare) \
    X("_CFStringGetSystemEncoding", iostodroid_compat_CFStringGetSystemEncoding) \
    X("_CFDataCreate", iostodroid_compat_CFDataCreate) \
    X("_CFDataGetBytePtr", iostodroid_compat_CFDataGetBytePtr) \
    X("_CFDataGetLength", iostodroid_compat_CFDataGetLength) \
    X("_CFArrayCreateMutable", iostodroid_compat_CFArrayCreateMutable) \
    X("_CFArrayAppendValue", iostodroid_compat_CFArrayAppendValue) \
    X("_CFArrayGetCount", iostodroid_compat_CFArrayGetCount) \
    X("_CFArrayGetValueAtIndex", iostodroid_compat_CFArrayGetValueAtIndex) \
    X("_CFDictionaryCreateMutable", iostodroid_compat_CFDictionaryCreateMutable) \
    X("_CFDictionarySetValue", iostodroid_compat_CFDictionarySetValue) \
    X("_CFDictionaryGetValue", iostodroid_compat_CFDictionaryGetValue) \
    X("_CFDictionaryGetCount", iostodroid_compat_CFDictionaryGetCount) \
    X("_CFNumberCreate", iostodroid_compat_CFNumberCreate) \
    X("_CFNumberGetValue", iostodroid_compat_CFNumberGetValue) \
    X("_CFDateCreate", iostodroid_compat_CFDateCreate) \
    X("_CFDateGetAbsoluteTime", iostodroid_compat_CFDateGetAbsoluteTime) \
    X("_CFDateGetTimeIntervalSinceDate", iostodroid_compat_CFDateGetTimeIntervalSinceDate) \
    X("_CFAbsoluteTimeGetGregorianDate", iostodroid_compat_CFAbsoluteTimeGetGregorianDate) \
    X("_CFRunLoopGetCurrent", iostodroid_compat_CFRunLoopGetCurrent) \
    X("_CFRunLoopGetMain", iostodroid_compat_CFRunLoopGetMain) \
    X("_CFRunLoopRun", iostodroid_compat_CFRunLoopRun) \
    X("_CFRunLoopRunInMode", iostodroid_compat_CFRunLoopRunInMode) \
    X("_CFRunLoopStop", iostodroid_compat_CFRunLoopStop) \
    X("_CFRunLoopWakeUp", iostodroid_compat_CFRunLoopWakeUp) \
    X("_malloc", iostodroid_compat_malloc) \
    X("_calloc", iostodroid_compat_calloc) \
    X("_realloc", iostodroid_compat_realloc) \
    X("_free", iostodroid_compat_free) \
    X("_memcpy", iostodroid_compat_memcpy) \
    X("_memmove", iostodroid_compat_memmove) \
    X("_memset", iostodroid_compat_memset) \
    X("_memcmp", iostodroid_compat_memcmp) \
    X("_memchr", iostodroid_compat_memchr) \
    X("_strlen", iostodroid_compat_strlen) \
    X("_strcpy", iostodroid_compat_strcpy) \
    X("_strncpy", iostodroid_compat_strncpy) \
    X("_strlcpy", iostodroid_compat_strlcpy) \
    X("_strlcat", iostodroid_compat_strlcat) \
    X("_strcmp", iostodroid_compat_strcmp) \
    X("_strncmp", iostodroid_compat_strncmp) \
    X("_strdup", iostodroid_compat_strdup) \
    X("_strchr", iostodroid_compat_strchr) \
    X("_strrchr", iostodroid_compat_strrchr) \
    X("_strstr", iostodroid_compat_strstr) \
    X("_strtol", iostodroid_compat_strtol) \
    X("_strtod", iostodroid_compat_strtod) \
    X("_atoi", iostodroid_compat_atoi) \
    X("_atof", iostodroid_compat_atof) \
    X("_strerror", iostodroid_compat_strerror) \
    X("_snprintf", iostodroid_compat_snprintf) \
    X("_vsnprintf", iostodroid_compat_vsnprintf) \
    X("_fopen", iostodroid_compat_fopen) \
    X("_fclose", iostodroid_compat_fclose) \
    X("_fread", iostodroid_compat_fread) \
    X("_fwrite", iostodroid_compat_fwrite) \
    X("_fputs", iostodroid_compat_fputs) \
    X("_fgets", iostodroid_compat_fgets) \
    X("_fflush", iostodroid_compat_fflush) \
    X("_fprintf", iostodroid_compat_fprintf) \
    X("_printf", iostodroid_compat_printf) \
    X("_puts", iostodroid_compat_puts) \
    X("_remove", iostodroid_compat_remove) \
    X("_feof", iostodroid_compat_feof) \
    X("_ftell", iostodroid_compat_ftell) \
    X("_fseek", iostodroid_compat_fseek) \
    X("_time", iostodroid_compat_time) \
    X("_gettimeofday", iostodroid_compat_gettimeofday) \
    X("_clock_gettime", iostodroid_compat_clock_gettime) \
    X("_nanosleep", iostodroid_compat_nanosleep) \
    X("_localtime_r", iostodroid_compat_localtime_r) \
    X("_gmtime_r", iostodroid_compat_gmtime_r) \
    X("_mktime", iostodroid_compat_mktime) \
    X("_getenv", iostodroid_compat_getenv) \
    X("_setenv", iostodroid_compat_setenv) \
    X("_unsetenv", iostodroid_compat_unsetenv) \
    X("_getpid", iostodroid_compat_getpid) \
    X("_qsort", iostodroid_compat_qsort) \
    X("_bsearch", iostodroid_compat_bsearch) \
    X("_abs", iostodroid_compat_abs) \
    X("_labs", iostodroid_compat_labs) \
    X("_rand", iostodroid_compat_rand) \
    X("_srand", iostodroid_compat_srand) \
    X("_sqrt", iostodroid_compat_sqrt) \
    X("_fabs", iostodroid_compat_fabs) \
    X("_floor", iostodroid_compat_floor) \
    X("_ceil", iostodroid_compat_ceil) \
    X("_pow", iostodroid_compat_pow) \
    X("_sin", iostodroid_compat_sin) \
    X("_cos", iostodroid_compat_cos) \
    X("_tan", iostodroid_compat_tan) \
    X("_atan2", iostodroid_compat_atan2) \
    X("_fmod", iostodroid_compat_fmod) \
    X("_pthread_mutex_init", iostodroid_compat_pthread_mutex_init) \
    X("_pthread_mutex_lock", iostodroid_compat_pthread_mutex_lock) \
    X("_pthread_mutex_unlock", iostodroid_compat_pthread_mutex_unlock) \
    X("_pthread_mutex_destroy", iostodroid_compat_pthread_mutex_destroy) \
    X("_pthread_cond_init", iostodroid_compat_pthread_cond_init) \
    X("_pthread_cond_wait", iostodroid_compat_pthread_cond_wait) \
    X("_pthread_cond_signal", iostodroid_compat_pthread_cond_signal) \
    X("_pthread_cond_broadcast", iostodroid_compat_pthread_cond_broadcast) \
    X("_pthread_cond_destroy", iostodroid_compat_pthread_cond_destroy) \
    X("_pthread_self", iostodroid_compat_pthread_self) \
    X("___CFConstantStringClassReference", iostodroid_compat_CFConstantStringClassReference) \
    X("_kCFAllocatorDefault", iostodroid_compat_CFAllocatorDefault) \
    X("_kCFBooleanTrue", iostodroid_compat_CFBooleanTrue) \
    X("_kCFBooleanFalse", iostodroid_compat_CFBooleanFalse) \
    X("_kCFTypeArrayCallBacks", iostodroid_compat_CFTypeArrayCallBacks) \
    X("_kCFTypeDictionaryKeyCallBacks", iostodroid_compat_CFTypeDictionaryKeyCallBacks) \
    X("_kCFTypeDictionaryValueCallBacks", iostodroid_compat_CFTypeDictionaryValueCallBacks) \
    X("_kCFRunLoopDefaultMode", iostodroid_compat_CFRunLoopDefaultMode) \
    X("_kCFRunLoopCommonModes", iostodroid_compat_CFRunLoopCommonModes) \
    X("_CFBundleGetMainBundle", iostodroid_compat_CFBundleGetMainBundle) \
    X("_CFBundleCopyBundleURL", iostodroid_compat_CFBundleCopyBundleURL) \
    X("_CFBundleCopyResourcesDirectoryURL", iostodroid_compat_CFBundleCopyResourcesDirectoryURL) \
    X("_CFBundleCopyResourceURL", iostodroid_compat_CFBundleCopyResourceURL) \
    X("_CFBundleGetIdentifier", iostodroid_compat_CFBundleGetIdentifier) \
    X("_CFBundleGetValueForInfoDictionaryKey", iostodroid_compat_CFBundleGetValueForInfoDictionaryKey) \
    X("_CFURLCreateWithFileSystemPath", iostodroid_compat_CFURLCreateWithFileSystemPath) \
    X("_CFURLCreateFromFileSystemRepresentation", iostodroid_compat_CFURLCreateFromFileSystemRepresentation) \
    X("_CFURLGetFileSystemRepresentation", iostodroid_compat_CFURLGetFileSystemRepresentation) \
    X("_CFURLCopyFileSystemPath", iostodroid_compat_CFURLCopyFileSystemPath) \
    X("_CFStringCreateWithBytes", iostodroid_compat_CFStringCreateWithBytes) \
    X("_CFStringCreateMutable", iostodroid_compat_CFStringCreateMutable) \
    X("_CFStringAppendCString", iostodroid_compat_CFStringAppendCString) \
    X("_CFStringHasPrefix", iostodroid_compat_CFStringHasPrefix) \
    X("_CFStringHasSuffix", iostodroid_compat_CFStringHasSuffix) \
    X("_CFStringGetIntValue", iostodroid_compat_CFStringGetIntValue) \
    X("_CFStringGetDoubleValue", iostodroid_compat_CFStringGetDoubleValue) \
    X("_CFArrayCreate", iostodroid_compat_CFArrayCreate) \
    X("_CFArrayRemoveValueAtIndex", iostodroid_compat_CFArrayRemoveValueAtIndex) \
    X("_CFArrayRemoveAllValues", iostodroid_compat_CFArrayRemoveAllValues) \
    X("_CFDictionaryCreate", iostodroid_compat_CFDictionaryCreate) \
    X("_CFDictionaryRemoveValue", iostodroid_compat_CFDictionaryRemoveValue) \
    X("_CFDictionaryRemoveAllValues", iostodroid_compat_CFDictionaryRemoveAllValues) \
    X("_CFDictionaryContainsKey", iostodroid_compat_CFDictionaryContainsKey) \
    X("_CFDataCreateMutable", iostodroid_compat_CFDataCreateMutable) \
    X("_CFDataAppendBytes", iostodroid_compat_CFDataAppendBytes) \
    X("_CFDataGetMutableBytePtr", iostodroid_compat_CFDataGetMutableBytePtr) \
    X("_CFDataGetBytes", iostodroid_compat_CFDataGetBytes) \
    X("_CFBooleanGetValue", iostodroid_compat_CFBooleanGetValue) \
    X("_CFEqual", iostodroid_compat_CFEqual) \
    X("_CFHash", iostodroid_compat_CFHash) \
    X("_CFGetTypeID", iostodroid_compat_CFGetTypeID) \
    X("_CFPreferencesCopyAppValue", iostodroid_compat_CFPreferencesCopyAppValue) \
    X("_CFPreferencesSetAppValue", iostodroid_compat_CFPreferencesSetAppValue) \
    X("_CFPreferencesAppSynchronize", iostodroid_compat_CFPreferencesAppSynchronize) \
    X("_CFUUIDCreate", iostodroid_compat_CFUUIDCreate) \
    X("_CFUUIDCreateString", iostodroid_compat_CFUUIDCreateString) \
    X("_CFLocaleCopyCurrent", iostodroid_compat_CFLocaleCopyCurrent) \
    X("_CFLocaleCopyPreferredLanguages", iostodroid_compat_CFLocaleCopyPreferredLanguages) \
    X("_CFLocaleGetIdentifier", iostodroid_compat_CFLocaleGetIdentifier) \
    X("_CFTimeZoneCopySystem", iostodroid_compat_CFTimeZoneCopySystem) \
    X("_AudioSessionInitialize", iostodroid_compat_AudioSessionInitialize) \
    X("_AudioSessionSetActive", iostodroid_compat_AudioSessionSetActive) \
    X("_NSSearchPathForDirectoriesInDomains", iostodroid_compat_NSSearchPathForDirectoriesInDomains) \
    X("_OBJC_CLASS_$_CAEAGLLayer", iostodroid_compat_OBJC_CLASS___CAEAGLLayer) \
    X("_OBJC_CLASS_$_EAGLContext", iostodroid_compat_OBJC_CLASS___EAGLContext) \
    X("_OBJC_CLASS_$_NSAutoreleasePool", iostodroid_compat_OBJC_CLASS___NSAutoreleasePool) \
    X("_OBJC_CLASS_$_NSBundle", iostodroid_compat_OBJC_CLASS___NSBundle) \
    X("_OBJC_CLASS_$_NSDictionary", iostodroid_compat_OBJC_CLASS___NSDictionary) \
    X("_OBJC_CLASS_$_NSNumber", iostodroid_compat_OBJC_CLASS___NSNumber) \
    X("_OBJC_CLASS_$_NSObject", iostodroid_compat_OBJC_CLASS___NSObject) \
    X("_OBJC_CLASS_$_NSString", iostodroid_compat_OBJC_CLASS___NSString) \
    X("_OBJC_CLASS_$_NSThread", iostodroid_compat_OBJC_CLASS___NSThread) \
    X("_OBJC_CLASS_$_NSURL", iostodroid_compat_OBJC_CLASS___NSURL) \
    X("_OBJC_CLASS_$_UIAccelerometer", iostodroid_compat_OBJC_CLASS___UIAccelerometer) \
    X("_OBJC_CLASS_$_UIApplication", iostodroid_compat_OBJC_CLASS___UIApplication) \
    X("_OBJC_CLASS_$_UIScreen", iostodroid_compat_OBJC_CLASS___UIScreen) \
    X("_OBJC_CLASS_$_UIView", iostodroid_compat_OBJC_CLASS___UIView) \
    X("_OBJC_CLASS_$_UIWindow", iostodroid_compat_OBJC_CLASS___UIWindow) \
    X("_OBJC_METACLASS_$_NSObject", iostodroid_compat_OBJC_METACLASS___NSObject) \
    X("_OBJC_METACLASS_$_UIView", iostodroid_compat_OBJC_METACLASS___UIView) \
    X("_UIApplicationMain", iostodroid_compat_UIApplicationMain) \
    X("__DefaultRuneLocale", iostodroid_compat__DefaultRuneLocale) \
    X("__Unwind_SjLj_Register", iostodroid_compat__Unwind_SjLj_Register) \
    X("__Unwind_SjLj_Resume", iostodroid_compat__Unwind_SjLj_Resume) \
    X("__Unwind_SjLj_Unregister", iostodroid_compat__Unwind_SjLj_Unregister) \
    X("__ZSt9terminatev", iostodroid_compat__ZSt9terminatev) \
    X("__ZTVN10__cxxabiv117__class_type_infoE", iostodroid_compat__ZTVN10__cxxabiv117__class_type_infoE) \
    X("__ZTVN10__cxxabiv119__pointer_type_infoE", iostodroid_compat__ZTVN10__cxxabiv119__pointer_type_infoE) \
    X("__ZTVN10__cxxabiv120__si_class_type_infoE", iostodroid_compat__ZTVN10__cxxabiv120__si_class_type_infoE) \
    X("__ZTVN10__cxxabiv121__vmi_class_type_infoE", iostodroid_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE) \
    X("__ZdaPv", iostodroid_compat__ZdaPv) \
    X("__ZdlPv", iostodroid_compat__ZdlPv) \
    X("__Znam", iostodroid_compat__Znam) \
    X("__Znwm", iostodroid_compat__Znwm) \
    X("___cxa_allocate_exception", iostodroid_compat___cxa_allocate_exception) \
    X("___cxa_atexit", iostodroid_compat___cxa_atexit) \
    X("___cxa_begin_catch", iostodroid_compat___cxa_begin_catch) \
    X("___cxa_end_catch", iostodroid_compat___cxa_end_catch) \
    X("___cxa_pure_virtual", iostodroid_compat___cxa_pure_virtual) \
    X("___cxa_throw", iostodroid_compat___cxa_throw) \
    X("___divdi3", iostodroid_compat___divdi3) \
    X("___divsi3", iostodroid_compat___divsi3) \
    X("___error", iostodroid_compat___error) \
    X("___fixdfdi", iostodroid_compat___fixdfdi) \
    X("___floatdidf", iostodroid_compat___floatdidf) \
    X("___floatdisf", iostodroid_compat___floatdisf) \
    X("___gxx_personality_sj0", iostodroid_compat___gxx_personality_sj0) \
    X("___maskrune", iostodroid_compat___maskrune) \
    X("___moddi3", iostodroid_compat___moddi3) \
    X("___modsi3", iostodroid_compat___modsi3) \
    X("___stderrp", iostodroid_compat___stderrp) \
    X("___stdinp", iostodroid_compat___stdinp) \
    X("___stdoutp", iostodroid_compat___stdoutp) \
    X("___tolower", iostodroid_compat___tolower) \
    X("___toupper", iostodroid_compat___toupper) \
    X("___udivsi3", iostodroid_compat___udivsi3) \
    X("___umodsi3", iostodroid_compat___umodsi3) \
    X("__objc_empty_cache", iostodroid_compat__objc_empty_cache) \
    X("__objc_empty_vtable", iostodroid_compat__objc_empty_vtable) \
    X("_abort", iostodroid_compat_abort) \
    X("_acosf", iostodroid_compat_acosf) \
    X("_alBufferData", iostodroid_compat_alBufferData) \
    X("_alDeleteBuffers", iostodroid_compat_alDeleteBuffers) \
    X("_alDeleteSources", iostodroid_compat_alDeleteSources) \
    X("_alGenBuffers", iostodroid_compat_alGenBuffers) \
    X("_alGenSources", iostodroid_compat_alGenSources) \
    X("_alGetSourcef", iostodroid_compat_alGetSourcef) \
    X("_alGetSourcei", iostodroid_compat_alGetSourcei) \
    X("_alSource3f", iostodroid_compat_alSource3f) \
    X("_alSourcePlay", iostodroid_compat_alSourcePlay) \
    X("_alSourceQueueBuffers", iostodroid_compat_alSourceQueueBuffers) \
    X("_alSourceStop", iostodroid_compat_alSourceStop) \
    X("_alSourceUnqueueBuffers", iostodroid_compat_alSourceUnqueueBuffers) \
    X("_alSourcef", iostodroid_compat_alSourcef) \
    X("_alSourcei", iostodroid_compat_alSourcei) \
    X("_alcCloseDevice", iostodroid_compat_alcCloseDevice) \
    X("_alcCreateContext", iostodroid_compat_alcCreateContext) \
    X("_alcDestroyContext", iostodroid_compat_alcDestroyContext) \
    X("_alcMakeContextCurrent", iostodroid_compat_alcMakeContextCurrent) \
    X("_alcOpenDevice", iostodroid_compat_alcOpenDevice) \
    X("_asinf", iostodroid_compat_asinf) \
    X("_atan2f", iostodroid_compat_atan2f) \
    X("_atanf", iostodroid_compat_atanf) \
    X("_ceilf", iostodroid_compat_ceilf) \
    X("_clearerr", iostodroid_compat_clearerr) \
    X("_clock", iostodroid_compat_clock) \
    X("_close", iostodroid_compat_close) \
    X("_cosf", iostodroid_compat_cosf) \
    X("_coshf", iostodroid_compat_coshf) \
    X("_difftime", iostodroid_compat_difftime) \
    X("_exit", iostodroid_compat_exit) \
    X("_expf", iostodroid_compat_expf) \
    X("_fcntl", iostodroid_compat_fcntl) \
    X("_ferror", iostodroid_compat_ferror) \
    X("_floorf", iostodroid_compat_floorf) \
    X("_fputc", iostodroid_compat_fputc) \
    X("_freopen", iostodroid_compat_freopen) \
    X("_frexp", iostodroid_compat_frexp) \
    X("_fscanf", iostodroid_compat_fscanf) \
    X("_getc", iostodroid_compat_getc) \
    X("_glActiveTexture", iostodroid_compat_glActiveTexture) \
    X("_glBindBuffer", iostodroid_compat_glBindBuffer) \
    X("_glBindFramebufferOES", iostodroid_compat_glBindFramebufferOES) \
    X("_glBindRenderbufferOES", iostodroid_compat_glBindRenderbufferOES) \
    X("_glBindTexture", iostodroid_compat_glBindTexture) \
    X("_glBlendFunc", iostodroid_compat_glBlendFunc) \
    X("_glBufferData", iostodroid_compat_glBufferData) \
    X("_glCheckFramebufferStatusOES", iostodroid_compat_glCheckFramebufferStatusOES) \
    X("_glClear", iostodroid_compat_glClear) \
    X("_glClearColor", iostodroid_compat_glClearColor) \
    X("_glClientActiveTexture", iostodroid_compat_glClientActiveTexture) \
    X("_glColor4f", iostodroid_compat_glColor4f) \
    X("_glColorPointer", iostodroid_compat_glColorPointer) \
    X("_glCompressedTexImage2D", iostodroid_compat_glCompressedTexImage2D) \
    X("_glDeleteBuffers", iostodroid_compat_glDeleteBuffers) \
    X("_glDeleteFramebuffersOES", iostodroid_compat_glDeleteFramebuffersOES) \
    X("_glDeleteRenderbuffersOES", iostodroid_compat_glDeleteRenderbuffersOES) \
    X("_glDeleteTextures", iostodroid_compat_glDeleteTextures) \
    X("_glDepthFunc", iostodroid_compat_glDepthFunc) \
    X("_glDepthMask", iostodroid_compat_glDepthMask) \
    X("_glDisable", iostodroid_compat_glDisable) \
    X("_glDisableClientState", iostodroid_compat_glDisableClientState) \
    X("_glDrawArrays", iostodroid_compat_glDrawArrays) \
    X("_glDrawElements", iostodroid_compat_glDrawElements) \
    X("_glEnable", iostodroid_compat_glEnable) \
    X("_glEnableClientState", iostodroid_compat_glEnableClientState) \
    X("_glFramebufferRenderbufferOES", iostodroid_compat_glFramebufferRenderbufferOES) \
    X("_glFramebufferTexture2DOES", iostodroid_compat_glFramebufferTexture2DOES) \
    X("_glFrontFace", iostodroid_compat_glFrontFace) \
    X("_glGenBuffers", iostodroid_compat_glGenBuffers) \
    X("_glGenFramebuffersOES", iostodroid_compat_glGenFramebuffersOES) \
    X("_glGenRenderbuffersOES", iostodroid_compat_glGenRenderbuffersOES) \
    X("_glGenTextures", iostodroid_compat_glGenTextures) \
    X("_glGetIntegerv", iostodroid_compat_glGetIntegerv) \
    X("_glGetRenderbufferParameterivOES", iostodroid_compat_glGetRenderbufferParameterivOES) \
    X("_glLightfv", iostodroid_compat_glLightfv) \
    X("_glLineWidth", iostodroid_compat_glLineWidth) \
    X("_glLoadMatrixf", iostodroid_compat_glLoadMatrixf) \
    X("_glMaterialfv", iostodroid_compat_glMaterialfv) \
    X("_glMatrixMode", iostodroid_compat_glMatrixMode) \
    X("_glNormalPointer", iostodroid_compat_glNormalPointer) \
    X("_glPixelStorei", iostodroid_compat_glPixelStorei) \
    X("_glRenderbufferStorageOES", iostodroid_compat_glRenderbufferStorageOES) \
    X("_glScissor", iostodroid_compat_glScissor) \
    X("_glTexCoordPointer", iostodroid_compat_glTexCoordPointer) \
    X("_glTexEnvi", iostodroid_compat_glTexEnvi) \
    X("_glTexImage2D", iostodroid_compat_glTexImage2D) \
    X("_glTexParameteri", iostodroid_compat_glTexParameteri) \
    X("_glTexSubImage2D", iostodroid_compat_glTexSubImage2D) \
    X("_glVertexPointer", iostodroid_compat_glVertexPointer) \
    X("_glViewport", iostodroid_compat_glViewport) \
    X("_gmtime", iostodroid_compat_gmtime) \
    X("_kEAGLColorFormatRGB565", iostodroid_compat_kEAGLColorFormatRGB565) \
    X("_kEAGLColorFormatRGBA8", iostodroid_compat_kEAGLColorFormatRGBA8) \
    X("_kEAGLDrawablePropertyColorFormat", iostodroid_compat_kEAGLDrawablePropertyColorFormat) \
    X("_kEAGLDrawablePropertyRetainedBacking", iostodroid_compat_kEAGLDrawablePropertyRetainedBacking) \
    X("_ldexp", iostodroid_compat_ldexp) \
    X("_localeconv", iostodroid_compat_localeconv) \
    X("_localtime", iostodroid_compat_localtime) \
    X("_log10f", iostodroid_compat_log10f) \
    X("_logf", iostodroid_compat_logf) \
    X("_longjmp", iostodroid_compat_longjmp) \
    X("_lseek", iostodroid_compat_lseek) \
    X("_modf", iostodroid_compat_modf) \
    X("_objc_enumerationMutation", iostodroid_compat_objc_enumerationMutation) \
    X("_objc_msgSend", iostodroid_compat_objc_msgSend) \
    X("_objc_msgSendSuper2", iostodroid_compat_objc_msgSendSuper2) \
    X("_objc_msgSend_stret", iostodroid_compat_objc_msgSend_stret) \
    X("_objc_setProperty", iostodroid_compat_objc_setProperty) \
    X("_pthread_create", iostodroid_compat_pthread_create) \
    X("_pthread_exit", iostodroid_compat_pthread_exit) \
    X("_pthread_getschedparam", iostodroid_compat_pthread_getschedparam) \
    X("_pthread_join", iostodroid_compat_pthread_join) \
    X("_pthread_mutex_trylock", iostodroid_compat_pthread_mutex_trylock) \
    X("_pthread_mutexattr_destroy", iostodroid_compat_pthread_mutexattr_destroy) \
    X("_pthread_mutexattr_init", iostodroid_compat_pthread_mutexattr_init) \
    X("_pthread_mutexattr_settype", iostodroid_compat_pthread_mutexattr_settype) \
    X("_pthread_setschedparam", iostodroid_compat_pthread_setschedparam) \
    X("_read", iostodroid_compat_read) \
    X("_rename", iostodroid_compat_rename) \
    X("_sched_yield", iostodroid_compat_sched_yield) \
    X("_select", iostodroid_compat_select) \
    X("_setjmp", iostodroid_compat_setjmp) \
    X("_setlocale", iostodroid_compat_setlocale) \
    X("_setvbuf", iostodroid_compat_setvbuf) \
    X("_sinf", iostodroid_compat_sinf) \
    X("_sinhf", iostodroid_compat_sinhf) \
    X("_sprintf", iostodroid_compat_sprintf) \
    X("_strcasecmp", iostodroid_compat_strcasecmp) \
    X("_strcat", iostodroid_compat_strcat) \
    X("_strcoll", iostodroid_compat_strcoll) \
    X("_strcspn", iostodroid_compat_strcspn) \
    X("_strftime", iostodroid_compat_strftime) \
    X("_strncat", iostodroid_compat_strncat) \
    X("_strpbrk", iostodroid_compat_strpbrk) \
    X("_strtok", iostodroid_compat_strtok) \
    X("_strtoul", iostodroid_compat_strtoul) \
    X("_system", iostodroid_compat_system) \
    X("_tanf", iostodroid_compat_tanf) \
    X("_tanhf", iostodroid_compat_tanhf) \
    X("_tmpfile", iostodroid_compat_tmpfile) \
    X("_tmpnam", iostodroid_compat_tmpnam) \
    X("_ungetc", iostodroid_compat_ungetc) \
    X("_usleep", iostodroid_compat_usleep) \
    X("_vsprintf", iostodroid_compat_vsprintf) \
    X("__exit", iostodroid_compat__exit) \
    X("_atexit", iostodroid_compat_atexit) \
    X("_sscanf", iostodroid_compat_sscanf) \
    X("_putchar", iostodroid_compat_putchar) \
    X("_getchar", iostodroid_compat_getchar) \
    X("_fgetc", iostodroid_compat_fgetc) \
    X("_putc", iostodroid_compat_putc) \
    X("_rewind", iostodroid_compat_rewind) \
    X("_fileno", iostodroid_compat_fileno) \
    X("_fdopen", iostodroid_compat_fdopen) \
    X("_perror", iostodroid_compat_perror) \
    X("_tzset", iostodroid_compat_tzset) \
    X("_sleep", iostodroid_compat_sleep) \
    X("_open", iostodroid_compat_open) \
    X("_write", iostodroid_compat_write) \
    X("_unlink", iostodroid_compat_unlink) \
    X("_mkdir", iostodroid_compat_mkdir) \
    X("_rmdir", iostodroid_compat_rmdir) \
    X("_access", iostodroid_compat_access) \
    X("_getcwd", iostodroid_compat_getcwd) \
    X("_chdir", iostodroid_compat_chdir) \
    X("_stat", iostodroid_compat_stat) \
    X("_fstat", iostodroid_compat_fstat) \
    X("_lstat", iostodroid_compat_lstat) \
    X("_opendir", iostodroid_compat_opendir) \
    X("_readdir", iostodroid_compat_readdir) \
    X("_closedir", iostodroid_compat_closedir) \
    X("_mmap", iostodroid_compat_mmap) \
    X("_munmap", iostodroid_compat_munmap) \
    X("_mprotect", iostodroid_compat_mprotect) \
    X("_poll", iostodroid_compat_poll) \
    X("_pipe", iostodroid_compat_pipe) \
    X("_dup", iostodroid_compat_dup) \
    X("_dup2", iostodroid_compat_dup2) \
    X("_fsync", iostodroid_compat_fsync) \
    X("_ftruncate", iostodroid_compat_ftruncate) \
    X("_truncate", iostodroid_compat_truncate) \
    X("_chmod", iostodroid_compat_chmod) \
    X("_umask", iostodroid_compat_umask) \
    X("_getuid", iostodroid_compat_getuid) \
    X("_geteuid", iostodroid_compat_geteuid) \
    X("_getgid", iostodroid_compat_getgid) \
    X("_getegid", iostodroid_compat_getegid) \
    X("_getppid", iostodroid_compat_getppid) \
    X("_sysconf", iostodroid_compat_sysconf) \
    X("_sysctl", iostodroid_compat_sysctl) \
    X("_sysctlbyname", iostodroid_compat_sysctlbyname) \
    X("_getpagesize", iostodroid_compat_getpagesize) \
    X("__setjmp", iostodroid_compat__setjmp) \
    X("__longjmp", iostodroid_compat__longjmp) \
    X("_sigaction", iostodroid_compat_sigaction) \
    X("_signal", iostodroid_compat_signal) \
    X("_raise", iostodroid_compat_raise) \
    X("_kill", iostodroid_compat_kill) \
    X("_tolower", iostodroid_compat_tolower) \
    X("_toupper", iostodroid_compat_toupper) \
    X("_isalpha", iostodroid_compat_isalpha) \
    X("_isdigit", iostodroid_compat_isdigit) \
    X("_isalnum", iostodroid_compat_isalnum) \
    X("_isspace", iostodroid_compat_isspace) \
    X("_isupper", iostodroid_compat_isupper) \
    X("_islower", iostodroid_compat_islower) \
    X("_isxdigit", iostodroid_compat_isxdigit) \
    X("_strncasecmp", iostodroid_compat_strncasecmp) \
    X("_strspn", iostodroid_compat_strspn) \
    X("_strtok_r", iostodroid_compat_strtok_r) \
    X("_strtoll", iostodroid_compat_strtoll) \
    X("_strtoull", iostodroid_compat_strtoull) \
    X("_strtof", iostodroid_compat_strtof) \
    X("_atol", iostodroid_compat_atol) \
    X("_atoll", iostodroid_compat_atoll) \
    X("_llabs", iostodroid_compat_llabs) \
    X("_bzero", iostodroid_compat_bzero) \
    X("_bcopy", iostodroid_compat_bcopy) \
    X("_bcmp", iostodroid_compat_bcmp) \
    X("_acos", iostodroid_compat_acos) \
    X("_asin", iostodroid_compat_asin) \
    X("_atan", iostodroid_compat_atan) \
    X("_cosh", iostodroid_compat_cosh) \
    X("_sinh", iostodroid_compat_sinh) \
    X("_tanh", iostodroid_compat_tanh) \
    X("_exp", iostodroid_compat_exp) \
    X("_log", iostodroid_compat_log) \
    X("_log10", iostodroid_compat_log10) \
    X("_log2", iostodroid_compat_log2) \
    X("_hypot", iostodroid_compat_hypot) \
    X("_hypotf", iostodroid_compat_hypotf) \
    X("_cbrt", iostodroid_compat_cbrt) \
    X("_round", iostodroid_compat_round) \
    X("_roundf", iostodroid_compat_roundf) \
    X("_trunc", iostodroid_compat_trunc) \
    X("_truncf", iostodroid_compat_truncf) \
    X("_lround", iostodroid_compat_lround) \
    X("_lroundf", iostodroid_compat_lroundf) \
    X("_frexpf", iostodroid_compat_frexpf) \
    X("_ldexpf", iostodroid_compat_ldexpf) \
    X("_log2f", iostodroid_compat_log2f) \
    X("_modff", iostodroid_compat_modff) \
    X("_powf", iostodroid_compat_powf) \
    X("_sqrtf", iostodroid_compat_sqrtf) \
    X("_fabsf", iostodroid_compat_fabsf) \
    X("_fmodf", iostodroid_compat_fmodf) \
    X("_pthread_detach", iostodroid_compat_pthread_detach) \
    X("_pthread_equal", iostodroid_compat_pthread_equal) \
    X("_pthread_once", iostodroid_compat_pthread_once) \
    X("_pthread_cond_timedwait", iostodroid_compat_pthread_cond_timedwait) \
    X("_pthread_key_create", iostodroid_compat_pthread_key_create) \
    X("_pthread_key_delete", iostodroid_compat_pthread_key_delete) \
    X("_pthread_setspecific", iostodroid_compat_pthread_setspecific) \
    X("_pthread_getspecific", iostodroid_compat_pthread_getspecific) \
    X("_pthread_rwlock_init", iostodroid_compat_pthread_rwlock_init) \
    X("_pthread_rwlock_rdlock", iostodroid_compat_pthread_rwlock_rdlock) \
    X("_pthread_rwlock_wrlock", iostodroid_compat_pthread_rwlock_wrlock) \
    X("_pthread_rwlock_unlock", iostodroid_compat_pthread_rwlock_unlock) \
    X("_pthread_rwlock_destroy", iostodroid_compat_pthread_rwlock_destroy) \
    X("_sem_init", iostodroid_compat_sem_init) \
    X("_sem_destroy", iostodroid_compat_sem_destroy) \
    X("_sem_wait", iostodroid_compat_sem_wait) \
    X("_sem_trywait", iostodroid_compat_sem_trywait) \
    X("_sem_post", iostodroid_compat_sem_post) \
    X("_dlopen", iostodroid_compat_dlopen) \
    X("_dlsym", iostodroid_compat_dlsym) \
    X("_dlclose", iostodroid_compat_dlclose) \
    X("_dlerror", iostodroid_compat_dlerror) \
    X("_socket", iostodroid_compat_socket) \
    X("_connect", iostodroid_compat_connect) \
    X("_bind", iostodroid_compat_bind) \
    X("_listen", iostodroid_compat_listen) \
    X("_accept", iostodroid_compat_accept) \
    X("_send", iostodroid_compat_send) \
    X("_sendto", iostodroid_compat_sendto) \
    X("_recv", iostodroid_compat_recv) \
    X("_recvfrom", iostodroid_compat_recvfrom) \
    X("_setsockopt", iostodroid_compat_setsockopt) \
    X("_getsockopt", iostodroid_compat_getsockopt) \
    X("_getsockname", iostodroid_compat_getsockname) \
    X("_getpeername", iostodroid_compat_getpeername) \
    X("_shutdown", iostodroid_compat_shutdown) \
    X("_getaddrinfo", iostodroid_compat_getaddrinfo) \
    X("_freeaddrinfo", iostodroid_compat_freeaddrinfo) \
    X("_gethostbyname", iostodroid_compat_gethostbyname) \
    X("_inet_ntop", iostodroid_compat_inet_ntop) \
    X("_inet_pton", iostodroid_compat_inet_pton) \
    X("_inet_addr", iostodroid_compat_inet_addr) \
    X("_inet_ntoa", iostodroid_compat_inet_ntoa) \
    X("_htons", iostodroid_compat_htons) \
    X("_htonl", iostodroid_compat_htonl) \
    X("_ntohs", iostodroid_compat_ntohs) \
    X("_ntohl", iostodroid_compat_ntohl) \
    X("_crc32", iostodroid_compat_crc32) \
    X("_adler32", iostodroid_compat_adler32) \
    X("_compress", iostodroid_compat_compress) \
    X("_compress2", iostodroid_compat_compress2) \
    X("_uncompress", iostodroid_compat_uncompress) \
    X("_deflateInit_", iostodroid_compat_deflateInit_) \
    X("_deflateInit2_", iostodroid_compat_deflateInit2_) \
    X("_deflate", iostodroid_compat_deflate) \
    X("_deflateEnd", iostodroid_compat_deflateEnd) \
    X("_deflateReset", iostodroid_compat_deflateReset) \
    X("_inflateInit_", iostodroid_compat_inflateInit_) \
    X("_inflateInit2_", iostodroid_compat_inflateInit2_) \
    X("_inflate", iostodroid_compat_inflate) \
    X("_inflateEnd", iostodroid_compat_inflateEnd) \
    X("_inflateReset", iostodroid_compat_inflateReset) \
    X("_gzopen", iostodroid_compat_gzopen) \
    X("_gzread", iostodroid_compat_gzread) \
    X("_gzwrite", iostodroid_compat_gzwrite) \
    X("_gzclose", iostodroid_compat_gzclose) \
    X("_alDistanceModel", iostodroid_compat_alDistanceModel) \
    X("_alDopplerFactor", iostodroid_compat_alDopplerFactor) \
    X("_alDopplerVelocity", iostodroid_compat_alDopplerVelocity) \
    X("_alSpeedOfSound", iostodroid_compat_alSpeedOfSound) \
    X("_alGetError", iostodroid_compat_alGetError) \
    X("_alGetSource3f", iostodroid_compat_alGetSource3f) \
    X("_alGetSourcefv", iostodroid_compat_alGetSourcefv) \
    X("_alSourcefv", iostodroid_compat_alSourcefv) \
    X("_alSourcePause", iostodroid_compat_alSourcePause) \
    X("_alSourceRewind", iostodroid_compat_alSourceRewind) \
    X("_alListener3f", iostodroid_compat_alListener3f) \
    X("_alListenerf", iostodroid_compat_alListenerf) \
    X("_alListenerfv", iostodroid_compat_alListenerfv) \
    X("_alListeneri", iostodroid_compat_alListeneri) \
    X("_alGetListenerf", iostodroid_compat_alGetListenerf) \
    X("_alGetListener3f", iostodroid_compat_alGetListener3f) \
    X("_alGetListenerfv", iostodroid_compat_alGetListenerfv) \
    X("_alEnable", iostodroid_compat_alEnable) \
    X("_alDisable", iostodroid_compat_alDisable) \
    X("_alIsEnabled", iostodroid_compat_alIsEnabled) \
    X("_alIsBuffer", iostodroid_compat_alIsBuffer) \
    X("_alIsSource", iostodroid_compat_alIsSource) \
    X("_alGetBoolean", iostodroid_compat_alGetBoolean) \
    X("_alGetInteger", iostodroid_compat_alGetInteger) \
    X("_alGetFloat", iostodroid_compat_alGetFloat) \
    X("_alGetDouble", iostodroid_compat_alGetDouble) \
    X("_alGetString", iostodroid_compat_alGetString) \
    X("_alGetEnumValue", iostodroid_compat_alGetEnumValue) \
    X("_alGetProcAddress", iostodroid_compat_alGetProcAddress) \
    X("_alIsExtensionPresent", iostodroid_compat_alIsExtensionPresent) \
    X("_alcGetContextsDevice", iostodroid_compat_alcGetContextsDevice) \
    X("_alcGetCurrentContext", iostodroid_compat_alcGetCurrentContext) \
    X("_alcProcessContext", iostodroid_compat_alcProcessContext) \
    X("_alcSuspendContext", iostodroid_compat_alcSuspendContext) \
    X("_alcGetError", iostodroid_compat_alcGetError) \
    X("_alcGetIntegerv", iostodroid_compat_alcGetIntegerv) \
    X("_alcGetString", iostodroid_compat_alcGetString) \
    X("_alcIsExtensionPresent", iostodroid_compat_alcIsExtensionPresent) \
    X("_alcGetProcAddress", iostodroid_compat_alcGetProcAddress) \
    X("_AudioSessionSetActiveWithFlags", iostodroid_compat_AudioSessionSetActiveWithFlags) \
    X("_AudioSessionGetProperty", iostodroid_compat_AudioSessionGetProperty) \
    X("_AudioSessionSetProperty", iostodroid_compat_AudioSessionSetProperty) \
    X("_AudioSessionGetPropertySize", iostodroid_compat_AudioSessionGetPropertySize) \
    X("_AudioSessionAddPropertyListener", iostodroid_compat_AudioSessionAddPropertyListener) \
    X("_AudioSessionRemovePropertyListenerWithUserData", iostodroid_compat_AudioSessionRemovePropertyListenerWithUserData) \
    X("_AudioServicesPlaySystemSound", iostodroid_compat_AudioServicesPlaySystemSound) \
    X("_AudioServicesPlayAlertSound", iostodroid_compat_AudioServicesPlayAlertSound) \
    X("_AudioServicesCreateSystemSoundID", iostodroid_compat_AudioServicesCreateSystemSoundID) \
    X("_AudioServicesDisposeSystemSoundID", iostodroid_compat_AudioServicesDisposeSystemSoundID) \
    X("_AudioFileOpenURL", iostodroid_compat_AudioFileOpenURL) \
    X("_AudioFileClose", iostodroid_compat_AudioFileClose) \
    X("_AudioFileGetProperty", iostodroid_compat_AudioFileGetProperty) \
    X("_AudioFileReadBytes", iostodroid_compat_AudioFileReadBytes) \
    X("_AudioFileReadPackets", iostodroid_compat_AudioFileReadPackets) \
    X("_ExtAudioFileOpenURL", iostodroid_compat_ExtAudioFileOpenURL) \
    X("_ExtAudioFileDispose", iostodroid_compat_ExtAudioFileDispose) \
    X("_ExtAudioFileGetProperty", iostodroid_compat_ExtAudioFileGetProperty) \
    X("_ExtAudioFileSetProperty", iostodroid_compat_ExtAudioFileSetProperty) \
    X("_ExtAudioFileRead", iostodroid_compat_ExtAudioFileRead) \
    X("_ExtAudioFileSeek", iostodroid_compat_ExtAudioFileSeek) \
    X("_AudioQueueNewOutput", iostodroid_compat_AudioQueueNewOutput) \
    X("_AudioQueueAllocateBuffer", iostodroid_compat_AudioQueueAllocateBuffer) \
    X("_AudioQueueFreeBuffer", iostodroid_compat_AudioQueueFreeBuffer) \
    X("_AudioQueueEnqueueBuffer", iostodroid_compat_AudioQueueEnqueueBuffer) \
    X("_AudioQueueStart", iostodroid_compat_AudioQueueStart) \
    X("_AudioQueuePause", iostodroid_compat_AudioQueuePause) \
    X("_AudioQueueStop", iostodroid_compat_AudioQueueStop) \
    X("_AudioQueueDispose", iostodroid_compat_AudioQueueDispose) \
    X("_AudioQueueSetParameter", iostodroid_compat_AudioQueueSetParameter) \
    X("_AudioComponentFindNext", iostodroid_compat_AudioComponentFindNext) \
    X("_AudioComponentInstanceNew", iostodroid_compat_AudioComponentInstanceNew) \
    X("_AudioComponentInstanceDispose", iostodroid_compat_AudioComponentInstanceDispose) \
    X("_AudioUnitInitialize", iostodroid_compat_AudioUnitInitialize) \
    X("_AudioUnitUninitialize", iostodroid_compat_AudioUnitUninitialize) \
    X("_AudioUnitSetProperty", iostodroid_compat_AudioUnitSetProperty) \
    X("_AudioUnitGetProperty", iostodroid_compat_AudioUnitGetProperty) \
    X("_AudioOutputUnitStart", iostodroid_compat_AudioOutputUnitStart) \
    X("_AudioOutputUnitStop", iostodroid_compat_AudioOutputUnitStop) \
    X("_AudioUnitRender", iostodroid_compat_AudioUnitRender) \
    X("_glAlphaFunc", iostodroid_compat_glAlphaFunc) \
    X("_glBindFramebuffer", iostodroid_compat_glBindFramebuffer) \
    X("_glBindRenderbuffer", iostodroid_compat_glBindRenderbuffer) \
    X("_glBlendEquation", iostodroid_compat_glBlendEquation) \
    X("_glBlendEquationOES", iostodroid_compat_glBlendEquationOES) \
    X("_glBlendFuncSeparate", iostodroid_compat_glBlendFuncSeparate) \
    X("_glBufferSubData", iostodroid_compat_glBufferSubData) \
    X("_glCheckFramebufferStatus", iostodroid_compat_glCheckFramebufferStatus) \
    X("_glClearDepthf", iostodroid_compat_glClearDepthf) \
    X("_glClearStencil", iostodroid_compat_glClearStencil) \
    X("_glColor4ub", iostodroid_compat_glColor4ub) \
    X("_glColorMask", iostodroid_compat_glColorMask) \
    X("_glCompileShader", iostodroid_compat_glCompileShader) \
    X("_glCopyTexImage2D", iostodroid_compat_glCopyTexImage2D) \
    X("_glCopyTexSubImage2D", iostodroid_compat_glCopyTexSubImage2D) \
    X("_glCreateProgram", iostodroid_compat_glCreateProgram) \
    X("_glCreateShader", iostodroid_compat_glCreateShader) \
    X("_glCullFace", iostodroid_compat_glCullFace) \
    X("_glDeleteFramebuffers", iostodroid_compat_glDeleteFramebuffers) \
    X("_glDeleteProgram", iostodroid_compat_glDeleteProgram) \
    X("_glDeleteRenderbuffers", iostodroid_compat_glDeleteRenderbuffers) \
    X("_glDeleteShader", iostodroid_compat_glDeleteShader) \
    X("_glDepthRangef", iostodroid_compat_glDepthRangef) \
    X("_glDisableVertexAttribArray", iostodroid_compat_glDisableVertexAttribArray) \
    X("_glEnableVertexAttribArray", iostodroid_compat_glEnableVertexAttribArray) \
    X("_glFinish", iostodroid_compat_glFinish) \
    X("_glFlush", iostodroid_compat_glFlush) \
    X("_glFogf", iostodroid_compat_glFogf) \
    X("_glFogfv", iostodroid_compat_glFogfv) \
    X("_glFramebufferRenderbuffer", iostodroid_compat_glFramebufferRenderbuffer) \
    X("_glFramebufferTexture2D", iostodroid_compat_glFramebufferTexture2D) \
    X("_glFrustumf", iostodroid_compat_glFrustumf) \
    X("_glGenFramebuffers", iostodroid_compat_glGenFramebuffers) \
    X("_glGenRenderbuffers", iostodroid_compat_glGenRenderbuffers) \
    X("_glGenerateMipmap", iostodroid_compat_glGenerateMipmap) \
    X("_glGenerateMipmapOES", iostodroid_compat_glGenerateMipmapOES) \
    X("_glGetAttribLocation", iostodroid_compat_glGetAttribLocation) \
    X("_glGetError", iostodroid_compat_glGetError) \
    X("_glGetFloatv", iostodroid_compat_glGetFloatv) \
    X("_glGetProgramInfoLog", iostodroid_compat_glGetProgramInfoLog) \
    X("_glGetProgramiv", iostodroid_compat_glGetProgramiv) \
    X("_glGetRenderbufferParameteriv", iostodroid_compat_glGetRenderbufferParameteriv) \
    X("_glGetShaderInfoLog", iostodroid_compat_glGetShaderInfoLog) \
    X("_glGetShaderiv", iostodroid_compat_glGetShaderiv) \
    X("_glGetString", iostodroid_compat_glGetString) \
    X("_glGetUniformLocation", iostodroid_compat_glGetUniformLocation) \
    X("_glHint", iostodroid_compat_glHint) \
    X("_glIsEnabled", iostodroid_compat_glIsEnabled) \
    X("_glIsTexture", iostodroid_compat_glIsTexture) \
    X("_glLightModelfv", iostodroid_compat_glLightModelfv) \
    X("_glLinkProgram", iostodroid_compat_glLinkProgram) \
    X("_glLoadIdentity", iostodroid_compat_glLoadIdentity) \
    X("_glLogicOp", iostodroid_compat_glLogicOp) \
    X("_glMaterialf", iostodroid_compat_glMaterialf) \
    X("_glMultMatrixf", iostodroid_compat_glMultMatrixf) \
    X("_glNormal3f", iostodroid_compat_glNormal3f) \
    X("_glOrthof", iostodroid_compat_glOrthof) \
    X("_glPointParameterf", iostodroid_compat_glPointParameterf) \
    X("_glPointParameterfv", iostodroid_compat_glPointParameterfv) \
    X("_glPointSize", iostodroid_compat_glPointSize) \
    X("_glPolygonOffset", iostodroid_compat_glPolygonOffset) \
    X("_glPopMatrix", iostodroid_compat_glPopMatrix) \
    X("_glPushMatrix", iostodroid_compat_glPushMatrix) \
    X("_glReadPixels", iostodroid_compat_glReadPixels) \
    X("_glRenderbufferStorage", iostodroid_compat_glRenderbufferStorage) \
    X("_glRotatef", iostodroid_compat_glRotatef) \
    X("_glScalef", iostodroid_compat_glScalef) \
    X("_glShadeModel", iostodroid_compat_glShadeModel) \
    X("_glShaderSource", iostodroid_compat_glShaderSource) \
    X("_glStencilFunc", iostodroid_compat_glStencilFunc) \
    X("_glStencilMask", iostodroid_compat_glStencilMask) \
    X("_glStencilOp", iostodroid_compat_glStencilOp) \
    X("_glTexEnvf", iostodroid_compat_glTexEnvf) \
    X("_glTexEnvfv", iostodroid_compat_glTexEnvfv) \
    X("_glTexParameterf", iostodroid_compat_glTexParameterf) \
    X("_glTexParameterfv", iostodroid_compat_glTexParameterfv) \
    X("_glTranslatef", iostodroid_compat_glTranslatef) \
    X("_glUniform1f", iostodroid_compat_glUniform1f) \
    X("_glUniform1i", iostodroid_compat_glUniform1i) \
    X("_glUniform2f", iostodroid_compat_glUniform2f) \
    X("_glUniform3f", iostodroid_compat_glUniform3f) \
    X("_glUniform4f", iostodroid_compat_glUniform4f) \
    X("_glUniformMatrix4fv", iostodroid_compat_glUniformMatrix4fv) \
    X("_glUseProgram", iostodroid_compat_glUseProgram) \
    X("_glVertexAttribPointer", iostodroid_compat_glVertexAttribPointer) \
    X("_eglGetDisplay", iostodroid_compat_eglGetDisplay) \
    X("_eglInitialize", iostodroid_compat_eglInitialize) \
    X("_eglChooseConfig", iostodroid_compat_eglChooseConfig) \
    X("_eglCreateWindowSurface", iostodroid_compat_eglCreateWindowSurface) \
    X("_eglCreateContext", iostodroid_compat_eglCreateContext) \
    X("_eglMakeCurrent", iostodroid_compat_eglMakeCurrent) \
    X("_eglSwapBuffers", iostodroid_compat_eglSwapBuffers) \
    X("_eglDestroyContext", iostodroid_compat_eglDestroyContext) \
    X("_eglDestroySurface", iostodroid_compat_eglDestroySurface) \
    X("_eglTerminate", iostodroid_compat_eglTerminate) \
    X("_eglGetError", iostodroid_compat_eglGetError) \
    X("_eglGetProcAddress", iostodroid_compat_eglGetProcAddress) \
    X("_CGColorSpaceCreateDeviceRGB", iostodroid_compat_CGColorSpaceCreateDeviceRGB) \
    X("_CGColorSpaceCreateDeviceGray", iostodroid_compat_CGColorSpaceCreateDeviceGray) \
    X("_CGColorSpaceRelease", iostodroid_compat_CGColorSpaceRelease) \
    X("_CGColorSpaceRetain", iostodroid_compat_CGColorSpaceRetain) \
    X("_CGBitmapContextCreate", iostodroid_compat_CGBitmapContextCreate) \
    X("_CGBitmapContextGetData", iostodroid_compat_CGBitmapContextGetData) \
    X("_CGBitmapContextGetWidth", iostodroid_compat_CGBitmapContextGetWidth) \
    X("_CGBitmapContextGetHeight", iostodroid_compat_CGBitmapContextGetHeight) \
    X("_CGBitmapContextGetBytesPerRow", iostodroid_compat_CGBitmapContextGetBytesPerRow) \
    X("_CGBitmapContextCreateImage", iostodroid_compat_CGBitmapContextCreateImage) \
    X("_CGContextRelease", iostodroid_compat_CGContextRelease) \
    X("_CGContextRetain", iostodroid_compat_CGContextRetain) \
    X("_CGContextClearRect", iostodroid_compat_CGContextClearRect) \
    X("_CGContextFillRect", iostodroid_compat_CGContextFillRect) \
    X("_CGContextDrawImage", iostodroid_compat_CGContextDrawImage) \
    X("_CGContextTranslateCTM", iostodroid_compat_CGContextTranslateCTM) \
    X("_CGContextScaleCTM", iostodroid_compat_CGContextScaleCTM) \
    X("_CGContextRotateCTM", iostodroid_compat_CGContextRotateCTM) \
    X("_CGContextSaveGState", iostodroid_compat_CGContextSaveGState) \
    X("_CGContextRestoreGState", iostodroid_compat_CGContextRestoreGState) \
    X("_CGContextSetRGBFillColor", iostodroid_compat_CGContextSetRGBFillColor) \
    X("_CGContextSetAlpha", iostodroid_compat_CGContextSetAlpha) \
    X("_CGImageGetWidth", iostodroid_compat_CGImageGetWidth) \
    X("_CGImageGetHeight", iostodroid_compat_CGImageGetHeight) \
    X("_CGImageGetBitsPerComponent", iostodroid_compat_CGImageGetBitsPerComponent) \
    X("_CGImageGetBitsPerPixel", iostodroid_compat_CGImageGetBitsPerPixel) \
    X("_CGImageGetBytesPerRow", iostodroid_compat_CGImageGetBytesPerRow) \
    X("_CGImageGetAlphaInfo", iostodroid_compat_CGImageGetAlphaInfo) \
    X("_CGImageGetDataProvider", iostodroid_compat_CGImageGetDataProvider) \
    X("_CGImageGetColorSpace", iostodroid_compat_CGImageGetColorSpace) \
    X("_CGImageRelease", iostodroid_compat_CGImageRelease) \
    X("_CGImageRetain", iostodroid_compat_CGImageRetain) \
    X("_CGDataProviderCopyData", iostodroid_compat_CGDataProviderCopyData) \
    X("_CGDataProviderCreateWithData", iostodroid_compat_CGDataProviderCreateWithData) \
    X("_CGDataProviderRelease", iostodroid_compat_CGDataProviderRelease) \
    X("_CGDataProviderRetain", iostodroid_compat_CGDataProviderRetain) \
    X("_CGAffineTransformMake", iostodroid_compat_CGAffineTransformMake) \
    X("_CGAffineTransformMakeTranslation", iostodroid_compat_CGAffineTransformMakeTranslation) \
    X("_CGAffineTransformMakeScale", iostodroid_compat_CGAffineTransformMakeScale) \
    X("_CGAffineTransformMakeRotation", iostodroid_compat_CGAffineTransformMakeRotation) \
    X("_CGAffineTransformTranslate", iostodroid_compat_CGAffineTransformTranslate) \
    X("_CGAffineTransformScale", iostodroid_compat_CGAffineTransformScale) \
    X("_CGAffineTransformRotate", iostodroid_compat_CGAffineTransformRotate) \
    X("_CGAffineTransformConcat", iostodroid_compat_CGAffineTransformConcat) \
    X("_objc_msgSendSuper", iostodroid_compat_objc_msgSendSuper) \
    X("_objc_msgSendSuper_stret", iostodroid_compat_objc_msgSendSuper_stret) \
    X("_objc_msgSendSuper2_stret", iostodroid_compat_objc_msgSendSuper2_stret) \
    X("_objc_msgSend_fpret", iostodroid_compat_objc_msgSend_fpret) \
    X("_objc_getClass", iostodroid_compat_objc_getClass) \
    X("_objc_lookUpClass", iostodroid_compat_objc_lookUpClass) \
    X("_objc_getMetaClass", iostodroid_compat_objc_getMetaClass) \
    X("_objc_getProtocol", iostodroid_compat_objc_getProtocol) \
    X("_objc_allocateClassPair", iostodroid_compat_objc_allocateClassPair) \
    X("_objc_registerClassPair", iostodroid_compat_objc_registerClassPair) \
    X("_objc_retain", iostodroid_compat_objc_retain) \
    X("_objc_release", iostodroid_compat_objc_release) \
    X("_objc_autorelease", iostodroid_compat_objc_autorelease) \
    X("_objc_autoreleasePoolPush", iostodroid_compat_objc_autoreleasePoolPush) \
    X("_objc_autoreleasePoolPop", iostodroid_compat_objc_autoreleasePoolPop) \
    X("_objc_retainAutorelease", iostodroid_compat_objc_retainAutorelease) \
    X("_objc_retainAutoreleaseReturnValue", iostodroid_compat_objc_retainAutoreleaseReturnValue) \
    X("_objc_retainAutoreleasedReturnValue", iostodroid_compat_objc_retainAutoreleasedReturnValue) \
    X("_objc_storeStrong", iostodroid_compat_objc_storeStrong) \
    X("_objc_storeWeak", iostodroid_compat_objc_storeWeak) \
    X("_objc_loadWeakRetained", iostodroid_compat_objc_loadWeakRetained) \
    X("_objc_destroyWeak", iostodroid_compat_objc_destroyWeak) \
    X("_objc_getProperty", iostodroid_compat_objc_getProperty) \
    X("_objc_copyStruct", iostodroid_compat_objc_copyStruct) \
    X("_objc_sync_enter", iostodroid_compat_objc_sync_enter) \
    X("_objc_sync_exit", iostodroid_compat_objc_sync_exit) \
    X("_objc_exception_throw", iostodroid_compat_objc_exception_throw) \
    X("_objc_begin_catch", iostodroid_compat_objc_begin_catch) \
    X("_objc_end_catch", iostodroid_compat_objc_end_catch) \
    X("_sel_registerName", iostodroid_compat_sel_registerName) \
    X("_sel_getUid", iostodroid_compat_sel_getUid) \
    X("_sel_getName", iostodroid_compat_sel_getName) \
    X("_class_getName", iostodroid_compat_class_getName) \
    X("_class_getSuperclass", iostodroid_compat_class_getSuperclass) \
    X("_class_getInstanceMethod", iostodroid_compat_class_getInstanceMethod) \
    X("_class_getClassMethod", iostodroid_compat_class_getClassMethod) \
    X("_class_addMethod", iostodroid_compat_class_addMethod) \
    X("_class_replaceMethod", iostodroid_compat_class_replaceMethod) \
    X("_class_createInstance", iostodroid_compat_class_createInstance) \
    X("_object_getClass", iostodroid_compat_object_getClass) \
    X("_object_getClassName", iostodroid_compat_object_getClassName) \
    X("_OBJC_CLASS_$_MPMoviePlayerController", iostodroid_compat_OBJC_CLASS___MPMoviePlayerController) \
    X("_OBJC_CLASS_$_NSDate", iostodroid_compat_OBJC_CLASS___NSDate) \
    X("_OBJC_CLASS_$_NSLocale", iostodroid_compat_OBJC_CLASS___NSLocale) \
    X("_OBJC_CLASS_$_NSNotificationCenter", iostodroid_compat_OBJC_CLASS___NSNotificationCenter) \
    X("_OBJC_CLASS_$_NSUserDefaults", iostodroid_compat_OBJC_CLASS___NSUserDefaults) \
    X("_OBJC_CLASS_$_UIColor", iostodroid_compat_OBJC_CLASS___UIColor) \
    X("_OBJC_CLASS_$_UIDevice", iostodroid_compat_OBJC_CLASS___UIDevice) \
    X("_OBJC_CLASS_$_UIImage", iostodroid_compat_OBJC_CLASS___UIImage) \
    X("_OBJC_CLASS_$_UIViewController", iostodroid_compat_OBJC_CLASS___UIViewController) \
    X("_OBJC_CLASS_$_AVAudioPlayer", iostodroid_compat_OBJC_CLASS___AVAudioPlayer) \
    X("_OBJC_CLASS_$_AVAudioSession", iostodroid_compat_OBJC_CLASS___AVAudioSession) \
    X("_OBJC_CLASS_$_NSArray", iostodroid_compat_OBJC_CLASS___NSArray) \
    X("_OBJC_CLASS_$_NSMutableArray", iostodroid_compat_OBJC_CLASS___NSMutableArray) \
    X("_OBJC_CLASS_$_NSMutableDictionary", iostodroid_compat_OBJC_CLASS___NSMutableDictionary) \
    X("_OBJC_CLASS_$_NSMutableString", iostodroid_compat_OBJC_CLASS___NSMutableString) \
    X("_OBJC_CLASS_$_NSData", iostodroid_compat_OBJC_CLASS___NSData) \
    X("_OBJC_CLASS_$_NSMutableData", iostodroid_compat_OBJC_CLASS___NSMutableData) \
    X("_OBJC_CLASS_$_NSSet", iostodroid_compat_OBJC_CLASS___NSSet) \
    X("_OBJC_CLASS_$_NSMutableSet", iostodroid_compat_OBJC_CLASS___NSMutableSet) \
    X("_OBJC_CLASS_$_NSFileManager", iostodroid_compat_OBJC_CLASS___NSFileManager) \
    X("_OBJC_CLASS_$_NSTimer", iostodroid_compat_OBJC_CLASS___NSTimer) \
    X("_OBJC_CLASS_$_NSRunLoop", iostodroid_compat_OBJC_CLASS___NSRunLoop) \
    X("_OBJC_CLASS_$_NSProcessInfo", iostodroid_compat_OBJC_CLASS___NSProcessInfo) \
    X("_OBJC_CLASS_$_NSValue", iostodroid_compat_OBJC_CLASS___NSValue) \
    X("_OBJC_CLASS_$_NSError", iostodroid_compat_OBJC_CLASS___NSError) \
    X("_OBJC_CLASS_$_UIImageView", iostodroid_compat_OBJC_CLASS___UIImageView) \
    X("_OBJC_CLASS_$_UILabel", iostodroid_compat_OBJC_CLASS___UILabel) \
    X("_OBJC_CLASS_$_UIButton", iostodroid_compat_OBJC_CLASS___UIButton) \
    X("_OBJC_CLASS_$_UIScrollView", iostodroid_compat_OBJC_CLASS___UIScrollView) \
    X("_OBJC_CLASS_$_UIAlertView", iostodroid_compat_OBJC_CLASS___UIAlertView) \
    X("_OBJC_CLASS_$_UIActivityIndicatorView", iostodroid_compat_OBJC_CLASS___UIActivityIndicatorView) \
    X("_OBJC_CLASS_$_UIWebView", iostodroid_compat_OBJC_CLASS___UIWebView) \
    X("_OBJC_CLASS_$_UIFont", iostodroid_compat_OBJC_CLASS___UIFont) \
    X("_OBJC_CLASS_$_UITouch", iostodroid_compat_OBJC_CLASS___UITouch) \
    X("_OBJC_CLASS_$_UIEvent", iostodroid_compat_OBJC_CLASS___UIEvent) \
    X("_OBJC_CLASS_$_CALayer", iostodroid_compat_OBJC_CLASS___CALayer) \
    X("_OBJC_CLASS_$_CATransaction", iostodroid_compat_OBJC_CLASS___CATransaction) \
    X("_OBJC_CLASS_$_CABasicAnimation", iostodroid_compat_OBJC_CLASS___CABasicAnimation) \
    X("_OBJC_CLASS_$_SKPaymentQueue", iostodroid_compat_OBJC_CLASS___SKPaymentQueue) \
    X("_OBJC_CLASS_$_SKProductsRequest", iostodroid_compat_OBJC_CLASS___SKProductsRequest) \
    X("_OBJC_CLASS_$_GKLocalPlayer", iostodroid_compat_OBJC_CLASS___GKLocalPlayer) \
    X("_OBJC_CLASS_$_CMMotionManager", iostodroid_compat_OBJC_CLASS___CMMotionManager) \
    X("_OBJC_CLASS_$_GCController", iostodroid_compat_OBJC_CLASS___GCController) \
    X("_OBJC_METACLASS_$_UIViewController", iostodroid_compat_OBJC_METACLASS___UIViewController) \
    X("_OBJC_METACLASS_$_UIApplication", iostodroid_compat_OBJC_METACLASS___UIApplication) \
    X("_UIGraphicsPushContext", iostodroid_compat_UIGraphicsPushContext) \
    X("_UIGraphicsPopContext", iostodroid_compat_UIGraphicsPopContext) \
    X("_UIGraphicsGetCurrentContext", iostodroid_compat_UIGraphicsGetCurrentContext) \
    X("_UIGraphicsBeginImageContext", iostodroid_compat_UIGraphicsBeginImageContext) \
    X("_UIGraphicsBeginImageContextWithOptions", iostodroid_compat_UIGraphicsBeginImageContextWithOptions) \
    X("_UIGraphicsGetImageFromCurrentImageContext", iostodroid_compat_UIGraphicsGetImageFromCurrentImageContext) \
    X("_UIGraphicsEndImageContext", iostodroid_compat_UIGraphicsEndImageContext) \
    X("_UIImagePNGRepresentation", iostodroid_compat_UIImagePNGRepresentation) \
    X("_UIImageJPEGRepresentation", iostodroid_compat_UIImageJPEGRepresentation) \
    X("_UIImageWriteToSavedPhotosAlbum", iostodroid_compat_UIImageWriteToSavedPhotosAlbum) \
    X("_NSTemporaryDirectory", iostodroid_compat_NSTemporaryDirectory) \
    X("_NSHomeDirectory", iostodroid_compat_NSHomeDirectory) \
    X("_NSLog", iostodroid_compat_NSLog) \
    X("_NSStringFromClass", iostodroid_compat_NSStringFromClass) \
    X("_NSClassFromString", iostodroid_compat_NSClassFromString) \
    X("_NSStringFromSelector", iostodroid_compat_NSStringFromSelector) \
    X("_NSSelectorFromString", iostodroid_compat_NSSelectorFromString) \
    X("_NSPageSize", iostodroid_compat_NSPageSize) \
    X("_dispatch_async", iostodroid_compat_dispatch_async) \
    X("_dispatch_sync", iostodroid_compat_dispatch_sync) \
    X("_dispatch_after", iostodroid_compat_dispatch_after) \
    X("_dispatch_once", iostodroid_compat_dispatch_once) \
    X("_dispatch_async_f", iostodroid_compat_dispatch_async_f) \
    X("_dispatch_sync_f", iostodroid_compat_dispatch_sync_f) \
    X("_dispatch_once_f", iostodroid_compat_dispatch_once_f) \
    X("_dispatch_get_main_queue", iostodroid_compat_dispatch_get_main_queue) \
    X("_dispatch_get_global_queue", iostodroid_compat_dispatch_get_global_queue) \
    X("_dispatch_queue_create", iostodroid_compat_dispatch_queue_create) \
    X("_dispatch_release", iostodroid_compat_dispatch_release) \
    X("_dispatch_retain", iostodroid_compat_dispatch_retain) \
    X("_dispatch_time", iostodroid_compat_dispatch_time) \
    X("_dispatch_semaphore_create", iostodroid_compat_dispatch_semaphore_create) \
    X("_dispatch_semaphore_wait", iostodroid_compat_dispatch_semaphore_wait) \
    X("_dispatch_semaphore_signal", iostodroid_compat_dispatch_semaphore_signal) \
    X("_dispatch_group_create", iostodroid_compat_dispatch_group_create) \
    X("_dispatch_group_async", iostodroid_compat_dispatch_group_async) \
    X("_dispatch_group_enter", iostodroid_compat_dispatch_group_enter) \
    X("_dispatch_group_leave", iostodroid_compat_dispatch_group_leave) \
    X("_dispatch_group_wait", iostodroid_compat_dispatch_group_wait) \
    X("_dispatch_group_notify", iostodroid_compat_dispatch_group_notify) \
    X("__dispatch_main_q", iostodroid_compat__dispatch_main_q) \
    X("_SCNetworkReachabilityCreateWithAddress", iostodroid_compat_SCNetworkReachabilityCreateWithAddress) \
    X("_SCNetworkReachabilityCreateWithName", iostodroid_compat_SCNetworkReachabilityCreateWithName) \
    X("_SCNetworkReachabilityGetFlags", iostodroid_compat_SCNetworkReachabilityGetFlags) \
    X("_SCNetworkReachabilitySetCallback", iostodroid_compat_SCNetworkReachabilitySetCallback) \
    X("_SCNetworkReachabilityScheduleWithRunLoop", iostodroid_compat_SCNetworkReachabilityScheduleWithRunLoop) \
    X("_SCNetworkReachabilityUnscheduleFromRunLoop", iostodroid_compat_SCNetworkReachabilityUnscheduleFromRunLoop) \
    X("_SCNetworkReachabilitySetDispatchQueue", iostodroid_compat_SCNetworkReachabilitySetDispatchQueue) \
    X("_SecRandomCopyBytes", iostodroid_compat_SecRandomCopyBytes) \
    X("_SecItemCopyMatching", iostodroid_compat_SecItemCopyMatching) \
    X("_SecItemAdd", iostodroid_compat_SecItemAdd) \
    X("_SecItemUpdate", iostodroid_compat_SecItemUpdate) \
    X("_SecItemDelete", iostodroid_compat_SecItemDelete) \
    X("_CC_MD5", iostodroid_compat_CC_MD5) \
    X("_CC_SHA1", iostodroid_compat_CC_SHA1) \
    X("_CC_SHA256", iostodroid_compat_CC_SHA256) \
    X("__Unwind_DeleteException", iostodroid_compat__Unwind_DeleteException) \
    X("__Unwind_GetIP", iostodroid_compat__Unwind_GetIP) \
    X("__Unwind_SetIP", iostodroid_compat__Unwind_SetIP) \
    X("__Unwind_GetGR", iostodroid_compat__Unwind_GetGR) \
    X("__Unwind_SetGR", iostodroid_compat__Unwind_SetGR) \
    X("__Unwind_GetLanguageSpecificData", iostodroid_compat__Unwind_GetLanguageSpecificData) \
    X("__Unwind_GetRegionStart", iostodroid_compat__Unwind_GetRegionStart) \
    X("___gxx_personality_v0", iostodroid_compat___gxx_personality_v0) \
    X("___gcc_personality_v0", iostodroid_compat___gcc_personality_v0) \
    X("___udivdi3", iostodroid_compat___udivdi3) \
    X("___umoddi3", iostodroid_compat___umoddi3) \
    X("___muldi3", iostodroid_compat___muldi3) \
    X("___fixsfdi", iostodroid_compat___fixsfdi) \
    X("___fixunsdfdi", iostodroid_compat___fixunsdfdi) \
    X("___fixunssfdi", iostodroid_compat___fixunssfdi) \
    X("___floatundidf", iostodroid_compat___floatundidf) \
    X("___floatundisf", iostodroid_compat___floatundisf) \
    X("___ashldi3", iostodroid_compat___ashldi3) \
    X("___ashrdi3", iostodroid_compat___ashrdi3) \
    X("___lshrdi3", iostodroid_compat___lshrdi3) \
    X("___cmpdi2", iostodroid_compat___cmpdi2) \
    X("___ucmpdi2", iostodroid_compat___ucmpdi2) \
    X("___clear_cache", iostodroid_compat___clear_cache) \
    X("__Znaj", iostodroid_compat__Znaj) \
    X("__Znwj", iostodroid_compat__Znwj) \
    X("___cxa_free_exception", iostodroid_compat___cxa_free_exception) \
    X("___cxa_rethrow", iostodroid_compat___cxa_rethrow) \
    X("___cxa_guard_acquire", iostodroid_compat___cxa_guard_acquire) \
    X("___cxa_guard_release", iostodroid_compat___cxa_guard_release) \
    X("___cxa_guard_abort", iostodroid_compat___cxa_guard_abort) \
    X("___cxa_demangle", iostodroid_compat___cxa_demangle) \
    X("___dynamic_cast", iostodroid_compat___dynamic_cast)

#ifdef __cplusplus
}  // extern "C"
#endif
