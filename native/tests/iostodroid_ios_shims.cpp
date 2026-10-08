// Host tests for the bounded C/POSIX/CoreFoundation compatibility shims.
//
// Everything exercised here is a real implementation body, not a resolution
// stub. The test is also run under ASan/UBSan, so it must stay leak-free:
// every CoreFoundation object created below is released.

#include "iostodroid_ios_shims.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (!(expression)) throw std::runtime_error("CHECK failed: " #expression);      \
    } while (false)

namespace {

int compareInts(const void *left, const void *right) {
    const int a = *static_cast<const int *>(left);
    const int b = *static_cast<const int *>(right);
    return (a > b) - (a < b);
}

int callVsnprintf(char *buffer, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = iostodroid_compat_vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}

void testLibc() {
    void *memory = iostodroid_compat_malloc(64);
    CHECK(memory != nullptr);
    iostodroid_compat_memset(memory, 0xAB, 64);
    CHECK(iostodroid_compat_memcmp(memory, std::vector<unsigned char>(64, 0xAB).data(), 64) == 0);
    void *zeroed = iostodroid_compat_calloc(8, 4);
    CHECK(zeroed != nullptr);
    CHECK(static_cast<unsigned char *>(zeroed)[31] == 0);
    void *grown = iostodroid_compat_realloc(memory, 4096);
    CHECK(grown != nullptr);
    iostodroid_compat_free(grown);
    iostodroid_compat_free(zeroed);

    char copiedString[16] = {};
    CHECK(iostodroid_compat_strcpy(copiedString, "copy") == copiedString);
    CHECK(std::string(copiedString) == "copy");
    char boundedString[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    CHECK(iostodroid_compat_strncpy(boundedString, "ok", 5) == boundedString);
    CHECK(boundedString[0] == 'o' && boundedString[1] == 'k' && boundedString[2] == '\0' && boundedString[4] == '\0');

    unsigned char copiedBytes[4] = {};
    const unsigned char sourceBytes[4] = {9, 8, 7, 6};
    CHECK(iostodroid_compat_memcpy(copiedBytes, sourceBytes, sizeof(sourceBytes)) == copiedBytes);
    CHECK(copiedBytes[0] == 9 && copiedBytes[3] == 6);

    CHECK(iostodroid_compat_strlen("iostodroid") == 5);
    CHECK(iostodroid_compat_strcmp("abc", "abc") == 0);
    CHECK(iostodroid_compat_strcmp("abc", "abd") < 0);
    CHECK(iostodroid_compat_strncmp("abcd", "abzz", 2) == 0);
    char *copy = iostodroid_compat_strdup("duplicated");
    CHECK(copy != nullptr && std::string(copy) == "duplicated");
    iostodroid_compat_free(copy);
    CHECK(iostodroid_compat_strchr("abcdef", 'd') != nullptr);
    CHECK(*iostodroid_compat_strchr("abcdef", 'd') == 'd');
    CHECK(iostodroid_compat_strrchr("a.b.c", '.') != nullptr);
    CHECK(std::string(iostodroid_compat_strstr("haystack", "stack")) == "stack");
    CHECK(iostodroid_compat_strtol("42abc", nullptr, 10) == 42);
    CHECK(std::abs(iostodroid_compat_strtod("2.5", nullptr) - 2.5) < 1e-9);
    CHECK(iostodroid_compat_atoi("-17") == -17);
    CHECK(std::abs(iostodroid_compat_atof("0.5") - 0.5) < 1e-9);
    CHECK(iostodroid_compat_strerror(0) != nullptr);

    char small[4] = {};
    CHECK(iostodroid_compat_strlcpy(small, "abcdef", sizeof(small)) == 6);
    CHECK(std::string(small) == "abc");
    CHECK(iostodroid_compat_strlcpy(small, "z", sizeof(small)) == 1);
    CHECK(std::string(small) == "z");
    CHECK(iostodroid_compat_strlcat(small, "123456", sizeof(small)) == 7);
    CHECK(std::string(small) == "z12");

    char formatted[32] = {};
    CHECK(iostodroid_compat_snprintf(formatted, sizeof(formatted), "%d-%s", 7, "ok") == 4);
    CHECK(std::string(formatted) == "7-ok");
    CHECK(callVsnprintf(formatted, sizeof(formatted), "%s-%d", "ok", 7) == 4);
    CHECK(std::string(formatted) == "ok-7");

    CHECK(iostodroid_compat_printf("shim-%d\n", 7) == 7);
    CHECK(iostodroid_compat_puts("shim-puts") >= 0);
    iostodroid_compat_srand(1234);
    const int randomValue = iostodroid_compat_rand();
    iostodroid_compat_srand(1234);
    CHECK(iostodroid_compat_rand() == randomValue);

    unsigned char buffer[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char moved[8] = {};
    iostodroid_compat_memmove(moved, buffer, sizeof(buffer));
    CHECK(moved[7] == 8);
    CHECK(iostodroid_compat_memchr(buffer, 5, sizeof(buffer)) == buffer + 4);

    CHECK(std::abs(iostodroid_compat_sqrt(81.0) - 9.0) < 1e-9);
    CHECK(std::abs(iostodroid_compat_fabs(-3.0) - 3.0) < 1e-9);
    CHECK(std::abs(iostodroid_compat_floor(2.7) - 2.0) < 1e-9);
    CHECK(std::abs(iostodroid_compat_ceil(2.1) - 3.0) < 1e-9);
    CHECK(std::abs(iostodroid_compat_pow(2.0, 10.0) - 1024.0) < 1e-6);
    CHECK(std::abs(iostodroid_compat_sin(0.0)) < 1e-12);
    CHECK(std::abs(iostodroid_compat_cos(0.0) - 1.0) < 1e-12);
    CHECK(std::abs(iostodroid_compat_tan(0.0)) < 1e-12);
    CHECK(std::abs(iostodroid_compat_atan2(0.0, 1.0)) < 1e-12);
    CHECK(std::abs(iostodroid_compat_fmod(7.0, 3.0) - 1.0) < 1e-9);
    CHECK(iostodroid_compat_abs(-9) == 9);
    CHECK(iostodroid_compat_labs(-900000L) == 900000L);

    int values[5] = {5, 1, 4, 2, 3};
    iostodroid_compat_qsort(values, 5, sizeof(int), compareInts);
    CHECK(values[0] == 1 && values[4] == 5);
    int key = 4;
    CHECK(iostodroid_compat_bsearch(&key, values, 5, sizeof(int), compareInts) != nullptr);

    CHECK(iostodroid_compat_getpid() > 0);
    CHECK(iostodroid_compat_setenv("IOSTODROID_SHIM_TEST", "1", 1) == 0);
    const char *value = iostodroid_compat_getenv("IOSTODROID_SHIM_TEST");
    CHECK(value != nullptr && std::string(value) == "1");
    CHECK(iostodroid_compat_unsetenv("IOSTODROID_SHIM_TEST") == 0);
    CHECK(iostodroid_compat_getenv("IOSTODROID_SHIM_TEST") == nullptr);
}

void testStdio() {
    const char *path = std::getenv("TMPDIR") ? (std::string(std::getenv("TMPDIR")) + "/iostodroid_shim_test.txt").c_str()
                                             : "/tmp/iostodroid_shim_test.txt";
    FILE *stream = iostodroid_compat_fopen(path, "w+");
    CHECK(stream != nullptr);
    CHECK(iostodroid_compat_fwrite("iostodroid", 1, 5, stream) == 5);
    CHECK(iostodroid_compat_fputs("-", stream) >= 0);
    CHECK(iostodroid_compat_fprintf(stream, "%d", 42) == 2);
    CHECK(iostodroid_compat_fflush(stream) == 0);
    CHECK(iostodroid_compat_fseek(stream, 0, SEEK_SET) == 0);
    char read[32] = {};
    CHECK(iostodroid_compat_fread(read, 1, sizeof(read) - 1, stream) == 8);
    CHECK(std::string(read) == "iostodroid-42");
    CHECK(iostodroid_compat_feof(stream) != 0);
    CHECK(iostodroid_compat_ftell(stream) == 8);
    CHECK(iostodroid_compat_fseek(stream, 0, SEEK_SET) == 0);
    char line[16] = {};
    CHECK(iostodroid_compat_fgets(line, sizeof(line), stream) == line);
    CHECK(std::string(line) == "iostodroid-42");
    CHECK(iostodroid_compat_fclose(stream) == 0);
    CHECK(iostodroid_compat_remove(path) == 0);
}

void testTime() {
    const time_t now = iostodroid_compat_time(nullptr);
    CHECK(now > 1500000000);

    iostodroid_darwin_timeval wall{};
    CHECK(iostodroid_compat_gettimeofday(&wall, nullptr) == 0);
    CHECK(wall.tv_sec > 1500000000);
    CHECK(wall.tv_usec >= 0 && wall.tv_usec < 1000000);

    struct timespec monotonic{};
    CHECK(iostodroid_compat_clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0);

    struct timespec request{};
    request.tv_nsec = 2 * 1000 * 1000;  // 2 ms
    const struct timespec before = monotonic;
    CHECK(iostodroid_compat_nanosleep(&request, nullptr) == 0);
    struct timespec after{};
    CHECK(iostodroid_compat_clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    const double elapsed = static_cast<double>(after.tv_sec - before.tv_sec) +
                           static_cast<double>(after.tv_nsec - before.tv_nsec) / 1e9;
    CHECK(elapsed >= 0.001);

    time_t clockValue = 0;  // 1970-01-01T00:00:00Z
    struct tm broken{};
    CHECK(iostodroid_compat_gmtime_r(&clockValue, &broken) != nullptr);
    CHECK(broken.tm_year == 70 && broken.tm_mon == 0 && broken.tm_mday == 1);
    struct tm localBroken{};
    CHECK(iostodroid_compat_localtime_r(&clockValue, &localBroken) != nullptr);
    struct tm rebuild{};
    rebuild.tm_year = 100;  // 2000
    rebuild.tm_mon = 0;
    rebuild.tm_mday = 1;
    rebuild.tm_hour = 12;
    CHECK(iostodroid_compat_mktime(&rebuild) > 0);
}

void testPthread() {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    CHECK(iostodroid_compat_pthread_mutex_init(&mutex, nullptr) == 0);
    CHECK(iostodroid_compat_pthread_cond_init(&condition, nullptr) == 0);

    std::atomic<bool> ready{false};
    std::atomic<bool> seen{false};
    std::thread worker([&] {
        iostodroid_compat_pthread_mutex_lock(&mutex);
        while (!ready.load()) iostodroid_compat_pthread_cond_wait(&condition, &mutex);
        iostodroid_compat_pthread_mutex_unlock(&mutex);
        seen.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(iostodroid_compat_pthread_mutex_lock(&mutex) == 0);
    ready.store(true);
    CHECK(iostodroid_compat_pthread_cond_broadcast(&condition) == 0);
    CHECK(iostodroid_compat_pthread_mutex_unlock(&mutex) == 0);
    CHECK(iostodroid_compat_pthread_cond_signal(&condition) == 0);
    worker.join();
    CHECK(seen.load());

    CHECK(iostodroid_compat_pthread_self() != 0);
    CHECK(iostodroid_compat_pthread_cond_destroy(&condition) == 0);
    CHECK(iostodroid_compat_pthread_mutex_destroy(&mutex) == 0);
}

struct RunLoopCall {
    std::atomic<unsigned> count{0};
    iostodroid_CFRunLoopRef loop = nullptr;
    bool stop = false;
};

void incrementRunLoopCall(void *opaque) {
    auto *call = static_cast<RunLoopCall *>(opaque);
    call->count.fetch_add(1, std::memory_order_relaxed);
    if (call->stop) iostodroid_compat_CFRunLoopStop(call->loop);
}

void testCoreFoundationRunLoop() {
    const iostodroid_CFRunLoopRef current = iostodroid_compat_CFRunLoopGetCurrent();
    CHECK(current != nullptr);
    CHECK(current == iostodroid_compat_CFRunLoopGetCurrent());
    CHECK(current == iostodroid_compat_CFRunLoopGetMain());
    CHECK(iostodroid_compat_CFRunLoopGetMain() == iostodroid_compat_CFRunLoopGetMain());
    CHECK(iostodroid_compat_CFRetain(current) == current);
    iostodroid_compat_CFRelease(current); // current/main loops have process/thread lifetime

    const iostodroid_CFAllocatorRef allocator = iostodroid_compat_CFAllocatorGetDefault();
    const iostodroid_CFStringRef defaultMode =
        iostodroid_compat_CFStringCreateWithCString(allocator, "default", IOSTODROID_KCFSTRINGENCODINGUTF8);
    const iostodroid_CFStringRef otherMode =
        iostodroid_compat_CFStringCreateWithCString(allocator, "other", IOSTODROID_KCFSTRINGENCODINGUTF8);
    CHECK(defaultMode != nullptr && otherMode != nullptr);

    RunLoopCall call;
    call.loop = current;
    CHECK(iostodroid_compat_CFRunLoopPerform(current, defaultMode, incrementRunLoopCall, &call) == 1);
    CHECK(iostodroid_compat_CFRunLoopRunInMode(defaultMode, 1.0, 1) == IOSTODROID_KCFRUNLOOPRUNHANDLEDSOURCE);
    CHECK(call.count.load(std::memory_order_relaxed) == 1);

    CHECK(iostodroid_compat_CFRunLoopPerform(current, otherMode, incrementRunLoopCall, &call) == 1);
    CHECK(iostodroid_compat_CFRunLoopRunInMode(defaultMode, 0.005, 0) == IOSTODROID_KCFRUNLOOPRUNTIMEDOUT);
    CHECK(call.count.load(std::memory_order_relaxed) == 1);
    CHECK(iostodroid_compat_CFRunLoopRunInMode(otherMode, 1.0, 1) == IOSTODROID_KCFRUNLOOPRUNHANDLEDSOURCE);
    CHECK(call.count.load(std::memory_order_relaxed) == 2);

    std::atomic<iostodroid_Boolean> queued{0};
    std::thread producer([&] {
        queued.store(iostodroid_compat_CFRunLoopPerform(current, defaultMode, incrementRunLoopCall, &call),
                     std::memory_order_release);
    });
    CHECK(iostodroid_compat_CFRunLoopRunInMode(defaultMode, 1.0, 1) == IOSTODROID_KCFRUNLOOPRUNHANDLEDSOURCE);
    producer.join();
    CHECK(queued.load(std::memory_order_acquire) == 1);
    CHECK(call.count.load(std::memory_order_relaxed) == 3);

    call.stop = true;
    CHECK(iostodroid_compat_CFRunLoopPerform(current, defaultMode, incrementRunLoopCall, &call) == 1);
    CHECK(iostodroid_compat_CFRunLoopRunInMode(defaultMode, 1.0, 0) == IOSTODROID_KCFRUNLOOPRUNSTOPPED);
    CHECK(call.count.load(std::memory_order_relaxed) == 4);
    iostodroid_compat_CFRunLoopWakeUp(current);
    std::thread stopper([current] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        iostodroid_compat_CFRunLoopStop(current);
    });
    iostodroid_compat_CFRunLoopRun();
    stopper.join();

    iostodroid_compat_CFRelease(defaultMode);
    iostodroid_compat_CFRelease(otherMode);
}

void testCoreFoundationObjects() {
    iostodroid_CFAllocatorRef allocator = iostodroid_compat_CFAllocatorGetDefault();
    CHECK(allocator != nullptr);
    // The process-lifetime allocator ignores retain/release.
    iostodroid_compat_CFRetain(allocator);
    iostodroid_compat_CFRelease(allocator);

    CHECK(iostodroid_compat_CFStringGetSystemEncoding() == IOSTODROID_KCFSTRINGENCODINGUTF8);

    iostodroid_CFStringRef hello = iostodroid_compat_CFStringCreateWithCString(allocator, "hello", IOSTODROID_KCFSTRINGENCODINGUTF8);
    CHECK(hello != nullptr);
    CHECK(iostodroid_compat_CFStringGetLength(hello) == 5);
    CHECK(iostodroid_compat_CFStringGetMaximumSizeForEncoding(5, IOSTODROID_KCFSTRINGENCODINGUTF8) == 16);
    const char *raw = iostodroid_compat_CFStringGetCStringPtr(hello, IOSTODROID_KCFSTRINGENCODINGUTF8);
    CHECK(raw != nullptr && std::string(raw) == "hello");
    char small[3] = {};
    CHECK(iostodroid_compat_CFStringGetCString(hello, small, sizeof(small), IOSTODROID_KCFSTRINGENCODINGUTF8) == 0);
    char roomy[16] = {};
    CHECK(iostodroid_compat_CFStringGetCString(hello, roomy, sizeof(roomy), IOSTODROID_KCFSTRINGENCODINGUTF8) == 1);
    CHECK(std::string(roomy) == "hello");
    // A non-UTF-8 encoding is refused instead of silently mis-decoding.
    CHECK(iostodroid_compat_CFStringGetCString(hello, roomy, sizeof(roomy), 0x0600) == 0);
    CHECK(iostodroid_compat_CFStringCreateWithCString(allocator, "x", 0x0600) == nullptr);

    iostodroid_CFStringRef other = iostodroid_compat_CFStringCreateWithCString(allocator, "hellp", IOSTODROID_KCFSTRINGENCODINGUTF8);
    CHECK(iostodroid_compat_CFStringCompare(hello, other, 0) == IOSTODROID_KCFCOMPARELESSTHAN);
    CHECK(iostodroid_compat_CFStringCompare(other, hello, 0) == IOSTODROID_KCFCOMPAREGREATERTHAN);
    CHECK(iostodroid_compat_CFStringCompare(hello, hello, 0) == IOSTODROID_KCFCOMPAREEQUALTO);

    CHECK(iostodroid_compat_CFGetRetainCount(hello) == 1);
    iostodroid_compat_CFRetain(hello);
    CHECK(iostodroid_compat_CFGetRetainCount(hello) == 2);
    iostodroid_compat_CFRelease(hello);
    CHECK(iostodroid_compat_CFGetRetainCount(hello) == 1);

    iostodroid_CFDataRef data = iostodroid_compat_CFDataCreate(allocator, reinterpret_cast<const uint8_t *>("abc"), 3);
    CHECK(data != nullptr);
    CHECK(iostodroid_compat_CFDataGetLength(data) == 3);
    CHECK(std::memcmp(iostodroid_compat_CFDataGetBytePtr(data), "abc", 3) == 0);
    iostodroid_CFDataRef empty = iostodroid_compat_CFDataCreate(allocator, nullptr, 0);
    CHECK(iostodroid_compat_CFDataGetLength(empty) == 0);
    CHECK(iostodroid_compat_CFDataGetBytePtr(empty) == nullptr);

    iostodroid_CFMutableArrayRef array = iostodroid_compat_CFArrayCreateMutable(allocator, 2, nullptr);
    CHECK(array != nullptr);
    CHECK(iostodroid_compat_CFArrayGetCount(array) == 0);
    iostodroid_compat_CFArrayAppendValue(array, hello);
    iostodroid_compat_CFArrayAppendValue(array, data);
    CHECK(iostodroid_compat_CFArrayGetCount(array) == 2);
    // Appending retains, so the element survives the caller's own release.
    CHECK(iostodroid_compat_CFGetRetainCount(hello) == 2);
    CHECK(iostodroid_compat_CFArrayGetValueAtIndex(array, 0) == hello);
    CHECK(iostodroid_compat_CFArrayGetValueAtIndex(array, 1) == data);
    CHECK(iostodroid_compat_CFArrayGetValueAtIndex(array, 2) == nullptr);
    CHECK(iostodroid_compat_CFArrayGetValueAtIndex(array, -1) == nullptr);

    iostodroid_CFMutableDictionaryRef dictionary =
        iostodroid_compat_CFDictionaryCreateMutable(allocator, 0, nullptr, nullptr);
    CHECK(dictionary != nullptr);
    CHECK(iostodroid_compat_CFDictionaryGetCount(dictionary) == 0);
    iostodroid_CFStringRef key = iostodroid_compat_CFStringCreateWithCString(allocator, "greeting", IOSTODROID_KCFSTRINGENCODINGUTF8);
    iostodroid_compat_CFDictionarySetValue(dictionary, key, hello);
    CHECK(iostodroid_compat_CFDictionaryGetCount(dictionary) == 1);
    CHECK(iostodroid_compat_CFDictionaryGetValue(dictionary, key) == hello);
    // Replacing a key releases the previous value and keeps the count at one.
    iostodroid_compat_CFDictionarySetValue(dictionary, key, data);
    CHECK(iostodroid_compat_CFDictionaryGetCount(dictionary) == 1);
    CHECK(iostodroid_compat_CFDictionaryGetValue(dictionary, key) == data);
    // The dictionary released the replaced value; the array still holds `hello`.
    CHECK(iostodroid_compat_CFGetRetainCount(hello) == 2);
    CHECK(iostodroid_compat_CFDictionaryGetValue(dictionary, other) == nullptr);

    iostodroid_CFNumberRef numberObject = nullptr;
    int32_t fortyTwo = 42;
    numberObject = iostodroid_compat_CFNumberCreate(allocator, IOSTODROID_KCFNUMBERSINT32TYPE, &fortyTwo);
    CHECK(numberObject != nullptr);
    double asDouble = 0.0;
    CHECK(iostodroid_compat_CFNumberGetValue(numberObject, IOSTODROID_KCFNUMBERDOUBLETYPE, &asDouble) == 1);
    CHECK(std::abs(asDouble - 42.0) < 1e-9);
    int64_t asInt64 = 0;
    CHECK(iostodroid_compat_CFNumberGetValue(numberObject, IOSTODROID_KCFNUMBERSINT64TYPE, &asInt64) == 1);
    CHECK(asInt64 == 42);
    float asFloat = 0.0f;
    CHECK(iostodroid_compat_CFNumberGetValue(numberObject, IOSTODROID_KCFNUMBERFLOAT32TYPE, &asFloat) == 1);
    CHECK(std::abs(asFloat - 42.0f) < 1e-3f);
    CHECK(iostodroid_compat_CFNumberGetValue(numberObject, 999u, &asDouble) == 0);
    CHECK(iostodroid_compat_CFNumberCreate(allocator, 999u, &fortyTwo) == nullptr);

    iostodroid_CFDateRef epoch = iostodroid_compat_CFDateCreate(allocator, 0.0);
    iostodroid_CFDateRef later = iostodroid_compat_CFDateCreate(allocator, 3600.0);
    CHECK(iostodroid_compat_CFDateGetAbsoluteTime(epoch) == 0.0);
    CHECK(std::abs(iostodroid_compat_CFDateGetTimeIntervalSinceDate(later, epoch) - 3600.0) < 1e-9);
    CHECK(std::abs(iostodroid_compat_CFDateGetTimeIntervalSinceDate(epoch, later) + 3600.0) < 1e-9);

    // CFAbsoluteTime 0 is 2001-01-01 00:00:00 UTC.
    const iostodroid_CFGregorianDate gregorian = iostodroid_compat_CFAbsoluteTimeGetGregorianDate(0.0, nullptr);
    CHECK(gregorian.year == 2001);
    CHECK(gregorian.month == 1);
    CHECK(gregorian.day == 1);
    CHECK(gregorian.hour == 0);
    CHECK(gregorian.minute == 0);
    CHECK(std::abs(gregorian.second) < 1e-6);

    // Everything created above is released exactly once; containers release
    // their elements when they die, which is why `hello` drops back to 1 above.
    iostodroid_compat_CFRelease(array);
    iostodroid_compat_CFRelease(dictionary);
    iostodroid_compat_CFRelease(key);
    iostodroid_compat_CFRelease(hello);
    iostodroid_compat_CFRelease(other);
    iostodroid_compat_CFRelease(data);
    iostodroid_compat_CFRelease(empty);
    iostodroid_compat_CFRelease(numberObject);
    iostodroid_compat_CFRelease(epoch);
    iostodroid_compat_CFRelease(later);
}

void testExpandedGameAndFrameworkShims() {
    // CoreFoundation expanded bundle/URL/boolean/equality shims
    CHECK(iostodroid_compat_CFBooleanGetValue(iostodroid_compat_CFBooleanTrue()) == 1);
    CHECK(iostodroid_compat_CFBooleanGetValue(iostodroid_compat_CFBooleanFalse()) == 0);
    iostodroid_CFTypeRef mainBundle = iostodroid_compat_CFBundleGetMainBundle();
    CHECK(mainBundle != nullptr);
    CHECK(iostodroid_compat_CFBundleGetIdentifier(mainBundle) != nullptr);
    iostodroid_CFTypeRef bundleUrl = iostodroid_compat_CFBundleCopyBundleURL(mainBundle);
    CHECK(bundleUrl != nullptr);
    uint8_t urlPath[64] = {};
    CHECK(iostodroid_compat_CFURLGetFileSystemRepresentation(bundleUrl, 1, urlPath, sizeof(urlPath)) == 1);
    CHECK(std::string(reinterpret_cast<char *>(urlPath)) == "/bundle");
    iostodroid_CFStringRef copiedPath = iostodroid_compat_CFURLCopyFileSystemPath(bundleUrl, 0);
    CHECK(copiedPath != nullptr);
    CHECK(iostodroid_compat_CFEqual(bundleUrl, copiedPath) == 1);
    CHECK(iostodroid_compat_CFHash(copiedPath) != 0);
    iostodroid_compat_CFRelease(copiedPath);
    iostodroid_compat_CFRelease(bundleUrl);

    // Compiler-rt 64-bit / 32-bit integer & float helpers and C++ ABI allocation
    CHECK(iostodroid_compat___divdi3(100, -4) == -25);
    CHECK(iostodroid_compat___divdi3(100, 0) == 0);
    CHECK(iostodroid_compat___moddi3(103, 10) == 3);
    CHECK(iostodroid_compat___udivdi3(100u, 4u) == 25u);
    CHECK(iostodroid_compat___umoddi3(103u, 10u) == 3u);
    CHECK(iostodroid_compat___divsi3(-42, 6) == -7);
    CHECK(iostodroid_compat___modsi3(-43, 6) == -1);
    CHECK(iostodroid_compat___udivsi3(42u, 6u) == 7u);
    CHECK(iostodroid_compat___umodsi3(43u, 6u) == 1u);
    CHECK(iostodroid_compat___fixdfdi(1234.75) == 1234);
    CHECK(std::abs(iostodroid_compat___floatdidf(1234) - 1234.0) < 1e-9);
    CHECK(std::abs(iostodroid_compat___floatdisf(1234) - 1234.0f) < 1e-5f);
    void *cppMem = iostodroid_compat__Znwm(64);
    CHECK(cppMem != nullptr);
    iostodroid_compat__ZdlPv(cppMem);
    void *cppArr = iostodroid_compat__Znam(128);
    CHECK(cppArr != nullptr);
    iostodroid_compat__ZdaPv(cppArr);
    CHECK(iostodroid_compat___error() != nullptr);

    // OpenGL ES and OpenAL stateful shims
    unsigned int tex[2] = {0, 0};
    iostodroid_compat_glGenTextures(2, tex);
    CHECK(tex[0] != 0 && tex[1] != 0 && tex[0] != tex[1]);
    iostodroid_compat_glBindTexture(0x0DE1u, tex[0]);
    iostodroid_compat_glFrontFace(0x0900u);
    iostodroid_compat_glDrawArrays(4u, 0, 4);
    iostodroid_compat_glDrawElements(4u, 6, 0x1403u, nullptr);
    CHECK(iostodroid_compat_glCheckFramebufferStatusOES(0x8D40u) == 0x8CD5u);
    CHECK(iostodroid_compat_glCheckFramebufferStatus(0x8D40u) == 0x8CD5u);
    void *alDev = iostodroid_compat_alcOpenDevice(nullptr);
    CHECK(alDev != nullptr);
    void *alCtx = iostodroid_compat_alcCreateContext(alDev, nullptr);
    CHECK(alCtx != nullptr);
    CHECK(iostodroid_compat_alcMakeContextCurrent(alCtx) == 1);
    unsigned int alBuf = 0, alSrc = 0;
    iostodroid_compat_alGenBuffers(1, &alBuf);
    iostodroid_compat_alGenSources(1, &alSrc);
    CHECK(alBuf != 0 && alSrc != 0);

    // Verify every expanded shim function address is linked and non-null
    const void *const kExpandedShimAddresses[] = {
        reinterpret_cast<const void *>(&iostodroid_compat_CFConstantStringClassReference),
        reinterpret_cast<const void *>(&iostodroid_compat_CFAllocatorDefault),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBooleanTrue),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBooleanFalse),
        reinterpret_cast<const void *>(&iostodroid_compat_CFTypeArrayCallBacks),
        reinterpret_cast<const void *>(&iostodroid_compat_CFTypeDictionaryKeyCallBacks),
        reinterpret_cast<const void *>(&iostodroid_compat_CFTypeDictionaryValueCallBacks),
        reinterpret_cast<const void *>(&iostodroid_compat_CFRunLoopDefaultMode),
        reinterpret_cast<const void *>(&iostodroid_compat_CFRunLoopCommonModes),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBundleGetMainBundle),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBundleCopyBundleURL),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBundleCopyResourcesDirectoryURL),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBundleCopyResourceURL),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBundleGetIdentifier),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBundleGetValueForInfoDictionaryKey),
        reinterpret_cast<const void *>(&iostodroid_compat_CFURLCreateWithFileSystemPath),
        reinterpret_cast<const void *>(&iostodroid_compat_CFURLCreateFromFileSystemRepresentation),
        reinterpret_cast<const void *>(&iostodroid_compat_CFURLGetFileSystemRepresentation),
        reinterpret_cast<const void *>(&iostodroid_compat_CFURLCopyFileSystemPath),
        reinterpret_cast<const void *>(&iostodroid_compat_CFStringCreateWithBytes),
        reinterpret_cast<const void *>(&iostodroid_compat_CFStringCreateMutable),
        reinterpret_cast<const void *>(&iostodroid_compat_CFStringAppendCString),
        reinterpret_cast<const void *>(&iostodroid_compat_CFStringHasPrefix),
        reinterpret_cast<const void *>(&iostodroid_compat_CFStringHasSuffix),
        reinterpret_cast<const void *>(&iostodroid_compat_CFStringGetIntValue),
        reinterpret_cast<const void *>(&iostodroid_compat_CFStringGetDoubleValue),
        reinterpret_cast<const void *>(&iostodroid_compat_CFArrayCreate),
        reinterpret_cast<const void *>(&iostodroid_compat_CFArrayRemoveValueAtIndex),
        reinterpret_cast<const void *>(&iostodroid_compat_CFArrayRemoveAllValues),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDictionaryCreate),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDictionaryRemoveValue),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDictionaryRemoveAllValues),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDictionaryContainsKey),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDataCreateMutable),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDataAppendBytes),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDataGetMutableBytePtr),
        reinterpret_cast<const void *>(&iostodroid_compat_CFDataGetBytes),
        reinterpret_cast<const void *>(&iostodroid_compat_CFBooleanGetValue),
        reinterpret_cast<const void *>(&iostodroid_compat_CFEqual),
        reinterpret_cast<const void *>(&iostodroid_compat_CFHash),
        reinterpret_cast<const void *>(&iostodroid_compat_CFGetTypeID),
        reinterpret_cast<const void *>(&iostodroid_compat_CFPreferencesCopyAppValue),
        reinterpret_cast<const void *>(&iostodroid_compat_CFPreferencesSetAppValue),
        reinterpret_cast<const void *>(&iostodroid_compat_CFPreferencesAppSynchronize),
        reinterpret_cast<const void *>(&iostodroid_compat_CFUUIDCreate),
        reinterpret_cast<const void *>(&iostodroid_compat_CFUUIDCreateString),
        reinterpret_cast<const void *>(&iostodroid_compat_CFLocaleCopyCurrent),
        reinterpret_cast<const void *>(&iostodroid_compat_CFLocaleCopyPreferredLanguages),
        reinterpret_cast<const void *>(&iostodroid_compat_CFLocaleGetIdentifier),
        reinterpret_cast<const void *>(&iostodroid_compat_CFTimeZoneCopySystem),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionInitialize),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionSetActive),
        reinterpret_cast<const void *>(&iostodroid_compat_NSSearchPathForDirectoriesInDomains),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___CAEAGLLayer),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___EAGLContext),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSAutoreleasePool),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSBundle),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSDictionary),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSNumber),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSObject),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSString),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSThread),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSURL),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIAccelerometer),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIApplication),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIScreen),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIView),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIWindow),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_METACLASS___NSObject),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_METACLASS___UIView),
        reinterpret_cast<const void *>(&iostodroid_compat_UIApplicationMain),
        reinterpret_cast<const void *>(&iostodroid_compat__DefaultRuneLocale),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_SjLj_Register),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_SjLj_Resume),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_SjLj_Unregister),
        reinterpret_cast<const void *>(&iostodroid_compat__ZSt9terminatev),
        reinterpret_cast<const void *>(&iostodroid_compat__ZTVN10__cxxabiv117__class_type_infoE),
        reinterpret_cast<const void *>(&iostodroid_compat__ZTVN10__cxxabiv119__pointer_type_infoE),
        reinterpret_cast<const void *>(&iostodroid_compat__ZTVN10__cxxabiv120__si_class_type_infoE),
        reinterpret_cast<const void *>(&iostodroid_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE),
        reinterpret_cast<const void *>(&iostodroid_compat__ZdaPv),
        reinterpret_cast<const void *>(&iostodroid_compat__ZdlPv),
        reinterpret_cast<const void *>(&iostodroid_compat__Znam),
        reinterpret_cast<const void *>(&iostodroid_compat__Znwm),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_allocate_exception),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_atexit),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_begin_catch),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_end_catch),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_pure_virtual),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_throw),
        reinterpret_cast<const void *>(&iostodroid_compat___divdi3),
        reinterpret_cast<const void *>(&iostodroid_compat___divsi3),
        reinterpret_cast<const void *>(&iostodroid_compat___error),
        reinterpret_cast<const void *>(&iostodroid_compat___fixdfdi),
        reinterpret_cast<const void *>(&iostodroid_compat___floatdidf),
        reinterpret_cast<const void *>(&iostodroid_compat___floatdisf),
        reinterpret_cast<const void *>(&iostodroid_compat___gxx_personality_sj0),
        reinterpret_cast<const void *>(&iostodroid_compat___maskrune),
        reinterpret_cast<const void *>(&iostodroid_compat___moddi3),
        reinterpret_cast<const void *>(&iostodroid_compat___modsi3),
        reinterpret_cast<const void *>(&iostodroid_compat___stderrp),
        reinterpret_cast<const void *>(&iostodroid_compat___stdinp),
        reinterpret_cast<const void *>(&iostodroid_compat___stdoutp),
        reinterpret_cast<const void *>(&iostodroid_compat___tolower),
        reinterpret_cast<const void *>(&iostodroid_compat___toupper),
        reinterpret_cast<const void *>(&iostodroid_compat___udivsi3),
        reinterpret_cast<const void *>(&iostodroid_compat___umodsi3),
        reinterpret_cast<const void *>(&iostodroid_compat__objc_empty_cache),
        reinterpret_cast<const void *>(&iostodroid_compat__objc_empty_vtable),
        reinterpret_cast<const void *>(&iostodroid_compat_abort),
        reinterpret_cast<const void *>(&iostodroid_compat_acosf),
        reinterpret_cast<const void *>(&iostodroid_compat_alBufferData),
        reinterpret_cast<const void *>(&iostodroid_compat_alDeleteBuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_alDeleteSources),
        reinterpret_cast<const void *>(&iostodroid_compat_alGenBuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_alGenSources),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetSourcef),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetSourcei),
        reinterpret_cast<const void *>(&iostodroid_compat_alSource3f),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourcePlay),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourceQueueBuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourceStop),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourceUnqueueBuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourcef),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourcei),
        reinterpret_cast<const void *>(&iostodroid_compat_alcCloseDevice),
        reinterpret_cast<const void *>(&iostodroid_compat_alcCreateContext),
        reinterpret_cast<const void *>(&iostodroid_compat_alcDestroyContext),
        reinterpret_cast<const void *>(&iostodroid_compat_alcMakeContextCurrent),
        reinterpret_cast<const void *>(&iostodroid_compat_alcOpenDevice),
        reinterpret_cast<const void *>(&iostodroid_compat_asinf),
        reinterpret_cast<const void *>(&iostodroid_compat_atan2f),
        reinterpret_cast<const void *>(&iostodroid_compat_atanf),
        reinterpret_cast<const void *>(&iostodroid_compat_ceilf),
        reinterpret_cast<const void *>(&iostodroid_compat_clearerr),
        reinterpret_cast<const void *>(&iostodroid_compat_clock),
        reinterpret_cast<const void *>(&iostodroid_compat_close),
        reinterpret_cast<const void *>(&iostodroid_compat_cosf),
        reinterpret_cast<const void *>(&iostodroid_compat_coshf),
        reinterpret_cast<const void *>(&iostodroid_compat_difftime),
        reinterpret_cast<const void *>(&iostodroid_compat_exit),
        reinterpret_cast<const void *>(&iostodroid_compat_expf),
        reinterpret_cast<const void *>(&iostodroid_compat_fcntl),
        reinterpret_cast<const void *>(&iostodroid_compat_ferror),
        reinterpret_cast<const void *>(&iostodroid_compat_floorf),
        reinterpret_cast<const void *>(&iostodroid_compat_fputc),
        reinterpret_cast<const void *>(&iostodroid_compat_freopen),
        reinterpret_cast<const void *>(&iostodroid_compat_frexp),
        reinterpret_cast<const void *>(&iostodroid_compat_fscanf),
        reinterpret_cast<const void *>(&iostodroid_compat_getc),
        reinterpret_cast<const void *>(&iostodroid_compat_glActiveTexture),
        reinterpret_cast<const void *>(&iostodroid_compat_glBindBuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_glBindFramebufferOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glBindRenderbufferOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glBindTexture),
        reinterpret_cast<const void *>(&iostodroid_compat_glBlendFunc),
        reinterpret_cast<const void *>(&iostodroid_compat_glBufferData),
        reinterpret_cast<const void *>(&iostodroid_compat_glCheckFramebufferStatusOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glClear),
        reinterpret_cast<const void *>(&iostodroid_compat_glClearColor),
        reinterpret_cast<const void *>(&iostodroid_compat_glClientActiveTexture),
        reinterpret_cast<const void *>(&iostodroid_compat_glColor4f),
        reinterpret_cast<const void *>(&iostodroid_compat_glColorPointer),
        reinterpret_cast<const void *>(&iostodroid_compat_glCompressedTexImage2D),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteBuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteFramebuffersOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteRenderbuffersOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteTextures),
        reinterpret_cast<const void *>(&iostodroid_compat_glDepthFunc),
        reinterpret_cast<const void *>(&iostodroid_compat_glDepthMask),
        reinterpret_cast<const void *>(&iostodroid_compat_glDisable),
        reinterpret_cast<const void *>(&iostodroid_compat_glDisableClientState),
        reinterpret_cast<const void *>(&iostodroid_compat_glDrawArrays),
        reinterpret_cast<const void *>(&iostodroid_compat_glDrawElements),
        reinterpret_cast<const void *>(&iostodroid_compat_glEnable),
        reinterpret_cast<const void *>(&iostodroid_compat_glEnableClientState),
        reinterpret_cast<const void *>(&iostodroid_compat_glFramebufferRenderbufferOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glFramebufferTexture2DOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glFrontFace),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenBuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenFramebuffersOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenRenderbuffersOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenTextures),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetIntegerv),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetRenderbufferParameterivOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glLightfv),
        reinterpret_cast<const void *>(&iostodroid_compat_glLineWidth),
        reinterpret_cast<const void *>(&iostodroid_compat_glLoadMatrixf),
        reinterpret_cast<const void *>(&iostodroid_compat_glMaterialfv),
        reinterpret_cast<const void *>(&iostodroid_compat_glMatrixMode),
        reinterpret_cast<const void *>(&iostodroid_compat_glNormalPointer),
        reinterpret_cast<const void *>(&iostodroid_compat_glPixelStorei),
        reinterpret_cast<const void *>(&iostodroid_compat_glRenderbufferStorageOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glScissor),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexCoordPointer),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexEnvi),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexImage2D),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexParameteri),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexSubImage2D),
        reinterpret_cast<const void *>(&iostodroid_compat_glVertexPointer),
        reinterpret_cast<const void *>(&iostodroid_compat_glViewport),
        reinterpret_cast<const void *>(&iostodroid_compat_gmtime),
        reinterpret_cast<const void *>(&iostodroid_compat_kEAGLColorFormatRGB565),
        reinterpret_cast<const void *>(&iostodroid_compat_kEAGLColorFormatRGBA8),
        reinterpret_cast<const void *>(&iostodroid_compat_kEAGLDrawablePropertyColorFormat),
        reinterpret_cast<const void *>(&iostodroid_compat_kEAGLDrawablePropertyRetainedBacking),
        reinterpret_cast<const void *>(&iostodroid_compat_ldexp),
        reinterpret_cast<const void *>(&iostodroid_compat_localeconv),
        reinterpret_cast<const void *>(&iostodroid_compat_localtime),
        reinterpret_cast<const void *>(&iostodroid_compat_log10f),
        reinterpret_cast<const void *>(&iostodroid_compat_logf),
        reinterpret_cast<const void *>(&iostodroid_compat_longjmp),
        reinterpret_cast<const void *>(&iostodroid_compat_lseek),
        reinterpret_cast<const void *>(&iostodroid_compat_modf),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_enumerationMutation),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_msgSend),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_msgSendSuper2),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_msgSend_stret),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_setProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_create),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_exit),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_getschedparam),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_join),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_mutex_trylock),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_mutexattr_destroy),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_mutexattr_init),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_mutexattr_settype),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_setschedparam),
        reinterpret_cast<const void *>(&iostodroid_compat_read),
        reinterpret_cast<const void *>(&iostodroid_compat_rename),
        reinterpret_cast<const void *>(&iostodroid_compat_sched_yield),
        reinterpret_cast<const void *>(&iostodroid_compat_select),
        reinterpret_cast<const void *>(&iostodroid_compat_setjmp),
        reinterpret_cast<const void *>(&iostodroid_compat_setlocale),
        reinterpret_cast<const void *>(&iostodroid_compat_setvbuf),
        reinterpret_cast<const void *>(&iostodroid_compat_sinf),
        reinterpret_cast<const void *>(&iostodroid_compat_sinhf),
        reinterpret_cast<const void *>(&iostodroid_compat_sprintf),
        reinterpret_cast<const void *>(&iostodroid_compat_strcasecmp),
        reinterpret_cast<const void *>(&iostodroid_compat_strcat),
        reinterpret_cast<const void *>(&iostodroid_compat_strcoll),
        reinterpret_cast<const void *>(&iostodroid_compat_strcspn),
        reinterpret_cast<const void *>(&iostodroid_compat_strftime),
        reinterpret_cast<const void *>(&iostodroid_compat_strncat),
        reinterpret_cast<const void *>(&iostodroid_compat_strpbrk),
        reinterpret_cast<const void *>(&iostodroid_compat_strtok),
        reinterpret_cast<const void *>(&iostodroid_compat_strtoul),
        reinterpret_cast<const void *>(&iostodroid_compat_system),
        reinterpret_cast<const void *>(&iostodroid_compat_tanf),
        reinterpret_cast<const void *>(&iostodroid_compat_tanhf),
        reinterpret_cast<const void *>(&iostodroid_compat_tmpfile),
        reinterpret_cast<const void *>(&iostodroid_compat_tmpnam),
        reinterpret_cast<const void *>(&iostodroid_compat_ungetc),
        reinterpret_cast<const void *>(&iostodroid_compat_usleep),
        reinterpret_cast<const void *>(&iostodroid_compat_vsprintf),
        reinterpret_cast<const void *>(&iostodroid_compat__exit),
        reinterpret_cast<const void *>(&iostodroid_compat_atexit),
        reinterpret_cast<const void *>(&iostodroid_compat_sscanf),
        reinterpret_cast<const void *>(&iostodroid_compat_putchar),
        reinterpret_cast<const void *>(&iostodroid_compat_getchar),
        reinterpret_cast<const void *>(&iostodroid_compat_fgetc),
        reinterpret_cast<const void *>(&iostodroid_compat_putc),
        reinterpret_cast<const void *>(&iostodroid_compat_rewind),
        reinterpret_cast<const void *>(&iostodroid_compat_fileno),
        reinterpret_cast<const void *>(&iostodroid_compat_fdopen),
        reinterpret_cast<const void *>(&iostodroid_compat_perror),
        reinterpret_cast<const void *>(&iostodroid_compat_tzset),
        reinterpret_cast<const void *>(&iostodroid_compat_sleep),
        reinterpret_cast<const void *>(&iostodroid_compat_open),
        reinterpret_cast<const void *>(&iostodroid_compat_write),
        reinterpret_cast<const void *>(&iostodroid_compat_unlink),
        reinterpret_cast<const void *>(&iostodroid_compat_mkdir),
        reinterpret_cast<const void *>(&iostodroid_compat_rmdir),
        reinterpret_cast<const void *>(&iostodroid_compat_access),
        reinterpret_cast<const void *>(&iostodroid_compat_getcwd),
        reinterpret_cast<const void *>(&iostodroid_compat_chdir),
        reinterpret_cast<const void *>(&iostodroid_compat_stat),
        reinterpret_cast<const void *>(&iostodroid_compat_fstat),
        reinterpret_cast<const void *>(&iostodroid_compat_lstat),
        reinterpret_cast<const void *>(&iostodroid_compat_opendir),
        reinterpret_cast<const void *>(&iostodroid_compat_readdir),
        reinterpret_cast<const void *>(&iostodroid_compat_closedir),
        reinterpret_cast<const void *>(&iostodroid_compat_mmap),
        reinterpret_cast<const void *>(&iostodroid_compat_munmap),
        reinterpret_cast<const void *>(&iostodroid_compat_mprotect),
        reinterpret_cast<const void *>(&iostodroid_compat_poll),
        reinterpret_cast<const void *>(&iostodroid_compat_pipe),
        reinterpret_cast<const void *>(&iostodroid_compat_dup),
        reinterpret_cast<const void *>(&iostodroid_compat_dup2),
        reinterpret_cast<const void *>(&iostodroid_compat_fsync),
        reinterpret_cast<const void *>(&iostodroid_compat_ftruncate),
        reinterpret_cast<const void *>(&iostodroid_compat_truncate),
        reinterpret_cast<const void *>(&iostodroid_compat_chmod),
        reinterpret_cast<const void *>(&iostodroid_compat_umask),
        reinterpret_cast<const void *>(&iostodroid_compat_getuid),
        reinterpret_cast<const void *>(&iostodroid_compat_geteuid),
        reinterpret_cast<const void *>(&iostodroid_compat_getgid),
        reinterpret_cast<const void *>(&iostodroid_compat_getegid),
        reinterpret_cast<const void *>(&iostodroid_compat_getppid),
        reinterpret_cast<const void *>(&iostodroid_compat_sysconf),
        reinterpret_cast<const void *>(&iostodroid_compat_sysctl),
        reinterpret_cast<const void *>(&iostodroid_compat_sysctlbyname),
        reinterpret_cast<const void *>(&iostodroid_compat_getpagesize),
        reinterpret_cast<const void *>(&iostodroid_compat__setjmp),
        reinterpret_cast<const void *>(&iostodroid_compat__longjmp),
        reinterpret_cast<const void *>(&iostodroid_compat_sigaction),
        reinterpret_cast<const void *>(&iostodroid_compat_signal),
        reinterpret_cast<const void *>(&iostodroid_compat_raise),
        reinterpret_cast<const void *>(&iostodroid_compat_kill),
        reinterpret_cast<const void *>(&iostodroid_compat_tolower),
        reinterpret_cast<const void *>(&iostodroid_compat_toupper),
        reinterpret_cast<const void *>(&iostodroid_compat_isalpha),
        reinterpret_cast<const void *>(&iostodroid_compat_isdigit),
        reinterpret_cast<const void *>(&iostodroid_compat_isalnum),
        reinterpret_cast<const void *>(&iostodroid_compat_isspace),
        reinterpret_cast<const void *>(&iostodroid_compat_isupper),
        reinterpret_cast<const void *>(&iostodroid_compat_islower),
        reinterpret_cast<const void *>(&iostodroid_compat_isxdigit),
        reinterpret_cast<const void *>(&iostodroid_compat_strncasecmp),
        reinterpret_cast<const void *>(&iostodroid_compat_strspn),
        reinterpret_cast<const void *>(&iostodroid_compat_strtok_r),
        reinterpret_cast<const void *>(&iostodroid_compat_strtoll),
        reinterpret_cast<const void *>(&iostodroid_compat_strtoull),
        reinterpret_cast<const void *>(&iostodroid_compat_strtof),
        reinterpret_cast<const void *>(&iostodroid_compat_atol),
        reinterpret_cast<const void *>(&iostodroid_compat_atoll),
        reinterpret_cast<const void *>(&iostodroid_compat_llabs),
        reinterpret_cast<const void *>(&iostodroid_compat_bzero),
        reinterpret_cast<const void *>(&iostodroid_compat_bcopy),
        reinterpret_cast<const void *>(&iostodroid_compat_bcmp),
        reinterpret_cast<const void *>(&iostodroid_compat_acos),
        reinterpret_cast<const void *>(&iostodroid_compat_asin),
        reinterpret_cast<const void *>(&iostodroid_compat_atan),
        reinterpret_cast<const void *>(&iostodroid_compat_cosh),
        reinterpret_cast<const void *>(&iostodroid_compat_sinh),
        reinterpret_cast<const void *>(&iostodroid_compat_tanh),
        reinterpret_cast<const void *>(&iostodroid_compat_exp),
        reinterpret_cast<const void *>(&iostodroid_compat_log),
        reinterpret_cast<const void *>(&iostodroid_compat_log10),
        reinterpret_cast<const void *>(&iostodroid_compat_log2),
        reinterpret_cast<const void *>(&iostodroid_compat_hypot),
        reinterpret_cast<const void *>(&iostodroid_compat_hypotf),
        reinterpret_cast<const void *>(&iostodroid_compat_cbrt),
        reinterpret_cast<const void *>(&iostodroid_compat_round),
        reinterpret_cast<const void *>(&iostodroid_compat_roundf),
        reinterpret_cast<const void *>(&iostodroid_compat_trunc),
        reinterpret_cast<const void *>(&iostodroid_compat_truncf),
        reinterpret_cast<const void *>(&iostodroid_compat_lround),
        reinterpret_cast<const void *>(&iostodroid_compat_lroundf),
        reinterpret_cast<const void *>(&iostodroid_compat_frexpf),
        reinterpret_cast<const void *>(&iostodroid_compat_ldexpf),
        reinterpret_cast<const void *>(&iostodroid_compat_log2f),
        reinterpret_cast<const void *>(&iostodroid_compat_modff),
        reinterpret_cast<const void *>(&iostodroid_compat_powf),
        reinterpret_cast<const void *>(&iostodroid_compat_sqrtf),
        reinterpret_cast<const void *>(&iostodroid_compat_fabsf),
        reinterpret_cast<const void *>(&iostodroid_compat_fmodf),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_detach),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_equal),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_once),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_cond_timedwait),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_key_create),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_key_delete),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_setspecific),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_getspecific),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_rwlock_init),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_rwlock_rdlock),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_rwlock_wrlock),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_rwlock_unlock),
        reinterpret_cast<const void *>(&iostodroid_compat_pthread_rwlock_destroy),
        reinterpret_cast<const void *>(&iostodroid_compat_sem_init),
        reinterpret_cast<const void *>(&iostodroid_compat_sem_destroy),
        reinterpret_cast<const void *>(&iostodroid_compat_sem_wait),
        reinterpret_cast<const void *>(&iostodroid_compat_sem_trywait),
        reinterpret_cast<const void *>(&iostodroid_compat_sem_post),
        reinterpret_cast<const void *>(&iostodroid_compat_dlopen),
        reinterpret_cast<const void *>(&iostodroid_compat_dlsym),
        reinterpret_cast<const void *>(&iostodroid_compat_dlclose),
        reinterpret_cast<const void *>(&iostodroid_compat_dlerror),
        reinterpret_cast<const void *>(&iostodroid_compat_socket),
        reinterpret_cast<const void *>(&iostodroid_compat_connect),
        reinterpret_cast<const void *>(&iostodroid_compat_bind),
        reinterpret_cast<const void *>(&iostodroid_compat_listen),
        reinterpret_cast<const void *>(&iostodroid_compat_accept),
        reinterpret_cast<const void *>(&iostodroid_compat_send),
        reinterpret_cast<const void *>(&iostodroid_compat_sendto),
        reinterpret_cast<const void *>(&iostodroid_compat_recv),
        reinterpret_cast<const void *>(&iostodroid_compat_recvfrom),
        reinterpret_cast<const void *>(&iostodroid_compat_setsockopt),
        reinterpret_cast<const void *>(&iostodroid_compat_getsockopt),
        reinterpret_cast<const void *>(&iostodroid_compat_getsockname),
        reinterpret_cast<const void *>(&iostodroid_compat_getpeername),
        reinterpret_cast<const void *>(&iostodroid_compat_shutdown),
        reinterpret_cast<const void *>(&iostodroid_compat_getaddrinfo),
        reinterpret_cast<const void *>(&iostodroid_compat_freeaddrinfo),
        reinterpret_cast<const void *>(&iostodroid_compat_gethostbyname),
        reinterpret_cast<const void *>(&iostodroid_compat_inet_ntop),
        reinterpret_cast<const void *>(&iostodroid_compat_inet_pton),
        reinterpret_cast<const void *>(&iostodroid_compat_inet_addr),
        reinterpret_cast<const void *>(&iostodroid_compat_inet_ntoa),
        reinterpret_cast<const void *>(&iostodroid_compat_htons),
        reinterpret_cast<const void *>(&iostodroid_compat_htonl),
        reinterpret_cast<const void *>(&iostodroid_compat_ntohs),
        reinterpret_cast<const void *>(&iostodroid_compat_ntohl),
        reinterpret_cast<const void *>(&iostodroid_compat_crc32),
        reinterpret_cast<const void *>(&iostodroid_compat_adler32),
        reinterpret_cast<const void *>(&iostodroid_compat_compress),
        reinterpret_cast<const void *>(&iostodroid_compat_compress2),
        reinterpret_cast<const void *>(&iostodroid_compat_uncompress),
        reinterpret_cast<const void *>(&iostodroid_compat_deflateInit_),
        reinterpret_cast<const void *>(&iostodroid_compat_deflateInit2_),
        reinterpret_cast<const void *>(&iostodroid_compat_deflate),
        reinterpret_cast<const void *>(&iostodroid_compat_deflateEnd),
        reinterpret_cast<const void *>(&iostodroid_compat_deflateReset),
        reinterpret_cast<const void *>(&iostodroid_compat_inflateInit_),
        reinterpret_cast<const void *>(&iostodroid_compat_inflateInit2_),
        reinterpret_cast<const void *>(&iostodroid_compat_inflate),
        reinterpret_cast<const void *>(&iostodroid_compat_inflateEnd),
        reinterpret_cast<const void *>(&iostodroid_compat_inflateReset),
        reinterpret_cast<const void *>(&iostodroid_compat_gzopen),
        reinterpret_cast<const void *>(&iostodroid_compat_gzread),
        reinterpret_cast<const void *>(&iostodroid_compat_gzwrite),
        reinterpret_cast<const void *>(&iostodroid_compat_gzclose),
        reinterpret_cast<const void *>(&iostodroid_compat_alDistanceModel),
        reinterpret_cast<const void *>(&iostodroid_compat_alDopplerFactor),
        reinterpret_cast<const void *>(&iostodroid_compat_alDopplerVelocity),
        reinterpret_cast<const void *>(&iostodroid_compat_alSpeedOfSound),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetError),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetSource3f),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetSourcefv),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourcefv),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourcePause),
        reinterpret_cast<const void *>(&iostodroid_compat_alSourceRewind),
        reinterpret_cast<const void *>(&iostodroid_compat_alListener3f),
        reinterpret_cast<const void *>(&iostodroid_compat_alListenerf),
        reinterpret_cast<const void *>(&iostodroid_compat_alListenerfv),
        reinterpret_cast<const void *>(&iostodroid_compat_alListeneri),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetListenerf),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetListener3f),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetListenerfv),
        reinterpret_cast<const void *>(&iostodroid_compat_alEnable),
        reinterpret_cast<const void *>(&iostodroid_compat_alDisable),
        reinterpret_cast<const void *>(&iostodroid_compat_alIsEnabled),
        reinterpret_cast<const void *>(&iostodroid_compat_alIsBuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_alIsSource),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetBoolean),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetInteger),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetFloat),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetDouble),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetString),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetEnumValue),
        reinterpret_cast<const void *>(&iostodroid_compat_alGetProcAddress),
        reinterpret_cast<const void *>(&iostodroid_compat_alIsExtensionPresent),
        reinterpret_cast<const void *>(&iostodroid_compat_alcGetContextsDevice),
        reinterpret_cast<const void *>(&iostodroid_compat_alcGetCurrentContext),
        reinterpret_cast<const void *>(&iostodroid_compat_alcProcessContext),
        reinterpret_cast<const void *>(&iostodroid_compat_alcSuspendContext),
        reinterpret_cast<const void *>(&iostodroid_compat_alcGetError),
        reinterpret_cast<const void *>(&iostodroid_compat_alcGetIntegerv),
        reinterpret_cast<const void *>(&iostodroid_compat_alcGetString),
        reinterpret_cast<const void *>(&iostodroid_compat_alcIsExtensionPresent),
        reinterpret_cast<const void *>(&iostodroid_compat_alcGetProcAddress),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionSetActiveWithFlags),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionGetProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionSetProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionGetPropertySize),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionAddPropertyListener),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioSessionRemovePropertyListenerWithUserData),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioServicesPlaySystemSound),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioServicesPlayAlertSound),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioServicesCreateSystemSoundID),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioServicesDisposeSystemSoundID),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioFileOpenURL),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioFileClose),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioFileGetProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioFileReadBytes),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioFileReadPackets),
        reinterpret_cast<const void *>(&iostodroid_compat_ExtAudioFileOpenURL),
        reinterpret_cast<const void *>(&iostodroid_compat_ExtAudioFileDispose),
        reinterpret_cast<const void *>(&iostodroid_compat_ExtAudioFileGetProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_ExtAudioFileSetProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_ExtAudioFileRead),
        reinterpret_cast<const void *>(&iostodroid_compat_ExtAudioFileSeek),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueNewOutput),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueAllocateBuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueFreeBuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueEnqueueBuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueStart),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueuePause),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueStop),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueDispose),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioQueueSetParameter),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioComponentFindNext),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioComponentInstanceNew),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioComponentInstanceDispose),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioUnitInitialize),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioUnitUninitialize),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioUnitSetProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioUnitGetProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioOutputUnitStart),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioOutputUnitStop),
        reinterpret_cast<const void *>(&iostodroid_compat_AudioUnitRender),
        reinterpret_cast<const void *>(&iostodroid_compat_glAlphaFunc),
        reinterpret_cast<const void *>(&iostodroid_compat_glBindFramebuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_glBindRenderbuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_glBlendEquation),
        reinterpret_cast<const void *>(&iostodroid_compat_glBlendEquationOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glBlendFuncSeparate),
        reinterpret_cast<const void *>(&iostodroid_compat_glBufferSubData),
        reinterpret_cast<const void *>(&iostodroid_compat_glCheckFramebufferStatus),
        reinterpret_cast<const void *>(&iostodroid_compat_glClearDepthf),
        reinterpret_cast<const void *>(&iostodroid_compat_glClearStencil),
        reinterpret_cast<const void *>(&iostodroid_compat_glColor4ub),
        reinterpret_cast<const void *>(&iostodroid_compat_glColorMask),
        reinterpret_cast<const void *>(&iostodroid_compat_glCompileShader),
        reinterpret_cast<const void *>(&iostodroid_compat_glCopyTexImage2D),
        reinterpret_cast<const void *>(&iostodroid_compat_glCopyTexSubImage2D),
        reinterpret_cast<const void *>(&iostodroid_compat_glCreateProgram),
        reinterpret_cast<const void *>(&iostodroid_compat_glCreateShader),
        reinterpret_cast<const void *>(&iostodroid_compat_glCullFace),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteFramebuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteProgram),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteRenderbuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_glDeleteShader),
        reinterpret_cast<const void *>(&iostodroid_compat_glDepthRangef),
        reinterpret_cast<const void *>(&iostodroid_compat_glDisableVertexAttribArray),
        reinterpret_cast<const void *>(&iostodroid_compat_glEnableVertexAttribArray),
        reinterpret_cast<const void *>(&iostodroid_compat_glFinish),
        reinterpret_cast<const void *>(&iostodroid_compat_glFlush),
        reinterpret_cast<const void *>(&iostodroid_compat_glFogf),
        reinterpret_cast<const void *>(&iostodroid_compat_glFogfv),
        reinterpret_cast<const void *>(&iostodroid_compat_glFramebufferRenderbuffer),
        reinterpret_cast<const void *>(&iostodroid_compat_glFramebufferTexture2D),
        reinterpret_cast<const void *>(&iostodroid_compat_glFrustumf),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenFramebuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenRenderbuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenerateMipmap),
        reinterpret_cast<const void *>(&iostodroid_compat_glGenerateMipmapOES),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetAttribLocation),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetError),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetFloatv),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetProgramInfoLog),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetProgramiv),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetRenderbufferParameteriv),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetShaderInfoLog),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetShaderiv),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetString),
        reinterpret_cast<const void *>(&iostodroid_compat_glGetUniformLocation),
        reinterpret_cast<const void *>(&iostodroid_compat_glHint),
        reinterpret_cast<const void *>(&iostodroid_compat_glIsEnabled),
        reinterpret_cast<const void *>(&iostodroid_compat_glIsTexture),
        reinterpret_cast<const void *>(&iostodroid_compat_glLightModelfv),
        reinterpret_cast<const void *>(&iostodroid_compat_glLinkProgram),
        reinterpret_cast<const void *>(&iostodroid_compat_glLoadIdentity),
        reinterpret_cast<const void *>(&iostodroid_compat_glLogicOp),
        reinterpret_cast<const void *>(&iostodroid_compat_glMaterialf),
        reinterpret_cast<const void *>(&iostodroid_compat_glMultMatrixf),
        reinterpret_cast<const void *>(&iostodroid_compat_glNormal3f),
        reinterpret_cast<const void *>(&iostodroid_compat_glOrthof),
        reinterpret_cast<const void *>(&iostodroid_compat_glPointParameterf),
        reinterpret_cast<const void *>(&iostodroid_compat_glPointParameterfv),
        reinterpret_cast<const void *>(&iostodroid_compat_glPointSize),
        reinterpret_cast<const void *>(&iostodroid_compat_glPolygonOffset),
        reinterpret_cast<const void *>(&iostodroid_compat_glPopMatrix),
        reinterpret_cast<const void *>(&iostodroid_compat_glPushMatrix),
        reinterpret_cast<const void *>(&iostodroid_compat_glReadPixels),
        reinterpret_cast<const void *>(&iostodroid_compat_glRenderbufferStorage),
        reinterpret_cast<const void *>(&iostodroid_compat_glRotatef),
        reinterpret_cast<const void *>(&iostodroid_compat_glScalef),
        reinterpret_cast<const void *>(&iostodroid_compat_glShadeModel),
        reinterpret_cast<const void *>(&iostodroid_compat_glShaderSource),
        reinterpret_cast<const void *>(&iostodroid_compat_glStencilFunc),
        reinterpret_cast<const void *>(&iostodroid_compat_glStencilMask),
        reinterpret_cast<const void *>(&iostodroid_compat_glStencilOp),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexEnvf),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexEnvfv),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexParameterf),
        reinterpret_cast<const void *>(&iostodroid_compat_glTexParameterfv),
        reinterpret_cast<const void *>(&iostodroid_compat_glTranslatef),
        reinterpret_cast<const void *>(&iostodroid_compat_glUniform1f),
        reinterpret_cast<const void *>(&iostodroid_compat_glUniform1i),
        reinterpret_cast<const void *>(&iostodroid_compat_glUniform2f),
        reinterpret_cast<const void *>(&iostodroid_compat_glUniform3f),
        reinterpret_cast<const void *>(&iostodroid_compat_glUniform4f),
        reinterpret_cast<const void *>(&iostodroid_compat_glUniformMatrix4fv),
        reinterpret_cast<const void *>(&iostodroid_compat_glUseProgram),
        reinterpret_cast<const void *>(&iostodroid_compat_glVertexAttribPointer),
        reinterpret_cast<const void *>(&iostodroid_compat_eglGetDisplay),
        reinterpret_cast<const void *>(&iostodroid_compat_eglInitialize),
        reinterpret_cast<const void *>(&iostodroid_compat_eglChooseConfig),
        reinterpret_cast<const void *>(&iostodroid_compat_eglCreateWindowSurface),
        reinterpret_cast<const void *>(&iostodroid_compat_eglCreateContext),
        reinterpret_cast<const void *>(&iostodroid_compat_eglMakeCurrent),
        reinterpret_cast<const void *>(&iostodroid_compat_eglSwapBuffers),
        reinterpret_cast<const void *>(&iostodroid_compat_eglDestroyContext),
        reinterpret_cast<const void *>(&iostodroid_compat_eglDestroySurface),
        reinterpret_cast<const void *>(&iostodroid_compat_eglTerminate),
        reinterpret_cast<const void *>(&iostodroid_compat_eglGetError),
        reinterpret_cast<const void *>(&iostodroid_compat_eglGetProcAddress),
        reinterpret_cast<const void *>(&iostodroid_compat_CGColorSpaceCreateDeviceRGB),
        reinterpret_cast<const void *>(&iostodroid_compat_CGColorSpaceCreateDeviceGray),
        reinterpret_cast<const void *>(&iostodroid_compat_CGColorSpaceRelease),
        reinterpret_cast<const void *>(&iostodroid_compat_CGColorSpaceRetain),
        reinterpret_cast<const void *>(&iostodroid_compat_CGBitmapContextCreate),
        reinterpret_cast<const void *>(&iostodroid_compat_CGBitmapContextGetData),
        reinterpret_cast<const void *>(&iostodroid_compat_CGBitmapContextGetWidth),
        reinterpret_cast<const void *>(&iostodroid_compat_CGBitmapContextGetHeight),
        reinterpret_cast<const void *>(&iostodroid_compat_CGBitmapContextGetBytesPerRow),
        reinterpret_cast<const void *>(&iostodroid_compat_CGBitmapContextCreateImage),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextRelease),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextRetain),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextClearRect),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextFillRect),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextDrawImage),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextTranslateCTM),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextScaleCTM),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextRotateCTM),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextSaveGState),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextRestoreGState),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextSetRGBFillColor),
        reinterpret_cast<const void *>(&iostodroid_compat_CGContextSetAlpha),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetWidth),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetHeight),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetBitsPerComponent),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetBitsPerPixel),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetBytesPerRow),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetAlphaInfo),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetDataProvider),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageGetColorSpace),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageRelease),
        reinterpret_cast<const void *>(&iostodroid_compat_CGImageRetain),
        reinterpret_cast<const void *>(&iostodroid_compat_CGDataProviderCopyData),
        reinterpret_cast<const void *>(&iostodroid_compat_CGDataProviderCreateWithData),
        reinterpret_cast<const void *>(&iostodroid_compat_CGDataProviderRelease),
        reinterpret_cast<const void *>(&iostodroid_compat_CGDataProviderRetain),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformMake),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformMakeTranslation),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformMakeScale),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformMakeRotation),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformTranslate),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformScale),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformRotate),
        reinterpret_cast<const void *>(&iostodroid_compat_CGAffineTransformConcat),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_msgSendSuper),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_msgSendSuper_stret),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_msgSendSuper2_stret),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_msgSend_fpret),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_getClass),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_lookUpClass),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_getMetaClass),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_getProtocol),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_allocateClassPair),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_registerClassPair),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_retain),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_release),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_autorelease),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_autoreleasePoolPush),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_autoreleasePoolPop),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_retainAutorelease),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_retainAutoreleaseReturnValue),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_retainAutoreleasedReturnValue),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_storeStrong),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_storeWeak),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_loadWeakRetained),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_destroyWeak),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_getProperty),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_copyStruct),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_sync_enter),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_sync_exit),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_exception_throw),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_begin_catch),
        reinterpret_cast<const void *>(&iostodroid_compat_objc_end_catch),
        reinterpret_cast<const void *>(&iostodroid_compat_sel_registerName),
        reinterpret_cast<const void *>(&iostodroid_compat_sel_getUid),
        reinterpret_cast<const void *>(&iostodroid_compat_sel_getName),
        reinterpret_cast<const void *>(&iostodroid_compat_class_getName),
        reinterpret_cast<const void *>(&iostodroid_compat_class_getSuperclass),
        reinterpret_cast<const void *>(&iostodroid_compat_class_getInstanceMethod),
        reinterpret_cast<const void *>(&iostodroid_compat_class_getClassMethod),
        reinterpret_cast<const void *>(&iostodroid_compat_class_addMethod),
        reinterpret_cast<const void *>(&iostodroid_compat_class_replaceMethod),
        reinterpret_cast<const void *>(&iostodroid_compat_class_createInstance),
        reinterpret_cast<const void *>(&iostodroid_compat_object_getClass),
        reinterpret_cast<const void *>(&iostodroid_compat_object_getClassName),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___MPMoviePlayerController),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSDate),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSLocale),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSNotificationCenter),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSUserDefaults),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIColor),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIDevice),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIImage),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIViewController),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___AVAudioPlayer),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___AVAudioSession),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSArray),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSMutableArray),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSMutableDictionary),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSMutableString),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSData),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSMutableData),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSSet),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSMutableSet),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSFileManager),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSTimer),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSRunLoop),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSProcessInfo),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSValue),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___NSError),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIImageView),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UILabel),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIButton),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIScrollView),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIAlertView),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIActivityIndicatorView),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIWebView),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIFont),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UITouch),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___UIEvent),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___CALayer),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___CATransaction),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___CABasicAnimation),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___SKPaymentQueue),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___SKProductsRequest),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___GKLocalPlayer),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___CMMotionManager),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_CLASS___GCController),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_METACLASS___UIViewController),
        reinterpret_cast<const void *>(&iostodroid_compat_OBJC_METACLASS___UIApplication),
        reinterpret_cast<const void *>(&iostodroid_compat_UIGraphicsPushContext),
        reinterpret_cast<const void *>(&iostodroid_compat_UIGraphicsPopContext),
        reinterpret_cast<const void *>(&iostodroid_compat_UIGraphicsGetCurrentContext),
        reinterpret_cast<const void *>(&iostodroid_compat_UIGraphicsBeginImageContext),
        reinterpret_cast<const void *>(&iostodroid_compat_UIGraphicsBeginImageContextWithOptions),
        reinterpret_cast<const void *>(&iostodroid_compat_UIGraphicsGetImageFromCurrentImageContext),
        reinterpret_cast<const void *>(&iostodroid_compat_UIGraphicsEndImageContext),
        reinterpret_cast<const void *>(&iostodroid_compat_UIImagePNGRepresentation),
        reinterpret_cast<const void *>(&iostodroid_compat_UIImageJPEGRepresentation),
        reinterpret_cast<const void *>(&iostodroid_compat_UIImageWriteToSavedPhotosAlbum),
        reinterpret_cast<const void *>(&iostodroid_compat_NSTemporaryDirectory),
        reinterpret_cast<const void *>(&iostodroid_compat_NSHomeDirectory),
        reinterpret_cast<const void *>(&iostodroid_compat_NSLog),
        reinterpret_cast<const void *>(&iostodroid_compat_NSStringFromClass),
        reinterpret_cast<const void *>(&iostodroid_compat_NSClassFromString),
        reinterpret_cast<const void *>(&iostodroid_compat_NSStringFromSelector),
        reinterpret_cast<const void *>(&iostodroid_compat_NSSelectorFromString),
        reinterpret_cast<const void *>(&iostodroid_compat_NSPageSize),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_async),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_sync),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_after),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_once),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_async_f),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_sync_f),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_once_f),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_get_main_queue),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_get_global_queue),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_queue_create),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_release),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_retain),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_time),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_semaphore_create),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_semaphore_wait),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_semaphore_signal),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_group_create),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_group_async),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_group_enter),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_group_leave),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_group_wait),
        reinterpret_cast<const void *>(&iostodroid_compat_dispatch_group_notify),
        reinterpret_cast<const void *>(&iostodroid_compat__dispatch_main_q),
        reinterpret_cast<const void *>(&iostodroid_compat_SCNetworkReachabilityCreateWithAddress),
        reinterpret_cast<const void *>(&iostodroid_compat_SCNetworkReachabilityCreateWithName),
        reinterpret_cast<const void *>(&iostodroid_compat_SCNetworkReachabilityGetFlags),
        reinterpret_cast<const void *>(&iostodroid_compat_SCNetworkReachabilitySetCallback),
        reinterpret_cast<const void *>(&iostodroid_compat_SCNetworkReachabilityScheduleWithRunLoop),
        reinterpret_cast<const void *>(&iostodroid_compat_SCNetworkReachabilityUnscheduleFromRunLoop),
        reinterpret_cast<const void *>(&iostodroid_compat_SCNetworkReachabilitySetDispatchQueue),
        reinterpret_cast<const void *>(&iostodroid_compat_SecRandomCopyBytes),
        reinterpret_cast<const void *>(&iostodroid_compat_SecItemCopyMatching),
        reinterpret_cast<const void *>(&iostodroid_compat_SecItemAdd),
        reinterpret_cast<const void *>(&iostodroid_compat_SecItemUpdate),
        reinterpret_cast<const void *>(&iostodroid_compat_SecItemDelete),
        reinterpret_cast<const void *>(&iostodroid_compat_CC_MD5),
        reinterpret_cast<const void *>(&iostodroid_compat_CC_SHA1),
        reinterpret_cast<const void *>(&iostodroid_compat_CC_SHA256),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_DeleteException),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_GetIP),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_SetIP),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_GetGR),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_SetGR),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_GetLanguageSpecificData),
        reinterpret_cast<const void *>(&iostodroid_compat__Unwind_GetRegionStart),
        reinterpret_cast<const void *>(&iostodroid_compat___gxx_personality_v0),
        reinterpret_cast<const void *>(&iostodroid_compat___gcc_personality_v0),
        reinterpret_cast<const void *>(&iostodroid_compat___udivdi3),
        reinterpret_cast<const void *>(&iostodroid_compat___umoddi3),
        reinterpret_cast<const void *>(&iostodroid_compat___muldi3),
        reinterpret_cast<const void *>(&iostodroid_compat___fixsfdi),
        reinterpret_cast<const void *>(&iostodroid_compat___fixunsdfdi),
        reinterpret_cast<const void *>(&iostodroid_compat___fixunssfdi),
        reinterpret_cast<const void *>(&iostodroid_compat___floatundidf),
        reinterpret_cast<const void *>(&iostodroid_compat___floatundisf),
        reinterpret_cast<const void *>(&iostodroid_compat___ashldi3),
        reinterpret_cast<const void *>(&iostodroid_compat___ashrdi3),
        reinterpret_cast<const void *>(&iostodroid_compat___lshrdi3),
        reinterpret_cast<const void *>(&iostodroid_compat___cmpdi2),
        reinterpret_cast<const void *>(&iostodroid_compat___ucmpdi2),
        reinterpret_cast<const void *>(&iostodroid_compat___clear_cache),
        reinterpret_cast<const void *>(&iostodroid_compat__Znaj),
        reinterpret_cast<const void *>(&iostodroid_compat__Znwj),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_free_exception),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_rethrow),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_guard_acquire),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_guard_release),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_guard_abort),
        reinterpret_cast<const void *>(&iostodroid_compat___cxa_demangle),
        reinterpret_cast<const void *>(&iostodroid_compat___dynamic_cast),
    };
    for (const void *addr : kExpandedShimAddresses) {
        CHECK(addr != nullptr);
    }
}

}  // namespace

int main() {
    testExpandedGameAndFrameworkShims();
    testLibc();
    testStdio();
    testTime();
    testPthread();
    testCoreFoundationObjects();
    testCoreFoundationRunLoop();
    std::cout << "Bounded C/POSIX/CoreFoundation compatibility shims passed\n";
    return 0;
}
