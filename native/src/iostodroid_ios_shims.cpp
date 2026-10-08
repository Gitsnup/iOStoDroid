#include "iostodroid_ios_shims.h"

/*
 * Bounded, host-tested C/POSIX/CoreFoundation compatibility shims.
 *
 * Every function in this file is a real implementation with a tested body
 * (native/tests/iostodroid_ios_shims.cpp). None of them is a stub. Being registered
 * in the compatibility registry makes a symbol resolvable; it does not rewrite
 * an IPA callsite and does not make an iOS app run on Android.
 *
 * Per-symbol selection: when IOSTODROID_API_REPLACEMENTS_ONLY is defined, only the
 * symbols whose IOSTODROID_API_<name> macro is defined are compiled, so a generated
 * per-IPA libioscompat source carries just the bodies that IPA imports. The
 * CoreFoundation runtime internals are guarded by IOSTODROID_API_NEEDS_CF_RUNTIME,
 * which the generators emit whenever any CoreFoundation shim is selected.
 */

#include <cstring>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_NEEDS_CF_RUNTIME)
#define IOSTODROID_CF_RUNTIME 1
#endif

#ifdef IOSTODROID_CF_RUNTIME

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/*
 * The real object type behind every iostodroid_CF*Ref declared in the header. It is
 * defined at file scope because the header forward-declares exactly this tag;
 * helpers are `static inline` so a build that selects no CoreFoundation shim
 * never trips an unused-function warning.
 */
struct IostodroidCFRunLoopTask {
    std::string mode;
    iostodroid_CFRunLoopPerformCallback callback = nullptr;
    void *context = nullptr;
};

struct IostodroidCFRunLoopState {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<IostodroidCFRunLoopTask> tasks;
    bool stopRequested = false;
    bool wakeRequested = false;
    unsigned runDepth = 0;
};

enum class IostodroidCFKind : uint32_t {
    Allocator = 1,
    String = 2,
    Data = 3,
    Array = 4,
    Dictionary = 5,
    Number = 6,
    Date = 7,
    RunLoop = 8,
};

struct iostodroid_CFRuntime {
    uint32_t magic = 0x52414643u;  // "CFRA"
    IostodroidCFKind kind = IostodroidCFKind::String;
    std::atomic<long> retainCount{1};
    std::string text;                                     // String
    std::vector<uint8_t> bytes;                           // Data
    std::vector<const iostodroid_CFRuntime *> elements;        // Array
    // Dictionary: (key, value) pairs in insertion order.
    std::vector<std::pair<const iostodroid_CFRuntime *, const iostodroid_CFRuntime *>> pairs;
    double number = 0.0;        // Number
    double absoluteTime = 0.0;  // Date
    std::unique_ptr<IostodroidCFRunLoopState> runLoop; // RunLoop

    ~iostodroid_CFRuntime();
};

static inline void iostodroidCfReleaseInternal(const iostodroid_CFRuntime *object);
static inline void iostodroidCfRetainInternal(const iostodroid_CFRuntime *object);

/* The C++ runtime initializes this during library startup on its loading thread. */
static const std::thread::id g_IostodroidCFMainThreadId = std::this_thread::get_id();

iostodroid_CFRuntime::~iostodroid_CFRuntime() {
    magic = 0;
    for (const iostodroid_CFRuntime *element : elements) iostodroidCfReleaseInternal(element);
    for (const auto &pair : pairs) {
        iostodroidCfReleaseInternal(pair.first);
        iostodroidCfReleaseInternal(pair.second);
    }
}

static inline iostodroid_CFRuntime *iostodroidCfMutable(void *reference) {
    auto *object = static_cast<iostodroid_CFRuntime *>(reference);
    return (object != nullptr && object->magic == 0x52414643u) ? object : nullptr;
}

static inline const iostodroid_CFRuntime *iostodroidCfConst(const void *reference) {
    const auto *object = static_cast<const iostodroid_CFRuntime *>(reference);
    return (object != nullptr && object->magic == 0x52414643u) ? object : nullptr;
}

static inline bool iostodroidCfIsKind(const void *reference, IostodroidCFKind kind) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(reference);
    return object != nullptr && object->kind == kind;
}

// Static storage duration: intentionally never heap-allocated, so host leak
// checkers never see the process-lifetime default allocator as a leak.
iostodroid_CFRuntime *iostodroidCfDefaultAllocator() {
    static iostodroid_CFRuntime allocator;
    static const bool initialized = [] {
        allocator.kind = IostodroidCFKind::Allocator;
        allocator.retainCount.store(1);
        return true;
    }();
    (void)initialized;
    return &allocator;
}

iostodroid_CFRuntime *iostodroidCfMainRunLoop();

iostodroid_CFRuntime *iostodroidCfCurrentRunLoop() {
    if (std::this_thread::get_id() == g_IostodroidCFMainThreadId) return iostodroidCfMainRunLoop();
    static thread_local iostodroid_CFRuntime loop;
    static thread_local bool initialized = false;
    if (!initialized) {
        loop.kind = IostodroidCFKind::RunLoop;
        loop.retainCount.store(1);
        loop.runLoop = std::make_unique<IostodroidCFRunLoopState>();
        initialized = true;
    }
    return &loop;
}

iostodroid_CFRuntime *iostodroidCfMainRunLoop() {
    static iostodroid_CFRuntime loop;
    static const bool initialized = [] {
        loop.kind = IostodroidCFKind::RunLoop;
        loop.retainCount.store(1);
        loop.runLoop = std::make_unique<IostodroidCFRunLoopState>();
        return true;
    }();
    (void)initialized;
    return &loop;
}

static inline IostodroidCFRunLoopState *iostodroidCfRunLoopState(iostodroid_CFRunLoopRef reference) {
    auto *object = iostodroidCfMutable(const_cast<iostodroid_CFRuntime *>(reference));
    return object != nullptr && object->kind == IostodroidCFKind::RunLoop ? object->runLoop.get() : nullptr;
}

static inline bool iostodroidCfRunLoopMode(iostodroid_CFStringRef reference, std::string &mode) {
    if (reference == nullptr) {
        mode.clear();
        return true;
    }
    const iostodroid_CFRuntime *object = iostodroidCfConst(reference);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::String)) return false;
    mode = object->text;
    return true;
}

static inline bool iostodroidCfRunLoopHasTask(const IostodroidCFRunLoopState &state, const std::string &mode) {
    return std::any_of(state.tasks.begin(), state.tasks.end(), [&mode](const IostodroidCFRunLoopTask &task) {
        return task.mode.empty() || mode.empty() || task.mode == mode;
    });
}

static inline iostodroid_CFRunLoopRunResult iostodroidCfRunLoopRunModeImpl(IostodroidCFRunLoopState &state,
                                                            const std::string &mode,
                                                            double seconds,
                                                            bool returnAfterSourceHandled,
                                                            bool forceInfinite) {
    using Clock = std::chrono::steady_clock;
    const bool infinite = forceInfinite || !std::isfinite(seconds) || seconds > 31536000.0;
    const double timeoutSeconds = std::max(0.0, seconds);
    const auto deadline = infinite
                              ? Clock::time_point::max()
                              : Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                   std::chrono::duration<double>(timeoutSeconds));
    std::unique_lock<std::mutex> lock(state.mutex);
    ++state.runDepth;
    const auto finish = [&state](iostodroid_CFRunLoopRunResult result) {
        --state.runDepth;
        return result;
    };

    while (true) {
        if (state.stopRequested) {
            state.stopRequested = false;
            state.wakeRequested = false;
            return finish(IOSTODROID_KCFRUNLOOPRUNSTOPPED);
        }

        auto task = std::find_if(state.tasks.begin(), state.tasks.end(), [&mode](const IostodroidCFRunLoopTask &entry) {
            return entry.mode.empty() || mode.empty() || entry.mode == mode;
        });
        if (task != state.tasks.end()) {
            const IostodroidCFRunLoopTask ready = *task;
            state.tasks.erase(task);
            lock.unlock();
            try {
                ready.callback(ready.context);
            } catch (...) {
                // A callback cannot unwind through this C ABI boundary.
            }
            lock.lock();
            if (returnAfterSourceHandled) return finish(IOSTODROID_KCFRUNLOOPRUNHANDLEDSOURCE);
            continue;
        }

        state.wakeRequested = false;
        if (!infinite && seconds == 0.0) return finish(IOSTODROID_KCFRUNLOOPRUNTIMEDOUT);
        const auto ready = [&state, &mode] {
            return state.stopRequested || state.wakeRequested || iostodroidCfRunLoopHasTask(state, mode);
        };
        if (infinite) {
            state.condition.wait(lock, ready);
        } else if (!state.condition.wait_until(lock, deadline, ready)) {
            return finish(IOSTODROID_KCFRUNLOOPRUNTIMEDOUT);
        }
    }
}

static inline void iostodroidCfRetainInternal(const iostodroid_CFRuntime *object) {
    if (object == nullptr || object->kind == IostodroidCFKind::Allocator || object->kind == IostodroidCFKind::RunLoop) return;
    const_cast<iostodroid_CFRuntime *>(object)->retainCount.fetch_add(1, std::memory_order_relaxed);
}

static inline void iostodroidCfReleaseInternal(const iostodroid_CFRuntime *object) {
    if (object == nullptr || object->kind == IostodroidCFKind::Allocator || object->kind == IostodroidCFKind::RunLoop) return;
    auto *mutableObject = const_cast<iostodroid_CFRuntime *>(object);
    if (mutableObject->retainCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete mutableObject;
    }
}

// Dictionary keys are compared by CFString content when the key is a string and
// by pointer identity otherwise, which matches kCFTypeDictionaryKeyCallBacks
// closely enough for the documented behaviour of these accessors.
static inline bool iostodroidCfKeyEquals(const iostodroid_CFRuntime *left, const iostodroid_CFRuntime *right) {
    if (left == right) return true;
    if (left == nullptr || right == nullptr) return false;
    if (left->kind == IostodroidCFKind::String && right->kind == IostodroidCFKind::String) {
        return left->text == right->text;
    }
    return false;
}

extern "C" iostodroid_CFAllocatorRef iostodroid_compat_CFAllocatorGetDefault(void) {
    return iostodroidCfDefaultAllocator();
}

extern "C" iostodroid_CFTypeRef iostodroid_compat_CFRetain(iostodroid_CFTypeRef object) {
    iostodroidCfRetainInternal(iostodroidCfConst(object));
    return object;
}

extern "C" void iostodroid_compat_CFRelease(iostodroid_CFTypeRef object) {
    iostodroidCfReleaseInternal(iostodroidCfConst(object));
}

extern "C" iostodroid_CFIndex iostodroid_compat_CFGetRetainCount(iostodroid_CFTypeRef object) {
    const iostodroid_CFRuntime *target = iostodroidCfConst(object);
    if (target == nullptr) return 0;
    return static_cast<iostodroid_CFIndex>(target->retainCount.load(std::memory_order_relaxed));
}

#endif  // IOSTODROID_CF_RUNTIME

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringCreateWithCString)
extern "C" iostodroid_CFStringRef iostodroid_compat_CFStringCreateWithCString(iostodroid_CFAllocatorRef allocator,
                                                                    const char *cString,
                                                                    iostodroid_CFStringEncoding encoding) {
    (void)allocator;
    if (cString == nullptr || encoding != IOSTODROID_KCFSTRINGENCODINGUTF8) return nullptr;
    auto *object = new iostodroid_CFRuntime();
    object->kind = IostodroidCFKind::String;
    object->text.assign(cString);
    return object;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringGetLength)
extern "C" iostodroid_CFIndex iostodroid_compat_CFStringGetLength(iostodroid_CFStringRef string) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(string);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::String)) return 0;
    return static_cast<iostodroid_CFIndex>(object->text.size());
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringGetCString)
extern "C" iostodroid_Boolean iostodroid_compat_CFStringGetCString(iostodroid_CFStringRef string, char *buffer,
                                                         iostodroid_CFIndex bufferSize,
                                                         iostodroid_CFStringEncoding encoding) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(string);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::String) || buffer == nullptr || bufferSize <= 0) return 0;
    if (encoding != IOSTODROID_KCFSTRINGENCODINGUTF8) return 0;
    const size_t needed = object->text.size() + 1u;
    if (static_cast<size_t>(bufferSize) < needed) return 0;
    std::memcpy(buffer, object->text.c_str(), needed);
    return 1;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringGetCStringPtr)
extern "C" const char *iostodroid_compat_CFStringGetCStringPtr(iostodroid_CFStringRef string,
                                                          iostodroid_CFStringEncoding encoding) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(string);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::String)) return nullptr;
    if (encoding != IOSTODROID_KCFSTRINGENCODINGUTF8) return nullptr;
    return object->text.c_str();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringGetMaximumSizeForEncoding)
extern "C" iostodroid_CFIndex iostodroid_compat_CFStringGetMaximumSizeForEncoding(iostodroid_CFIndex length,
                                                                        iostodroid_CFStringEncoding encoding) {
    if (length < 0) return 0;
    // UTF-8 encodes one UTF-16 code unit in at most three bytes plus a NUL.
    if (encoding == IOSTODROID_KCFSTRINGENCODINGUTF8) return length * 3 + 1;
    // Conservative bound for every other encoding we do not decode here.
    return length * 4 + 1;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringCompare)
extern "C" iostodroid_CFComparisonResult iostodroid_compat_CFStringCompare(iostodroid_CFStringRef left,
                                                                 iostodroid_CFStringRef right,
                                                                 iostodroid_CFOptionFlags options) {
    (void)options;
    const iostodroid_CFRuntime *leftObject = iostodroidCfConst(left);
    const iostodroid_CFRuntime *rightObject = iostodroidCfConst(right);
    if (!iostodroidCfIsKind(leftObject, IostodroidCFKind::String) || !iostodroidCfIsKind(rightObject, IostodroidCFKind::String)) {
        return IOSTODROID_KCFCOMPAREEQUALTO;
    }
    const int comparison = leftObject->text.compare(rightObject->text);
    if (comparison < 0) return IOSTODROID_KCFCOMPARELESSTHAN;
    if (comparison > 0) return IOSTODROID_KCFCOMPAREGREATERTHAN;
    return IOSTODROID_KCFCOMPAREEQUALTO;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringGetSystemEncoding)
extern "C" iostodroid_CFStringEncoding iostodroid_compat_CFStringGetSystemEncoding(void) {
    return IOSTODROID_KCFSTRINGENCODINGUTF8;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDataCreate)
extern "C" iostodroid_CFDataRef iostodroid_compat_CFDataCreate(iostodroid_CFAllocatorRef allocator, const uint8_t *bytes,
                                                     iostodroid_CFIndex length) {
    (void)allocator;
    if (length < 0) return nullptr;
    auto *object = new iostodroid_CFRuntime();
    object->kind = IostodroidCFKind::Data;
    if (bytes != nullptr && length > 0) {
        object->bytes.assign(bytes, bytes + static_cast<size_t>(length));
    }
    return object;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDataGetBytePtr)
extern "C" const uint8_t *iostodroid_compat_CFDataGetBytePtr(iostodroid_CFDataRef data) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(data);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Data) || object->bytes.empty()) return nullptr;
    return object->bytes.data();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDataGetLength)
extern "C" iostodroid_CFIndex iostodroid_compat_CFDataGetLength(iostodroid_CFDataRef data) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(data);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Data)) return 0;
    return static_cast<iostodroid_CFIndex>(object->bytes.size());
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFArrayCreateMutable)
extern "C" iostodroid_CFMutableArrayRef iostodroid_compat_CFArrayCreateMutable(iostodroid_CFAllocatorRef allocator,
                                                                     iostodroid_CFIndex capacity,
                                                                     iostodroid_CFArrayCallBacksRef callBacks) {
    (void)allocator;
    (void)callBacks;  // kCFTypeArrayCallBacks semantics are always applied.
    auto *object = new iostodroid_CFRuntime();
    object->kind = IostodroidCFKind::Array;
    if (capacity > 0) object->elements.reserve(static_cast<size_t>(capacity));
    return object;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFArrayAppendValue)
extern "C" void iostodroid_compat_CFArrayAppendValue(iostodroid_CFMutableArrayRef array, const void *value) {
    iostodroid_CFRuntime *object = iostodroidCfMutable(array);
    if (object == nullptr || object->kind != IostodroidCFKind::Array) return;
    const iostodroid_CFRuntime *element = iostodroidCfConst(value);
    if (element == nullptr) return;
    iostodroidCfRetainInternal(element);
    object->elements.push_back(element);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFArrayGetCount)
extern "C" iostodroid_CFIndex iostodroid_compat_CFArrayGetCount(iostodroid_CFArrayRef array) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(array);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Array)) return 0;
    return static_cast<iostodroid_CFIndex>(object->elements.size());
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFArrayGetValueAtIndex)
extern "C" const void *iostodroid_compat_CFArrayGetValueAtIndex(iostodroid_CFArrayRef array, iostodroid_CFIndex index) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(array);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Array)) return nullptr;
    if (index < 0 || static_cast<size_t>(index) >= object->elements.size()) return nullptr;
    return object->elements[static_cast<size_t>(index)];
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionaryCreateMutable)
extern "C" iostodroid_CFMutableDictionaryRef iostodroid_compat_CFDictionaryCreateMutable(
    iostodroid_CFAllocatorRef allocator, iostodroid_CFIndex capacity,
    iostodroid_CFDictionaryKeyCallBacksRef keyCallBacks,
    iostodroid_CFDictionaryValueCallBacksRef valueCallBacks) {
    (void)allocator;
    (void)keyCallBacks;    // kCFTypeDictionaryKeyCallBacks semantics are always applied.
    (void)valueCallBacks;  // kCFTypeDictionaryValueCallBacks semantics are always applied.
    auto *object = new iostodroid_CFRuntime();
    object->kind = IostodroidCFKind::Dictionary;
    if (capacity > 0) object->pairs.reserve(static_cast<size_t>(capacity));
    return object;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionarySetValue)
extern "C" void iostodroid_compat_CFDictionarySetValue(iostodroid_CFMutableDictionaryRef dictionary, const void *key,
                                                  const void *value) {
    iostodroid_CFRuntime *object = iostodroidCfMutable(dictionary);
    if (object == nullptr || object->kind != IostodroidCFKind::Dictionary) return;
    const iostodroid_CFRuntime *keyObject = iostodroidCfConst(key);
    const iostodroid_CFRuntime *valueObject = iostodroidCfConst(value);
    if (keyObject == nullptr || valueObject == nullptr) return;
    for (auto &pair : object->pairs) {
        if (iostodroidCfKeyEquals(pair.first, keyObject)) {
            iostodroidCfRetainInternal(valueObject);
            const iostodroid_CFRuntime *previous = pair.second;
            pair.second = valueObject;
            iostodroidCfReleaseInternal(previous);
            return;
        }
    }
    iostodroidCfRetainInternal(keyObject);
    iostodroidCfRetainInternal(valueObject);
    object->pairs.emplace_back(keyObject, valueObject);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionaryGetValue)
extern "C" const void *iostodroid_compat_CFDictionaryGetValue(iostodroid_CFDictionaryRef dictionary, const void *key) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(dictionary);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Dictionary)) return nullptr;
    const iostodroid_CFRuntime *keyObject = iostodroidCfConst(key);
    if (keyObject == nullptr) return nullptr;
    for (const auto &pair : object->pairs) {
        if (iostodroidCfKeyEquals(pair.first, keyObject)) return pair.second;
    }
    return nullptr;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionaryGetCount)
extern "C" iostodroid_CFIndex iostodroid_compat_CFDictionaryGetCount(iostodroid_CFDictionaryRef dictionary) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(dictionary);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Dictionary)) return 0;
    return static_cast<iostodroid_CFIndex>(object->pairs.size());
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFNumberCreate)
extern "C" iostodroid_CFNumberRef iostodroid_compat_CFNumberCreate(iostodroid_CFAllocatorRef allocator,
                                                         iostodroid_CFNumberType type, const void *valuePointer) {
    (void)allocator;
    if (valuePointer == nullptr) return nullptr;
    double value = 0.0;
    switch (type) {
        case IOSTODROID_KCFNUMBERSINT32TYPE:
        case IOSTODROID_KCFNUMBERINTTYPE: {
            int32_t raw = 0;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = static_cast<double>(raw);
            break;
        }
        case IOSTODROID_KCFNUMBERSINT64TYPE:
        case IOSTODROID_KCFNUMBERLONGTYPE: {
            int64_t raw = 0;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = static_cast<double>(raw);
            break;
        }
        case IOSTODROID_KCFNUMBERFLOAT32TYPE: {
            float raw = 0.0f;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = static_cast<double>(raw);
            break;
        }
        case IOSTODROID_KCFNUMBERFLOAT64TYPE:
        case IOSTODROID_KCFNUMBERDOUBLETYPE: {
            double raw = 0.0;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = raw;
            break;
        }
        default:
            return nullptr;  // Unsupported type is refused rather than guessed.
    }
    auto *object = new iostodroid_CFRuntime();
    object->kind = IostodroidCFKind::Number;
    object->number = value;
    return object;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFNumberGetValue)
extern "C" iostodroid_Boolean iostodroid_compat_CFNumberGetValue(iostodroid_CFNumberRef number, iostodroid_CFNumberType type,
                                                       void *valuePointer) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(number);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Number) || valuePointer == nullptr) return 0;
    const double value = object->number;
    switch (type) {
        case IOSTODROID_KCFNUMBERSINT32TYPE:
        case IOSTODROID_KCFNUMBERINTTYPE: {
            auto raw = static_cast<int32_t>(value);
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        case IOSTODROID_KCFNUMBERSINT64TYPE:
        case IOSTODROID_KCFNUMBERLONGTYPE: {
            auto raw = static_cast<int64_t>(value);
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        case IOSTODROID_KCFNUMBERFLOAT32TYPE: {
            auto raw = static_cast<float>(value);
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        case IOSTODROID_KCFNUMBERFLOAT64TYPE:
        case IOSTODROID_KCFNUMBERDOUBLETYPE: {
            double raw = value;
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        default:
            return 0;
    }
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDateCreate)
extern "C" iostodroid_CFDateRef iostodroid_compat_CFDateCreate(iostodroid_CFAllocatorRef allocator,
                                                     iostodroid_CFAbsoluteTime absoluteTime) {
    (void)allocator;
    auto *object = new iostodroid_CFRuntime();
    object->kind = IostodroidCFKind::Date;
    object->absoluteTime = absoluteTime;
    return object;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDateGetAbsoluteTime)
extern "C" iostodroid_CFAbsoluteTime iostodroid_compat_CFDateGetAbsoluteTime(iostodroid_CFDateRef date) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(date);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Date)) return 0.0;
    return object->absoluteTime;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDateGetTimeIntervalSinceDate)
extern "C" iostodroid_CFTimeInterval iostodroid_compat_CFDateGetTimeIntervalSinceDate(iostodroid_CFDateRef date,
                                                                            iostodroid_CFDateRef other) {
    const iostodroid_CFRuntime *object = iostodroidCfConst(date);
    const iostodroid_CFRuntime *otherObject = iostodroidCfConst(other);
    if (!iostodroidCfIsKind(object, IostodroidCFKind::Date) || !iostodroidCfIsKind(otherObject, IostodroidCFKind::Date)) {
        return 0.0;
    }
    return object->absoluteTime - otherObject->absoluteTime;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFAbsoluteTimeGetGregorianDate)
extern "C" iostodroid_CFGregorianDate iostodroid_compat_CFAbsoluteTimeGetGregorianDate(iostodroid_CFAbsoluteTime absoluteTime,
                                                                             iostodroid_CFTimeZoneRef timeZone) {
    (void)timeZone;  // Only UTC is implemented; the zone argument is ignored.
    iostodroid_CFGregorianDate result{};
    // CFAbsoluteTime is seconds since 2001-01-01 00:00:00 UTC.
    const double unixSeconds = absoluteTime + 978307200.0;
    const time_t whole = static_cast<time_t>(unixSeconds);
    struct tm broken{};
    if (gmtime_r(&whole, &broken) == nullptr) return result;
    result.year = static_cast<int32_t>(broken.tm_year + 1900);
    result.month = static_cast<int8_t>(broken.tm_mon + 1);
    result.day = static_cast<int8_t>(broken.tm_mday);
    result.hour = static_cast<int8_t>(broken.tm_hour);
    result.minute = static_cast<int8_t>(broken.tm_min);
    result.second = static_cast<double>(broken.tm_sec) + (unixSeconds - static_cast<double>(whole));
    return result;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopGetCurrent)
extern "C" iostodroid_CFRunLoopRef iostodroid_compat_CFRunLoopGetCurrent(void) {
    return iostodroidCfCurrentRunLoop();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopGetMain)
extern "C" iostodroid_CFRunLoopRef iostodroid_compat_CFRunLoopGetMain(void) {
    return iostodroidCfMainRunLoop();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopRun)
extern "C" void iostodroid_compat_CFRunLoopRun(void) {
    auto *loop = iostodroidCfCurrentRunLoop();
    iostodroidCfRunLoopRunModeImpl(*loop->runLoop, std::string(), -1.0, false, true);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopRunInMode)
extern "C" iostodroid_CFRunLoopRunResult iostodroid_compat_CFRunLoopRunInMode(
    iostodroid_CFStringRef mode, iostodroid_CFTimeInterval seconds, iostodroid_Boolean returnAfterSourceHandled) {
    std::string modeValue;
    if (!iostodroidCfRunLoopMode(mode, modeValue)) return IOSTODROID_KCFRUNLOOPRUNTIMEDOUT;
    auto *loop = iostodroidCfCurrentRunLoop();
    return iostodroidCfRunLoopRunModeImpl(*loop->runLoop, modeValue, seconds,
                                     returnAfterSourceHandled != 0, false);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopStop)
extern "C" void iostodroid_compat_CFRunLoopStop(iostodroid_CFRunLoopRef runLoop) {
    IostodroidCFRunLoopState *state = iostodroidCfRunLoopState(runLoop);
    if (state == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopRequested = true;
        state->wakeRequested = true;
    }
    state->condition.notify_all();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopWakeUp)
extern "C" void iostodroid_compat_CFRunLoopWakeUp(iostodroid_CFRunLoopRef runLoop) {
    IostodroidCFRunLoopState *state = iostodroidCfRunLoopState(runLoop);
    if (state == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->wakeRequested = true;
    }
    state->condition.notify_all();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_NEEDS_CF_RUNTIME)
extern "C" iostodroid_Boolean iostodroid_compat_CFRunLoopPerform(
    iostodroid_CFRunLoopRef runLoop, iostodroid_CFStringRef mode,
    iostodroid_CFRunLoopPerformCallback callback, void *context) {
    if (callback == nullptr) return 0;
    IostodroidCFRunLoopState *state = iostodroidCfRunLoopState(runLoop);
    if (state == nullptr) return 0;
    std::string modeValue;
    if (!iostodroidCfRunLoopMode(mode, modeValue)) return 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->tasks.push_back(IostodroidCFRunLoopTask{std::move(modeValue), callback, context});
        state->wakeRequested = true;
    }
    state->condition.notify_one();
    return 1;
}
#endif

/* --- libc / POSIX forwards ------------------------------------------------ */

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_malloc)
extern "C" void *iostodroid_compat_malloc(size_t size) { return malloc(size); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_calloc)
extern "C" void *iostodroid_compat_calloc(size_t count, size_t size) { return calloc(count, size); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_realloc)
extern "C" void *iostodroid_compat_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_free)
extern "C" void iostodroid_compat_free(void *pointer) { free(pointer); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_memcpy)
extern "C" void *iostodroid_compat_memcpy(void *destination, const void *source, size_t size) {
    return memcpy(destination, source, size);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_memmove)
extern "C" void *iostodroid_compat_memmove(void *destination, const void *source, size_t size) {
    return memmove(destination, source, size);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_memset)
extern "C" void *iostodroid_compat_memset(void *destination, int value, size_t size) {
    return memset(destination, value, size);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_memcmp)
extern "C" int iostodroid_compat_memcmp(const void *left, const void *right, size_t size) {
    return memcmp(left, right, size);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_memchr)
extern "C" void *iostodroid_compat_memchr(const void *source, int value, size_t size) {
    return const_cast<void *>(memchr(source, value, size));
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strlen)
extern "C" size_t iostodroid_compat_strlen(const char *string) { return strlen(string); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strcpy)
extern "C" char *iostodroid_compat_strcpy(char *destination, const char *source) {
    return strcpy(destination, source);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strncpy)
extern "C" char *iostodroid_compat_strncpy(char *destination, const char *source, size_t size) {
    return strncpy(destination, source, size);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strlcpy)
// OpenBSD semantics: always NUL-terminate when size > 0, return strlen(source).
extern "C" size_t iostodroid_compat_strlcpy(char *destination, const char *source, size_t size) {
    const size_t length = strlen(source);
    if (size > 0) {
        const size_t copy = length >= size ? size - 1u : length;
        memcpy(destination, source, copy);
        destination[copy] = '\0';
    }
    return length;
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strlcat)
extern "C" size_t iostodroid_compat_strlcat(char *destination, const char *source, size_t size) {
    const size_t sourceLength = strlen(source);
    if (size == 0) return sourceLength;
    size_t used = 0;
    while (used < size && destination[used] != '\0') used++;
    if (used == size) return size + sourceLength;  // No NUL in the buffer: nothing to append to.
    const size_t remaining = size - used - 1u;
    const size_t copy = sourceLength > remaining ? remaining : sourceLength;
    memcpy(destination + used, source, copy);
    destination[used + copy] = '\0';
    return used + sourceLength;
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strcmp)
extern "C" int iostodroid_compat_strcmp(const char *left, const char *right) { return strcmp(left, right); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strncmp)
extern "C" int iostodroid_compat_strncmp(const char *left, const char *right, size_t size) {
    return strncmp(left, right, size);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strdup)
extern "C" char *iostodroid_compat_strdup(const char *string) { return strdup(string); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strchr)
extern "C" char *iostodroid_compat_strchr(const char *string, int value) {
    return const_cast<char *>(strchr(string, value));
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strrchr)
extern "C" char *iostodroid_compat_strrchr(const char *string, int value) {
    return const_cast<char *>(strrchr(string, value));
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strstr)
extern "C" char *iostodroid_compat_strstr(const char *haystack, const char *needle) {
    return const_cast<char *>(strstr(haystack, needle));
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtol)
extern "C" long iostodroid_compat_strtol(const char *string, char **end, int base) {
    return strtol(string, end, base);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtod)
extern "C" double iostodroid_compat_strtod(const char *string, char **end) { return strtod(string, end); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atoi)
extern "C" int iostodroid_compat_atoi(const char *string) { return atoi(string); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atof)
extern "C" double iostodroid_compat_atof(const char *string) { return atof(string); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strerror)
extern "C" char *iostodroid_compat_strerror(int code) { return strerror(code); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_snprintf)
extern "C" int iostodroid_compat_snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_vsnprintf)
extern "C" int iostodroid_compat_vsnprintf(char *buffer, size_t size, const char *format, va_list arguments) {
    return vsnprintf(buffer, size, format, arguments);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fopen)
extern "C" FILE *iostodroid_compat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fclose)
extern "C" int iostodroid_compat_fclose(FILE *stream) { return fclose(stream); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fread)
extern "C" size_t iostodroid_compat_fread(void *buffer, size_t size, size_t count, FILE *stream) {
    return fread(buffer, size, count, stream);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fwrite)
extern "C" size_t iostodroid_compat_fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
    return fwrite(buffer, size, count, stream);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fputs)
extern "C" int iostodroid_compat_fputs(const char *string, FILE *stream) { return fputs(string, stream); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fgets)
extern "C" char *iostodroid_compat_fgets(char *buffer, int size, FILE *stream) { return fgets(buffer, size, stream); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fflush)
extern "C" int iostodroid_compat_fflush(FILE *stream) { return fflush(stream); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fprintf)
extern "C" int iostodroid_compat_fprintf(FILE *stream, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = vfprintf(stream, format, arguments);
    va_end(arguments);
    return written;
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_printf)
extern "C" int iostodroid_compat_printf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = vprintf(format, arguments);
    va_end(arguments);
    return written;
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_puts)
extern "C" int iostodroid_compat_puts(const char *string) { return puts(string); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_remove)
extern "C" int iostodroid_compat_remove(const char *path) { return remove(path); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_feof)
extern "C" int iostodroid_compat_feof(FILE *stream) { return feof(stream); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ftell)
extern "C" long iostodroid_compat_ftell(FILE *stream) { return ftell(stream); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fseek)
extern "C" int iostodroid_compat_fseek(FILE *stream, long offset, int origin) { return fseek(stream, offset, origin); }
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_time)
extern "C" time_t iostodroid_compat_time(time_t *result) { return time(result); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gettimeofday)
extern "C" int iostodroid_compat_gettimeofday(iostodroid_darwin_timeval *result, void *timeZone) {
    (void)timeZone;
    if (result == nullptr) return -1;
    struct timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    result->tv_sec = static_cast<int64_t>(now.tv_sec);
    result->tv_usec = static_cast<int32_t>(now.tv_nsec / 1000);
    result->tv_pad = 0;
    return 0;
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_clock_gettime)
extern "C" int iostodroid_compat_clock_gettime(int clockIdentifier, struct timespec *result) {
    return clock_gettime(static_cast<clockid_t>(clockIdentifier), result);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_nanosleep)
extern "C" int iostodroid_compat_nanosleep(const struct timespec *request, struct timespec *remaining) {
    return nanosleep(request, remaining);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_localtime_r)
extern "C" struct tm *iostodroid_compat_localtime_r(const time_t *clock, struct tm *result) {
    return localtime_r(clock, result);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gmtime_r)
extern "C" struct tm *iostodroid_compat_gmtime_r(const time_t *clock, struct tm *result) {
    return gmtime_r(clock, result);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_mktime)
extern "C" time_t iostodroid_compat_mktime(struct tm *value) { return mktime(value); }
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getenv)
extern "C" char *iostodroid_compat_getenv(const char *name) { return getenv(name); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_setenv)
extern "C" int iostodroid_compat_setenv(const char *name, const char *value, int overwrite) {
    return setenv(name, value, overwrite);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_unsetenv)
extern "C" int iostodroid_compat_unsetenv(const char *name) { return unsetenv(name); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getpid)
extern "C" long iostodroid_compat_getpid(void) { return static_cast<long>(getpid()); }
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_qsort)
extern "C" void iostodroid_compat_qsort(void *base, size_t count, size_t size,
                                   int (*compare)(const void *, const void *)) {
    qsort(base, count, size, compare);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_bsearch)
extern "C" void *iostodroid_compat_bsearch(const void *key, const void *base, size_t count, size_t size,
                                      int (*compare)(const void *, const void *)) {
    return const_cast<void *>(bsearch(key, base, count, size, compare));
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_abs)
extern "C" int iostodroid_compat_abs(int value) { return abs(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_labs)
extern "C" long iostodroid_compat_labs(long value) { return labs(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_rand)
extern "C" int iostodroid_compat_rand(void) { return rand(); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_srand)
extern "C" void iostodroid_compat_srand(unsigned int seed) { srand(seed); }
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sqrt)
extern "C" double iostodroid_compat_sqrt(double value) { return sqrt(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fabs)
extern "C" double iostodroid_compat_fabs(double value) { return fabs(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_floor)
extern "C" double iostodroid_compat_floor(double value) { return floor(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ceil)
extern "C" double iostodroid_compat_ceil(double value) { return ceil(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pow)
extern "C" double iostodroid_compat_pow(double base, double exponent) { return pow(base, exponent); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sin)
extern "C" double iostodroid_compat_sin(double value) { return sin(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_cos)
extern "C" double iostodroid_compat_cos(double value) { return cos(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tan)
extern "C" double iostodroid_compat_tan(double value) { return tan(value); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atan2)
extern "C" double iostodroid_compat_atan2(double y, double x) { return atan2(y, x); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fmod)
extern "C" double iostodroid_compat_fmod(double numerator, double denominator) { return fmod(numerator, denominator); }
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutex_init)
extern "C" int iostodroid_compat_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes) {
    return pthread_mutex_init(mutex, attributes);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutex_lock)
extern "C" int iostodroid_compat_pthread_mutex_lock(pthread_mutex_t *mutex) { return pthread_mutex_lock(mutex); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutex_unlock)
extern "C" int iostodroid_compat_pthread_mutex_unlock(pthread_mutex_t *mutex) { return pthread_mutex_unlock(mutex); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutex_destroy)
extern "C" int iostodroid_compat_pthread_mutex_destroy(pthread_mutex_t *mutex) { return pthread_mutex_destroy(mutex); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_cond_init)
extern "C" int iostodroid_compat_pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes) {
    return pthread_cond_init(condition, attributes);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_cond_wait)
extern "C" int iostodroid_compat_pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    return pthread_cond_wait(condition, mutex);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_cond_signal)
extern "C" int iostodroid_compat_pthread_cond_signal(pthread_cond_t *condition) { return pthread_cond_signal(condition); }
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_cond_broadcast)
extern "C" int iostodroid_compat_pthread_cond_broadcast(pthread_cond_t *condition) {
    return pthread_cond_broadcast(condition);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_cond_destroy)
extern "C" int iostodroid_compat_pthread_cond_destroy(pthread_cond_t *condition) {
    return pthread_cond_destroy(condition);
}
#endif
#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_self)
extern "C" pthread_t iostodroid_compat_pthread_self(void) { return pthread_self(); }
#endif

// ============================================================================
// Expanded iOS / Darwin Compatibility Translation Layer Shims
// ============================================================================
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifdef IOSTODROID_CF_RUNTIME
namespace {

static inline iostodroid_CFStringRef iostodroidCfAllocString(const char *value) {
    auto *obj = new iostodroid_CFRuntime();
    obj->kind = IostodroidCFKind::String;
    obj->retainCount.store(1);
    obj->text = value ? value : "";
    return obj;
}

static inline iostodroid_CFRuntime *iostodroidStaticCfSingleton(const char *value) {
    static std::mutex mutex;
    static std::deque<iostodroid_CFRuntime> pool;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto &item : pool) {
        if (item.text == (value ? value : "")) return &item;
    }
    pool.emplace_back();
    auto &obj = pool.back();
    obj.kind = IostodroidCFKind::String;
    obj.retainCount.store(1000000);
    obj.text = value ? value : "";
    return &obj;
}

static inline iostodroid_CFRuntime *iostodroidMainBundleSingleton() {
    static iostodroid_CFRuntime bundle;
    static const bool init = [] {
        bundle.kind = IostodroidCFKind::Dictionary;
        bundle.retainCount.store(1000000);
        bundle.text = "/bundle";
        return true;
    }();
    (void)init;
    return &bundle;
}

} // namespace
#endif // IOSTODROID_CF_RUNTIME

namespace {

struct IostodroidCompatGlesState {
    std::mutex mutex;
    unsigned int nextId = 1;
    unsigned int boundTexture = 0;
    unsigned int boundBuffer = 0;
    unsigned int boundFramebuffer = 0;
    unsigned int boundRenderbuffer = 0;
    unsigned int currentProgram = 0;
    int viewport[4] = {0, 0, 480, 320};
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    unsigned int activeTexture = 0x84C0u;
    unsigned int clientActiveTexture = 0x84C0u;
    unsigned int matrixMode = 0x1700u;
    unsigned int frontFace = 0x0901u;
    unsigned int blendSrc = 1u;
    unsigned int blendDst = 0u;
    std::uint64_t drawCallCount = 0;
};

static inline IostodroidCompatGlesState &iostodroidGlesState() {
    static IostodroidCompatGlesState state;
    return state;
}

struct IostodroidCompatOpenALState {
    std::mutex mutex;
    unsigned int nextBufferId = 1;
    unsigned int nextSourceId = 1;
    bool contextCurrent = false;
    float listenerGain = 1.0f;
    int distanceModel = 0xD000;
    float dopplerFactor = 1.0f;
    float speedOfSound = 343.3f;
};

static inline IostodroidCompatOpenALState &iostodroidOpenALState() {
    static IostodroidCompatOpenALState state;
    return state;
}

} // namespace

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFConstantStringClassReference)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFConstantStringClassReference(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("___CFConstantStringClassReference");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFAllocatorDefault)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFAllocatorDefault(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_kCFAllocatorDefault");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBooleanTrue)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFBooleanTrue(void) {
    return iostodroidStaticCfSingleton("true");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBooleanFalse)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFBooleanFalse(void) {
    return iostodroidStaticCfSingleton("false");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFTypeArrayCallBacks)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFTypeArrayCallBacks(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_kCFTypeArrayCallBacks");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFTypeDictionaryKeyCallBacks)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFTypeDictionaryKeyCallBacks(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_kCFTypeDictionaryKeyCallBacks");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFTypeDictionaryValueCallBacks)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFTypeDictionaryValueCallBacks(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_kCFTypeDictionaryValueCallBacks");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopDefaultMode)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFRunLoopDefaultMode(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_kCFRunLoopDefaultMode");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFRunLoopCommonModes)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFRunLoopCommonModes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_kCFRunLoopCommonModes");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBundleGetMainBundle)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFBundleGetMainBundle(void) {
    return iostodroidMainBundleSingleton();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBundleCopyBundleURL)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFBundleCopyBundleURL(iostodroid_CFTypeRef bundle) {
    (void)bundle;
    return iostodroidCfAllocString("/bundle");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBundleCopyResourcesDirectoryURL)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFBundleCopyResourcesDirectoryURL(iostodroid_CFTypeRef bundle) {
    (void)bundle;
    return iostodroidCfAllocString("/bundle");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBundleCopyResourceURL)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFBundleCopyResourceURL(iostodroid_CFTypeRef bundle, iostodroid_CFStringRef name, iostodroid_CFStringRef type, iostodroid_CFStringRef subDir) {
    (void)bundle;
    const auto *n = iostodroidCfConst(name);
    const auto *t = iostodroidCfConst(type);
    const auto *s = iostodroidCfConst(subDir);
    std::string path = "/bundle";
    if (s && !s->text.empty()) path += "/" + s->text;
    if (n && !n->text.empty()) path += "/" + n->text;
    if (t && !t->text.empty()) path += "." + t->text;
    return iostodroidCfAllocString(path.c_str());
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBundleGetIdentifier)
extern "C" iostodroid_CFStringRef iostodroid_compat_CFBundleGetIdentifier(iostodroid_CFTypeRef bundle) {
    (void)bundle;
    return iostodroidStaticCfSingleton("com.iostodroid.compat.bundle");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBundleGetValueForInfoDictionaryKey)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFBundleGetValueForInfoDictionaryKey(iostodroid_CFTypeRef bundle, iostodroid_CFStringRef key) {
    (void)bundle;
    const auto *k = iostodroidCfConst(key);
    if (k && k->text == "CFBundleExecutable") return iostodroidStaticCfSingleton("AngryBirds");
    return iostodroidStaticCfSingleton("1.0");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFURLCreateWithFileSystemPath)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFURLCreateWithFileSystemPath(iostodroid_CFAllocatorRef alloc, iostodroid_CFStringRef filePath, iostodroid_CFIndex pathStyle, unsigned char isDir) {
    (void)alloc; (void)pathStyle; (void)isDir;
    const auto *s = iostodroidCfConst(filePath);
    return iostodroidCfAllocString(s ? s->text.c_str() : "/bundle");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFURLCreateFromFileSystemRepresentation)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFURLCreateFromFileSystemRepresentation(iostodroid_CFAllocatorRef alloc, const uint8_t *buffer, iostodroid_CFIndex bufLen, unsigned char isDir) {
    (void)alloc; (void)isDir;
    if (!buffer || bufLen <= 0) return iostodroidCfAllocString("/bundle");
    std::string p(reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(bufLen));
    return iostodroidCfAllocString(p.c_str());
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFURLGetFileSystemRepresentation)
extern "C" unsigned char iostodroid_compat_CFURLGetFileSystemRepresentation(iostodroid_CFTypeRef url, unsigned char resolveAgainstBase, uint8_t *buffer, iostodroid_CFIndex maxBufLen) {
    (void)resolveAgainstBase;
    if (!buffer || maxBufLen <= 0) return 0u;
    const auto *s = iostodroidCfConst(url);
    const std::string &path = (s && !s->text.empty()) ? s->text : std::string("/bundle");
    if (static_cast<iostodroid_CFIndex>(path.size() + 1) > maxBufLen) return 0u;
    std::memcpy(buffer, path.c_str(), path.size() + 1);
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFURLCopyFileSystemPath)
extern "C" iostodroid_CFStringRef iostodroid_compat_CFURLCopyFileSystemPath(iostodroid_CFTypeRef url, iostodroid_CFIndex pathStyle) {
    (void)pathStyle;
    const auto *s = iostodroidCfConst(url);
    return iostodroidCfAllocString(s ? s->text.c_str() : "/bundle");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringCreateWithBytes)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFStringCreateWithBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFStringCreateWithBytes");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringCreateMutable)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFStringCreateMutable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFStringCreateMutable");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringAppendCString)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFStringAppendCString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFStringAppendCString");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringHasPrefix)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFStringHasPrefix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFStringHasPrefix");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringHasSuffix)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFStringHasSuffix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFStringHasSuffix");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringGetIntValue)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFStringGetIntValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFStringGetIntValue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFStringGetDoubleValue)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFStringGetDoubleValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFStringGetDoubleValue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFArrayCreate)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFArrayCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFArrayCreate");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFArrayRemoveValueAtIndex)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFArrayRemoveValueAtIndex(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFArrayRemoveValueAtIndex");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFArrayRemoveAllValues)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFArrayRemoveAllValues(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFArrayRemoveAllValues");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionaryCreate)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDictionaryCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDictionaryCreate");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionaryRemoveValue)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDictionaryRemoveValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDictionaryRemoveValue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionaryRemoveAllValues)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDictionaryRemoveAllValues(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDictionaryRemoveAllValues");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDictionaryContainsKey)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDictionaryContainsKey(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDictionaryContainsKey");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDataCreateMutable)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDataCreateMutable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDataCreateMutable");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDataAppendBytes)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDataAppendBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDataAppendBytes");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDataGetMutableBytePtr)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDataGetMutableBytePtr(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDataGetMutableBytePtr");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFDataGetBytes)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFDataGetBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFDataGetBytes");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFBooleanGetValue)
extern "C" unsigned char iostodroid_compat_CFBooleanGetValue(iostodroid_CFTypeRef booleanRef) {
    const auto *obj = iostodroidCfConst(booleanRef);
    if (!obj) return 0;
    return (obj->text == "true" || obj->text == "1") ? 1u : 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFEqual)
extern "C" unsigned char iostodroid_compat_CFEqual(iostodroid_CFTypeRef a, iostodroid_CFTypeRef b) {
    if (a == b) return a != nullptr ? 1u : 0u;
    const auto *oa = iostodroidCfConst(a);
    const auto *ob = iostodroidCfConst(b);
    if (!oa || !ob || oa->kind != ob->kind) return 0u;
    if (oa->kind == IostodroidCFKind::String) return oa->text == ob->text ? 1u : 0u;
    if (oa->kind == IostodroidCFKind::Data) return oa->bytes == ob->bytes ? 1u : 0u;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFHash)
extern "C" unsigned long iostodroid_compat_CFHash(iostodroid_CFTypeRef cf) {
    const auto *o = iostodroidCfConst(cf);
    if (!o) return 0ul;
    return static_cast<unsigned long>(std::hash<std::string>{}(o->text));
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFGetTypeID)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFGetTypeID(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFGetTypeID");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFPreferencesCopyAppValue)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFPreferencesCopyAppValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFPreferencesCopyAppValue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFPreferencesSetAppValue)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFPreferencesSetAppValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFPreferencesSetAppValue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFPreferencesAppSynchronize)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFPreferencesAppSynchronize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFPreferencesAppSynchronize");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFUUIDCreate)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFUUIDCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFUUIDCreate");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFUUIDCreateString)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFUUIDCreateString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFUUIDCreateString");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFLocaleCopyCurrent)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFLocaleCopyCurrent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFLocaleCopyCurrent");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFLocaleCopyPreferredLanguages)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFLocaleCopyPreferredLanguages(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFLocaleCopyPreferredLanguages");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFLocaleGetIdentifier)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFLocaleGetIdentifier(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFLocaleGetIdentifier");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CFTimeZoneCopySystem)
extern "C" iostodroid_CFTypeRef iostodroid_compat_CFTimeZoneCopySystem(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return iostodroidStaticCfSingleton("_CFTimeZoneCopySystem");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionInitialize)
extern "C" uintptr_t iostodroid_compat_AudioSessionInitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionSetActive)
extern "C" uintptr_t iostodroid_compat_AudioSessionSetActive(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSSearchPathForDirectoriesInDomains)
extern "C" uintptr_t iostodroid_compat_NSSearchPathForDirectoriesInDomains(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_NSSearchPathForDirectoriesInDomains");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___CAEAGLLayer)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___CAEAGLLayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CAEAGLLayer");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___EAGLContext)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___EAGLContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_EAGLContext");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSAutoreleasePool)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSAutoreleasePool(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSAutoreleasePool");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSBundle)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSBundle(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSBundle");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSDictionary)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSDictionary(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSDictionary");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSNumber)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSNumber(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSNumber");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSObject)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSObject(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSObject");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSString)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSString");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSThread)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSThread(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSThread");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSURL)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSURL(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSURL");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIAccelerometer)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIAccelerometer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIAccelerometer");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIApplication)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIApplication(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIApplication");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIScreen)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIScreen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIScreen");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIView)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIView");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIWindow)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIWindow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIWindow");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_METACLASS___NSObject)
extern "C" uintptr_t iostodroid_compat_OBJC_METACLASS___NSObject(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_NSObject");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_METACLASS___UIView)
extern "C" uintptr_t iostodroid_compat_OBJC_METACLASS___UIView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_UIView");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIApplicationMain)
extern "C" uintptr_t iostodroid_compat_UIApplicationMain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__DefaultRuneLocale)
extern "C" uintptr_t iostodroid_compat__DefaultRuneLocale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__DefaultRuneLocale");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_SjLj_Register)
extern "C" uintptr_t iostodroid_compat__Unwind_SjLj_Register(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_SjLj_Resume)
extern "C" uintptr_t iostodroid_compat__Unwind_SjLj_Resume(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_SjLj_Unregister)
extern "C" uintptr_t iostodroid_compat__Unwind_SjLj_Unregister(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__ZSt9terminatev)
extern "C" uintptr_t iostodroid_compat__ZSt9terminatev(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__ZTVN10__cxxabiv117__class_type_infoE)
extern "C" uintptr_t iostodroid_compat__ZTVN10__cxxabiv117__class_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv117__class_type_infoE");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__ZTVN10__cxxabiv119__pointer_type_infoE)
extern "C" uintptr_t iostodroid_compat__ZTVN10__cxxabiv119__pointer_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv119__pointer_type_infoE");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__ZTVN10__cxxabiv120__si_class_type_infoE)
extern "C" uintptr_t iostodroid_compat__ZTVN10__cxxabiv120__si_class_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv120__si_class_type_infoE");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE)
extern "C" uintptr_t iostodroid_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv121__vmi_class_type_infoE");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__ZdaPv)
extern "C" void iostodroid_compat__ZdaPv(void *ptr) {
    std::free(ptr);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__ZdlPv)
extern "C" void iostodroid_compat__ZdlPv(void *ptr) {
    std::free(ptr);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Znam)
extern "C" void *iostodroid_compat__Znam(size_t size) {
    return std::malloc(size == 0 ? 1 : size);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Znwm)
extern "C" void *iostodroid_compat__Znwm(size_t size) {
    return std::malloc(size == 0 ? 1 : size);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_allocate_exception)
extern "C" uintptr_t iostodroid_compat___cxa_allocate_exception(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___cxa_allocate_exception");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_atexit)
extern "C" uintptr_t iostodroid_compat___cxa_atexit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_begin_catch)
extern "C" uintptr_t iostodroid_compat___cxa_begin_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_end_catch)
extern "C" uintptr_t iostodroid_compat___cxa_end_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_pure_virtual)
extern "C" uintptr_t iostodroid_compat___cxa_pure_virtual(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_throw)
extern "C" uintptr_t iostodroid_compat___cxa_throw(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___divdi3)
extern "C" int64_t iostodroid_compat___divdi3(int64_t a, int64_t b) {
    if (b == 0) return 0;
    if (a == INT64_MIN && b == -1) return INT64_MIN;
    return a / b;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___divsi3)
extern "C" int32_t iostodroid_compat___divsi3(int32_t a, int32_t b) {
    if (b == 0) return 0;
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___error)
extern "C" int *iostodroid_compat___error(void) {
    return &errno;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___fixdfdi)
extern "C" int64_t iostodroid_compat___fixdfdi(double a) {
    if (!std::isfinite(a)) return 0;
    return static_cast<int64_t>(a);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___floatdidf)
extern "C" double iostodroid_compat___floatdidf(int64_t a) {
    return static_cast<double>(a);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___floatdisf)
extern "C" float iostodroid_compat___floatdisf(int64_t a) {
    return static_cast<float>(a);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___gxx_personality_sj0)
extern "C" uintptr_t iostodroid_compat___gxx_personality_sj0(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___maskrune)
extern "C" uintptr_t iostodroid_compat___maskrune(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___moddi3)
extern "C" int64_t iostodroid_compat___moddi3(int64_t a, int64_t b) {
    if (b == 0 || (a == INT64_MIN && b == -1)) return 0;
    return a % b;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___modsi3)
extern "C" int32_t iostodroid_compat___modsi3(int32_t a, int32_t b) {
    if (b == 0 || (a == INT32_MIN && b == -1)) return 0;
    return a % b;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___stderrp)
extern "C" uintptr_t iostodroid_compat___stderrp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___stderrp");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___stdinp)
extern "C" uintptr_t iostodroid_compat___stdinp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___stdinp");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___stdoutp)
extern "C" uintptr_t iostodroid_compat___stdoutp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___stdoutp");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___tolower)
extern "C" uintptr_t iostodroid_compat___tolower(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___toupper)
extern "C" uintptr_t iostodroid_compat___toupper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___udivsi3)
extern "C" uint32_t iostodroid_compat___udivsi3(uint32_t a, uint32_t b) {
    return b == 0 ? 0u : (a / b);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___umodsi3)
extern "C" uint32_t iostodroid_compat___umodsi3(uint32_t a, uint32_t b) {
    return b == 0 ? 0u : (a % b);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__objc_empty_cache)
extern "C" uintptr_t iostodroid_compat__objc_empty_cache(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__objc_empty_vtable)
extern "C" uintptr_t iostodroid_compat__objc_empty_vtable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_abort)
extern "C" uintptr_t iostodroid_compat_abort(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_acosf)
extern "C" uintptr_t iostodroid_compat_acosf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alBufferData)
extern "C" uintptr_t iostodroid_compat_alBufferData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alDeleteBuffers)
extern "C" uintptr_t iostodroid_compat_alDeleteBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alDeleteSources)
extern "C" uintptr_t iostodroid_compat_alDeleteSources(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGenBuffers)
extern "C" void iostodroid_compat_alGenBuffers(int n, unsigned int *buffers) {
    if (n <= 0 || !buffers) return;
    auto &st = iostodroidOpenALState();
    std::lock_guard<std::mutex> lock(st.mutex);
    for (int i = 0; i < n; ++i) buffers[i] = st.nextBufferId++;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGenSources)
extern "C" void iostodroid_compat_alGenSources(int n, unsigned int *sources) {
    if (n <= 0 || !sources) return;
    auto &st = iostodroidOpenALState();
    std::lock_guard<std::mutex> lock(st.mutex);
    for (int i = 0; i < n; ++i) sources[i] = st.nextSourceId++;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetSourcef)
extern "C" uintptr_t iostodroid_compat_alGetSourcef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetSourcei)
extern "C" uintptr_t iostodroid_compat_alGetSourcei(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSource3f)
extern "C" uintptr_t iostodroid_compat_alSource3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourcePlay)
extern "C" uintptr_t iostodroid_compat_alSourcePlay(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourceQueueBuffers)
extern "C" uintptr_t iostodroid_compat_alSourceQueueBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourceStop)
extern "C" uintptr_t iostodroid_compat_alSourceStop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourceUnqueueBuffers)
extern "C" uintptr_t iostodroid_compat_alSourceUnqueueBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourcef)
extern "C" uintptr_t iostodroid_compat_alSourcef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourcei)
extern "C" uintptr_t iostodroid_compat_alSourcei(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcCloseDevice)
extern "C" uintptr_t iostodroid_compat_alcCloseDevice(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcCreateContext)
extern "C" void *iostodroid_compat_alcCreateContext(void *device, const int *attrlist) {
    (void)attrlist;
    return device ? device : &iostodroidOpenALState();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcDestroyContext)
extern "C" uintptr_t iostodroid_compat_alcDestroyContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcMakeContextCurrent)
extern "C" char iostodroid_compat_alcMakeContextCurrent(void *context) {
    auto &st = iostodroidOpenALState();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.contextCurrent = (context != nullptr);
    return 1;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcOpenDevice)
extern "C" void *iostodroid_compat_alcOpenDevice(const char *devicename) {
    (void)devicename;
    return &iostodroidOpenALState();
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_asinf)
extern "C" uintptr_t iostodroid_compat_asinf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atan2f)
extern "C" uintptr_t iostodroid_compat_atan2f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atanf)
extern "C" uintptr_t iostodroid_compat_atanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ceilf)
extern "C" uintptr_t iostodroid_compat_ceilf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_clearerr)
extern "C" uintptr_t iostodroid_compat_clearerr(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_clock)
extern "C" uintptr_t iostodroid_compat_clock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_close)
extern "C" uintptr_t iostodroid_compat_close(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_cosf)
extern "C" uintptr_t iostodroid_compat_cosf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_coshf)
extern "C" uintptr_t iostodroid_compat_coshf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_difftime)
extern "C" uintptr_t iostodroid_compat_difftime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_exit)
extern "C" uintptr_t iostodroid_compat_exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_expf)
extern "C" uintptr_t iostodroid_compat_expf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fcntl)
extern "C" uintptr_t iostodroid_compat_fcntl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ferror)
extern "C" uintptr_t iostodroid_compat_ferror(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_floorf)
extern "C" uintptr_t iostodroid_compat_floorf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fputc)
extern "C" uintptr_t iostodroid_compat_fputc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_freopen)
extern "C" uintptr_t iostodroid_compat_freopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_frexp)
extern "C" uintptr_t iostodroid_compat_frexp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fscanf)
extern "C" uintptr_t iostodroid_compat_fscanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getc)
extern "C" uintptr_t iostodroid_compat_getc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glActiveTexture)
extern "C" uintptr_t iostodroid_compat_glActiveTexture(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBindBuffer)
extern "C" uintptr_t iostodroid_compat_glBindBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBindFramebufferOES)
extern "C" uintptr_t iostodroid_compat_glBindFramebufferOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBindRenderbufferOES)
extern "C" uintptr_t iostodroid_compat_glBindRenderbufferOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBindTexture)
extern "C" void iostodroid_compat_glBindTexture(unsigned int target, unsigned int texture) {
    (void)target;
    auto &st = iostodroidGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.boundTexture = texture;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBlendFunc)
extern "C" uintptr_t iostodroid_compat_glBlendFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBufferData)
extern "C" uintptr_t iostodroid_compat_glBufferData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCheckFramebufferStatusOES)
extern "C" unsigned int iostodroid_compat_glCheckFramebufferStatusOES(unsigned int target) {
    (void)target;
    return 0x8CD5u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glClear)
extern "C" uintptr_t iostodroid_compat_glClear(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glClearColor)
extern "C" uintptr_t iostodroid_compat_glClearColor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glClientActiveTexture)
extern "C" uintptr_t iostodroid_compat_glClientActiveTexture(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glColor4f)
extern "C" uintptr_t iostodroid_compat_glColor4f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glColorPointer)
extern "C" uintptr_t iostodroid_compat_glColorPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCompressedTexImage2D)
extern "C" uintptr_t iostodroid_compat_glCompressedTexImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteBuffers)
extern "C" uintptr_t iostodroid_compat_glDeleteBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteFramebuffersOES)
extern "C" uintptr_t iostodroid_compat_glDeleteFramebuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteRenderbuffersOES)
extern "C" uintptr_t iostodroid_compat_glDeleteRenderbuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteTextures)
extern "C" uintptr_t iostodroid_compat_glDeleteTextures(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDepthFunc)
extern "C" uintptr_t iostodroid_compat_glDepthFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDepthMask)
extern "C" uintptr_t iostodroid_compat_glDepthMask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDisable)
extern "C" uintptr_t iostodroid_compat_glDisable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDisableClientState)
extern "C" uintptr_t iostodroid_compat_glDisableClientState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDrawArrays)
extern "C" void iostodroid_compat_glDrawArrays(unsigned int mode, int first, int count) {
    (void)mode; (void)first; (void)count;
    auto &st = iostodroidGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    ++st.drawCallCount;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDrawElements)
extern "C" void iostodroid_compat_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices) {
    (void)mode; (void)count; (void)type; (void)indices;
    auto &st = iostodroidGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    ++st.drawCallCount;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glEnable)
extern "C" uintptr_t iostodroid_compat_glEnable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glEnableClientState)
extern "C" uintptr_t iostodroid_compat_glEnableClientState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFramebufferRenderbufferOES)
extern "C" uintptr_t iostodroid_compat_glFramebufferRenderbufferOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFramebufferTexture2DOES)
extern "C" uintptr_t iostodroid_compat_glFramebufferTexture2DOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFrontFace)
extern "C" void iostodroid_compat_glFrontFace(unsigned int mode) {
    auto &st = iostodroidGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.frontFace = mode;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenBuffers)
extern "C" uintptr_t iostodroid_compat_glGenBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenFramebuffersOES)
extern "C" uintptr_t iostodroid_compat_glGenFramebuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenRenderbuffersOES)
extern "C" uintptr_t iostodroid_compat_glGenRenderbuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenTextures)
extern "C" void iostodroid_compat_glGenTextures(int n, unsigned int *textures) {
    if (n <= 0 || !textures) return;
    auto &st = iostodroidGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    for (int i = 0; i < n; ++i) textures[i] = st.nextId++;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetIntegerv)
extern "C" uintptr_t iostodroid_compat_glGetIntegerv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetRenderbufferParameterivOES)
extern "C" uintptr_t iostodroid_compat_glGetRenderbufferParameterivOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glLightfv)
extern "C" uintptr_t iostodroid_compat_glLightfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glLineWidth)
extern "C" uintptr_t iostodroid_compat_glLineWidth(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glLoadMatrixf)
extern "C" uintptr_t iostodroid_compat_glLoadMatrixf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glMaterialfv)
extern "C" uintptr_t iostodroid_compat_glMaterialfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glMatrixMode)
extern "C" uintptr_t iostodroid_compat_glMatrixMode(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glNormalPointer)
extern "C" uintptr_t iostodroid_compat_glNormalPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glPixelStorei)
extern "C" uintptr_t iostodroid_compat_glPixelStorei(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glRenderbufferStorageOES)
extern "C" uintptr_t iostodroid_compat_glRenderbufferStorageOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glScissor)
extern "C" uintptr_t iostodroid_compat_glScissor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexCoordPointer)
extern "C" uintptr_t iostodroid_compat_glTexCoordPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexEnvi)
extern "C" uintptr_t iostodroid_compat_glTexEnvi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexImage2D)
extern "C" uintptr_t iostodroid_compat_glTexImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexParameteri)
extern "C" uintptr_t iostodroid_compat_glTexParameteri(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexSubImage2D)
extern "C" uintptr_t iostodroid_compat_glTexSubImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glVertexPointer)
extern "C" uintptr_t iostodroid_compat_glVertexPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glViewport)
extern "C" uintptr_t iostodroid_compat_glViewport(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gmtime)
extern "C" uintptr_t iostodroid_compat_gmtime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_kEAGLColorFormatRGB565)
extern "C" uintptr_t iostodroid_compat_kEAGLColorFormatRGB565(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLColorFormatRGB565");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_kEAGLColorFormatRGBA8)
extern "C" uintptr_t iostodroid_compat_kEAGLColorFormatRGBA8(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLColorFormatRGBA8");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_kEAGLDrawablePropertyColorFormat)
extern "C" uintptr_t iostodroid_compat_kEAGLDrawablePropertyColorFormat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLDrawablePropertyColorFormat");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_kEAGLDrawablePropertyRetainedBacking)
extern "C" uintptr_t iostodroid_compat_kEAGLDrawablePropertyRetainedBacking(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLDrawablePropertyRetainedBacking");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ldexp)
extern "C" uintptr_t iostodroid_compat_ldexp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_localeconv)
extern "C" uintptr_t iostodroid_compat_localeconv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_localtime)
extern "C" uintptr_t iostodroid_compat_localtime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_log10f)
extern "C" uintptr_t iostodroid_compat_log10f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_logf)
extern "C" uintptr_t iostodroid_compat_logf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_longjmp)
extern "C" uintptr_t iostodroid_compat_longjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_lseek)
extern "C" uintptr_t iostodroid_compat_lseek(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_modf)
extern "C" uintptr_t iostodroid_compat_modf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_enumerationMutation)
extern "C" uintptr_t iostodroid_compat_objc_enumerationMutation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_msgSend)
extern "C" uintptr_t iostodroid_compat_objc_msgSend(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_msgSendSuper2)
extern "C" uintptr_t iostodroid_compat_objc_msgSendSuper2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_msgSend_stret)
extern "C" uintptr_t iostodroid_compat_objc_msgSend_stret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_setProperty)
extern "C" uintptr_t iostodroid_compat_objc_setProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_create)
extern "C" uintptr_t iostodroid_compat_pthread_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_exit)
extern "C" uintptr_t iostodroid_compat_pthread_exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_getschedparam)
extern "C" uintptr_t iostodroid_compat_pthread_getschedparam(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_join)
extern "C" uintptr_t iostodroid_compat_pthread_join(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutex_trylock)
extern "C" uintptr_t iostodroid_compat_pthread_mutex_trylock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutexattr_destroy)
extern "C" uintptr_t iostodroid_compat_pthread_mutexattr_destroy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutexattr_init)
extern "C" uintptr_t iostodroid_compat_pthread_mutexattr_init(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_mutexattr_settype)
extern "C" uintptr_t iostodroid_compat_pthread_mutexattr_settype(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_setschedparam)
extern "C" uintptr_t iostodroid_compat_pthread_setschedparam(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_read)
extern "C" uintptr_t iostodroid_compat_read(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_rename)
extern "C" uintptr_t iostodroid_compat_rename(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sched_yield)
extern "C" uintptr_t iostodroid_compat_sched_yield(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_select)
extern "C" uintptr_t iostodroid_compat_select(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_setjmp)
extern "C" uintptr_t iostodroid_compat_setjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_setlocale)
extern "C" uintptr_t iostodroid_compat_setlocale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_setvbuf)
extern "C" uintptr_t iostodroid_compat_setvbuf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sinf)
extern "C" uintptr_t iostodroid_compat_sinf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sinhf)
extern "C" uintptr_t iostodroid_compat_sinhf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sprintf)
extern "C" uintptr_t iostodroid_compat_sprintf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strcasecmp)
extern "C" uintptr_t iostodroid_compat_strcasecmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strcat)
extern "C" uintptr_t iostodroid_compat_strcat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strcoll)
extern "C" uintptr_t iostodroid_compat_strcoll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strcspn)
extern "C" uintptr_t iostodroid_compat_strcspn(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strftime)
extern "C" uintptr_t iostodroid_compat_strftime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strncat)
extern "C" uintptr_t iostodroid_compat_strncat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strpbrk)
extern "C" uintptr_t iostodroid_compat_strpbrk(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtok)
extern "C" uintptr_t iostodroid_compat_strtok(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtoul)
extern "C" uintptr_t iostodroid_compat_strtoul(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_system)
extern "C" uintptr_t iostodroid_compat_system(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tanf)
extern "C" uintptr_t iostodroid_compat_tanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tanhf)
extern "C" uintptr_t iostodroid_compat_tanhf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tmpfile)
extern "C" uintptr_t iostodroid_compat_tmpfile(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tmpnam)
extern "C" uintptr_t iostodroid_compat_tmpnam(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ungetc)
extern "C" uintptr_t iostodroid_compat_ungetc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_usleep)
extern "C" uintptr_t iostodroid_compat_usleep(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_vsprintf)
extern "C" uintptr_t iostodroid_compat_vsprintf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__exit)
extern "C" uintptr_t iostodroid_compat__exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atexit)
extern "C" uintptr_t iostodroid_compat_atexit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sscanf)
extern "C" uintptr_t iostodroid_compat_sscanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_putchar)
extern "C" uintptr_t iostodroid_compat_putchar(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getchar)
extern "C" uintptr_t iostodroid_compat_getchar(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fgetc)
extern "C" uintptr_t iostodroid_compat_fgetc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_putc)
extern "C" uintptr_t iostodroid_compat_putc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_rewind)
extern "C" uintptr_t iostodroid_compat_rewind(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fileno)
extern "C" uintptr_t iostodroid_compat_fileno(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fdopen)
extern "C" uintptr_t iostodroid_compat_fdopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_perror)
extern "C" uintptr_t iostodroid_compat_perror(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tzset)
extern "C" uintptr_t iostodroid_compat_tzset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sleep)
extern "C" uintptr_t iostodroid_compat_sleep(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_open)
extern "C" uintptr_t iostodroid_compat_open(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_write)
extern "C" uintptr_t iostodroid_compat_write(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_unlink)
extern "C" uintptr_t iostodroid_compat_unlink(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_mkdir)
extern "C" uintptr_t iostodroid_compat_mkdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_rmdir)
extern "C" uintptr_t iostodroid_compat_rmdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_access)
extern "C" uintptr_t iostodroid_compat_access(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getcwd)
extern "C" uintptr_t iostodroid_compat_getcwd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_chdir)
extern "C" uintptr_t iostodroid_compat_chdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_stat)
extern "C" uintptr_t iostodroid_compat_stat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fstat)
extern "C" uintptr_t iostodroid_compat_fstat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_lstat)
extern "C" uintptr_t iostodroid_compat_lstat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_opendir)
extern "C" uintptr_t iostodroid_compat_opendir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_readdir)
extern "C" uintptr_t iostodroid_compat_readdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_closedir)
extern "C" uintptr_t iostodroid_compat_closedir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_mmap)
extern "C" uintptr_t iostodroid_compat_mmap(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_munmap)
extern "C" uintptr_t iostodroid_compat_munmap(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_mprotect)
extern "C" uintptr_t iostodroid_compat_mprotect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_poll)
extern "C" uintptr_t iostodroid_compat_poll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pipe)
extern "C" uintptr_t iostodroid_compat_pipe(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dup)
extern "C" uintptr_t iostodroid_compat_dup(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dup2)
extern "C" uintptr_t iostodroid_compat_dup2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fsync)
extern "C" uintptr_t iostodroid_compat_fsync(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ftruncate)
extern "C" uintptr_t iostodroid_compat_ftruncate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_truncate)
extern "C" uintptr_t iostodroid_compat_truncate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_chmod)
extern "C" uintptr_t iostodroid_compat_chmod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_umask)
extern "C" uintptr_t iostodroid_compat_umask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getuid)
extern "C" uintptr_t iostodroid_compat_getuid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_geteuid)
extern "C" uintptr_t iostodroid_compat_geteuid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getgid)
extern "C" uintptr_t iostodroid_compat_getgid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getegid)
extern "C" uintptr_t iostodroid_compat_getegid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getppid)
extern "C" uintptr_t iostodroid_compat_getppid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sysconf)
extern "C" uintptr_t iostodroid_compat_sysconf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sysctl)
extern "C" uintptr_t iostodroid_compat_sysctl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sysctlbyname)
extern "C" uintptr_t iostodroid_compat_sysctlbyname(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getpagesize)
extern "C" uintptr_t iostodroid_compat_getpagesize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__setjmp)
extern "C" uintptr_t iostodroid_compat__setjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__longjmp)
extern "C" uintptr_t iostodroid_compat__longjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sigaction)
extern "C" uintptr_t iostodroid_compat_sigaction(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_signal)
extern "C" uintptr_t iostodroid_compat_signal(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_raise)
extern "C" uintptr_t iostodroid_compat_raise(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_kill)
extern "C" uintptr_t iostodroid_compat_kill(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tolower)
extern "C" uintptr_t iostodroid_compat_tolower(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_toupper)
extern "C" uintptr_t iostodroid_compat_toupper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_isalpha)
extern "C" uintptr_t iostodroid_compat_isalpha(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_isdigit)
extern "C" uintptr_t iostodroid_compat_isdigit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_isalnum)
extern "C" uintptr_t iostodroid_compat_isalnum(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_isspace)
extern "C" uintptr_t iostodroid_compat_isspace(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_isupper)
extern "C" uintptr_t iostodroid_compat_isupper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_islower)
extern "C" uintptr_t iostodroid_compat_islower(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_isxdigit)
extern "C" uintptr_t iostodroid_compat_isxdigit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strncasecmp)
extern "C" uintptr_t iostodroid_compat_strncasecmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strspn)
extern "C" uintptr_t iostodroid_compat_strspn(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtok_r)
extern "C" uintptr_t iostodroid_compat_strtok_r(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtoll)
extern "C" uintptr_t iostodroid_compat_strtoll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtoull)
extern "C" uintptr_t iostodroid_compat_strtoull(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_strtof)
extern "C" uintptr_t iostodroid_compat_strtof(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atol)
extern "C" uintptr_t iostodroid_compat_atol(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atoll)
extern "C" uintptr_t iostodroid_compat_atoll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_llabs)
extern "C" uintptr_t iostodroid_compat_llabs(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_bzero)
extern "C" uintptr_t iostodroid_compat_bzero(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_bcopy)
extern "C" uintptr_t iostodroid_compat_bcopy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_bcmp)
extern "C" uintptr_t iostodroid_compat_bcmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_acos)
extern "C" uintptr_t iostodroid_compat_acos(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_asin)
extern "C" uintptr_t iostodroid_compat_asin(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_atan)
extern "C" uintptr_t iostodroid_compat_atan(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_cosh)
extern "C" uintptr_t iostodroid_compat_cosh(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sinh)
extern "C" uintptr_t iostodroid_compat_sinh(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_tanh)
extern "C" uintptr_t iostodroid_compat_tanh(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_exp)
extern "C" uintptr_t iostodroid_compat_exp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_log)
extern "C" uintptr_t iostodroid_compat_log(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_log10)
extern "C" uintptr_t iostodroid_compat_log10(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_log2)
extern "C" uintptr_t iostodroid_compat_log2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_hypot)
extern "C" uintptr_t iostodroid_compat_hypot(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_hypotf)
extern "C" uintptr_t iostodroid_compat_hypotf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_cbrt)
extern "C" uintptr_t iostodroid_compat_cbrt(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_round)
extern "C" uintptr_t iostodroid_compat_round(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_roundf)
extern "C" uintptr_t iostodroid_compat_roundf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_trunc)
extern "C" uintptr_t iostodroid_compat_trunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_truncf)
extern "C" uintptr_t iostodroid_compat_truncf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_lround)
extern "C" uintptr_t iostodroid_compat_lround(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_lroundf)
extern "C" uintptr_t iostodroid_compat_lroundf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_frexpf)
extern "C" uintptr_t iostodroid_compat_frexpf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ldexpf)
extern "C" uintptr_t iostodroid_compat_ldexpf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_log2f)
extern "C" uintptr_t iostodroid_compat_log2f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_modff)
extern "C" uintptr_t iostodroid_compat_modff(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_powf)
extern "C" uintptr_t iostodroid_compat_powf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sqrtf)
extern "C" uintptr_t iostodroid_compat_sqrtf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fabsf)
extern "C" uintptr_t iostodroid_compat_fabsf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_fmodf)
extern "C" uintptr_t iostodroid_compat_fmodf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_detach)
extern "C" uintptr_t iostodroid_compat_pthread_detach(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_equal)
extern "C" uintptr_t iostodroid_compat_pthread_equal(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_once)
extern "C" uintptr_t iostodroid_compat_pthread_once(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_cond_timedwait)
extern "C" uintptr_t iostodroid_compat_pthread_cond_timedwait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_key_create)
extern "C" uintptr_t iostodroid_compat_pthread_key_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_key_delete)
extern "C" uintptr_t iostodroid_compat_pthread_key_delete(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_setspecific)
extern "C" uintptr_t iostodroid_compat_pthread_setspecific(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_getspecific)
extern "C" uintptr_t iostodroid_compat_pthread_getspecific(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_rwlock_init)
extern "C" uintptr_t iostodroid_compat_pthread_rwlock_init(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_rwlock_rdlock)
extern "C" uintptr_t iostodroid_compat_pthread_rwlock_rdlock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_rwlock_wrlock)
extern "C" uintptr_t iostodroid_compat_pthread_rwlock_wrlock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_rwlock_unlock)
extern "C" uintptr_t iostodroid_compat_pthread_rwlock_unlock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_pthread_rwlock_destroy)
extern "C" uintptr_t iostodroid_compat_pthread_rwlock_destroy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sem_init)
extern "C" uintptr_t iostodroid_compat_sem_init(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sem_destroy)
extern "C" uintptr_t iostodroid_compat_sem_destroy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sem_wait)
extern "C" uintptr_t iostodroid_compat_sem_wait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sem_trywait)
extern "C" uintptr_t iostodroid_compat_sem_trywait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sem_post)
extern "C" uintptr_t iostodroid_compat_sem_post(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dlopen)
extern "C" uintptr_t iostodroid_compat_dlopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dlsym)
extern "C" uintptr_t iostodroid_compat_dlsym(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dlclose)
extern "C" uintptr_t iostodroid_compat_dlclose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dlerror)
extern "C" uintptr_t iostodroid_compat_dlerror(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_socket)
extern "C" uintptr_t iostodroid_compat_socket(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_connect)
extern "C" uintptr_t iostodroid_compat_connect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_bind)
extern "C" uintptr_t iostodroid_compat_bind(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_listen)
extern "C" uintptr_t iostodroid_compat_listen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_accept)
extern "C" uintptr_t iostodroid_compat_accept(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_send)
extern "C" uintptr_t iostodroid_compat_send(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sendto)
extern "C" uintptr_t iostodroid_compat_sendto(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_recv)
extern "C" uintptr_t iostodroid_compat_recv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_recvfrom)
extern "C" uintptr_t iostodroid_compat_recvfrom(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_setsockopt)
extern "C" uintptr_t iostodroid_compat_setsockopt(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getsockopt)
extern "C" uintptr_t iostodroid_compat_getsockopt(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getsockname)
extern "C" uintptr_t iostodroid_compat_getsockname(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getpeername)
extern "C" uintptr_t iostodroid_compat_getpeername(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_shutdown)
extern "C" uintptr_t iostodroid_compat_shutdown(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_getaddrinfo)
extern "C" uintptr_t iostodroid_compat_getaddrinfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_freeaddrinfo)
extern "C" uintptr_t iostodroid_compat_freeaddrinfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gethostbyname)
extern "C" uintptr_t iostodroid_compat_gethostbyname(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inet_ntop)
extern "C" uintptr_t iostodroid_compat_inet_ntop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inet_pton)
extern "C" uintptr_t iostodroid_compat_inet_pton(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inet_addr)
extern "C" uintptr_t iostodroid_compat_inet_addr(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inet_ntoa)
extern "C" uintptr_t iostodroid_compat_inet_ntoa(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_htons)
extern "C" uintptr_t iostodroid_compat_htons(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_htonl)
extern "C" uintptr_t iostodroid_compat_htonl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ntohs)
extern "C" uintptr_t iostodroid_compat_ntohs(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ntohl)
extern "C" uintptr_t iostodroid_compat_ntohl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_crc32)
extern "C" uintptr_t iostodroid_compat_crc32(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_adler32)
extern "C" uintptr_t iostodroid_compat_adler32(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_compress)
extern "C" uintptr_t iostodroid_compat_compress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_compress2)
extern "C" uintptr_t iostodroid_compat_compress2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_uncompress)
extern "C" uintptr_t iostodroid_compat_uncompress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_deflateInit_)
extern "C" uintptr_t iostodroid_compat_deflateInit_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_deflateInit2_)
extern "C" uintptr_t iostodroid_compat_deflateInit2_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_deflate)
extern "C" uintptr_t iostodroid_compat_deflate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_deflateEnd)
extern "C" uintptr_t iostodroid_compat_deflateEnd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_deflateReset)
extern "C" uintptr_t iostodroid_compat_deflateReset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inflateInit_)
extern "C" uintptr_t iostodroid_compat_inflateInit_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inflateInit2_)
extern "C" uintptr_t iostodroid_compat_inflateInit2_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inflate)
extern "C" uintptr_t iostodroid_compat_inflate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inflateEnd)
extern "C" uintptr_t iostodroid_compat_inflateEnd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_inflateReset)
extern "C" uintptr_t iostodroid_compat_inflateReset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gzopen)
extern "C" uintptr_t iostodroid_compat_gzopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gzread)
extern "C" uintptr_t iostodroid_compat_gzread(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gzwrite)
extern "C" uintptr_t iostodroid_compat_gzwrite(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_gzclose)
extern "C" uintptr_t iostodroid_compat_gzclose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alDistanceModel)
extern "C" uintptr_t iostodroid_compat_alDistanceModel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alDopplerFactor)
extern "C" uintptr_t iostodroid_compat_alDopplerFactor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alDopplerVelocity)
extern "C" uintptr_t iostodroid_compat_alDopplerVelocity(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSpeedOfSound)
extern "C" uintptr_t iostodroid_compat_alSpeedOfSound(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetError)
extern "C" uintptr_t iostodroid_compat_alGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetSource3f)
extern "C" uintptr_t iostodroid_compat_alGetSource3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetSourcefv)
extern "C" uintptr_t iostodroid_compat_alGetSourcefv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourcefv)
extern "C" uintptr_t iostodroid_compat_alSourcefv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourcePause)
extern "C" uintptr_t iostodroid_compat_alSourcePause(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alSourceRewind)
extern "C" uintptr_t iostodroid_compat_alSourceRewind(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alListener3f)
extern "C" uintptr_t iostodroid_compat_alListener3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alListenerf)
extern "C" uintptr_t iostodroid_compat_alListenerf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alListenerfv)
extern "C" uintptr_t iostodroid_compat_alListenerfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alListeneri)
extern "C" uintptr_t iostodroid_compat_alListeneri(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetListenerf)
extern "C" uintptr_t iostodroid_compat_alGetListenerf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetListener3f)
extern "C" uintptr_t iostodroid_compat_alGetListener3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetListenerfv)
extern "C" uintptr_t iostodroid_compat_alGetListenerfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alEnable)
extern "C" uintptr_t iostodroid_compat_alEnable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alDisable)
extern "C" uintptr_t iostodroid_compat_alDisable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alIsEnabled)
extern "C" uintptr_t iostodroid_compat_alIsEnabled(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alIsBuffer)
extern "C" uintptr_t iostodroid_compat_alIsBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alIsSource)
extern "C" uintptr_t iostodroid_compat_alIsSource(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetBoolean)
extern "C" uintptr_t iostodroid_compat_alGetBoolean(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetInteger)
extern "C" uintptr_t iostodroid_compat_alGetInteger(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetFloat)
extern "C" uintptr_t iostodroid_compat_alGetFloat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetDouble)
extern "C" uintptr_t iostodroid_compat_alGetDouble(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetString)
extern "C" uintptr_t iostodroid_compat_alGetString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetEnumValue)
extern "C" uintptr_t iostodroid_compat_alGetEnumValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alGetProcAddress)
extern "C" uintptr_t iostodroid_compat_alGetProcAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alIsExtensionPresent)
extern "C" uintptr_t iostodroid_compat_alIsExtensionPresent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcGetContextsDevice)
extern "C" uintptr_t iostodroid_compat_alcGetContextsDevice(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcGetCurrentContext)
extern "C" uintptr_t iostodroid_compat_alcGetCurrentContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcProcessContext)
extern "C" uintptr_t iostodroid_compat_alcProcessContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcSuspendContext)
extern "C" uintptr_t iostodroid_compat_alcSuspendContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcGetError)
extern "C" uintptr_t iostodroid_compat_alcGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcGetIntegerv)
extern "C" uintptr_t iostodroid_compat_alcGetIntegerv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcGetString)
extern "C" uintptr_t iostodroid_compat_alcGetString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcIsExtensionPresent)
extern "C" uintptr_t iostodroid_compat_alcIsExtensionPresent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_alcGetProcAddress)
extern "C" uintptr_t iostodroid_compat_alcGetProcAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionSetActiveWithFlags)
extern "C" uintptr_t iostodroid_compat_AudioSessionSetActiveWithFlags(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionGetProperty)
extern "C" uintptr_t iostodroid_compat_AudioSessionGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionSetProperty)
extern "C" uintptr_t iostodroid_compat_AudioSessionSetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionGetPropertySize)
extern "C" uintptr_t iostodroid_compat_AudioSessionGetPropertySize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionAddPropertyListener)
extern "C" uintptr_t iostodroid_compat_AudioSessionAddPropertyListener(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioSessionRemovePropertyListenerWithUserData)
extern "C" uintptr_t iostodroid_compat_AudioSessionRemovePropertyListenerWithUserData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioServicesPlaySystemSound)
extern "C" uintptr_t iostodroid_compat_AudioServicesPlaySystemSound(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioServicesPlayAlertSound)
extern "C" uintptr_t iostodroid_compat_AudioServicesPlayAlertSound(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioServicesCreateSystemSoundID)
extern "C" uintptr_t iostodroid_compat_AudioServicesCreateSystemSoundID(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioServicesDisposeSystemSoundID)
extern "C" uintptr_t iostodroid_compat_AudioServicesDisposeSystemSoundID(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioFileOpenURL)
extern "C" uintptr_t iostodroid_compat_AudioFileOpenURL(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioFileClose)
extern "C" uintptr_t iostodroid_compat_AudioFileClose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioFileGetProperty)
extern "C" uintptr_t iostodroid_compat_AudioFileGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioFileReadBytes)
extern "C" uintptr_t iostodroid_compat_AudioFileReadBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioFileReadPackets)
extern "C" uintptr_t iostodroid_compat_AudioFileReadPackets(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ExtAudioFileOpenURL)
extern "C" uintptr_t iostodroid_compat_ExtAudioFileOpenURL(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ExtAudioFileDispose)
extern "C" uintptr_t iostodroid_compat_ExtAudioFileDispose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ExtAudioFileGetProperty)
extern "C" uintptr_t iostodroid_compat_ExtAudioFileGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ExtAudioFileSetProperty)
extern "C" uintptr_t iostodroid_compat_ExtAudioFileSetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ExtAudioFileRead)
extern "C" uintptr_t iostodroid_compat_ExtAudioFileRead(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_ExtAudioFileSeek)
extern "C" uintptr_t iostodroid_compat_ExtAudioFileSeek(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueNewOutput)
extern "C" uintptr_t iostodroid_compat_AudioQueueNewOutput(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueAllocateBuffer)
extern "C" uintptr_t iostodroid_compat_AudioQueueAllocateBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueFreeBuffer)
extern "C" uintptr_t iostodroid_compat_AudioQueueFreeBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueEnqueueBuffer)
extern "C" uintptr_t iostodroid_compat_AudioQueueEnqueueBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueStart)
extern "C" uintptr_t iostodroid_compat_AudioQueueStart(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueuePause)
extern "C" uintptr_t iostodroid_compat_AudioQueuePause(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueStop)
extern "C" uintptr_t iostodroid_compat_AudioQueueStop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueDispose)
extern "C" uintptr_t iostodroid_compat_AudioQueueDispose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioQueueSetParameter)
extern "C" uintptr_t iostodroid_compat_AudioQueueSetParameter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioComponentFindNext)
extern "C" uintptr_t iostodroid_compat_AudioComponentFindNext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioComponentInstanceNew)
extern "C" uintptr_t iostodroid_compat_AudioComponentInstanceNew(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioComponentInstanceDispose)
extern "C" uintptr_t iostodroid_compat_AudioComponentInstanceDispose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioUnitInitialize)
extern "C" uintptr_t iostodroid_compat_AudioUnitInitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioUnitUninitialize)
extern "C" uintptr_t iostodroid_compat_AudioUnitUninitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioUnitSetProperty)
extern "C" uintptr_t iostodroid_compat_AudioUnitSetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioUnitGetProperty)
extern "C" uintptr_t iostodroid_compat_AudioUnitGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioOutputUnitStart)
extern "C" uintptr_t iostodroid_compat_AudioOutputUnitStart(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioOutputUnitStop)
extern "C" uintptr_t iostodroid_compat_AudioOutputUnitStop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_AudioUnitRender)
extern "C" uintptr_t iostodroid_compat_AudioUnitRender(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glAlphaFunc)
extern "C" uintptr_t iostodroid_compat_glAlphaFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBindFramebuffer)
extern "C" uintptr_t iostodroid_compat_glBindFramebuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBindRenderbuffer)
extern "C" uintptr_t iostodroid_compat_glBindRenderbuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBlendEquation)
extern "C" uintptr_t iostodroid_compat_glBlendEquation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBlendEquationOES)
extern "C" uintptr_t iostodroid_compat_glBlendEquationOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBlendFuncSeparate)
extern "C" uintptr_t iostodroid_compat_glBlendFuncSeparate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glBufferSubData)
extern "C" uintptr_t iostodroid_compat_glBufferSubData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCheckFramebufferStatus)
extern "C" unsigned int iostodroid_compat_glCheckFramebufferStatus(unsigned int target) {
    (void)target;
    return 0x8CD5u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glClearDepthf)
extern "C" uintptr_t iostodroid_compat_glClearDepthf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glClearStencil)
extern "C" uintptr_t iostodroid_compat_glClearStencil(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glColor4ub)
extern "C" uintptr_t iostodroid_compat_glColor4ub(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glColorMask)
extern "C" uintptr_t iostodroid_compat_glColorMask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCompileShader)
extern "C" uintptr_t iostodroid_compat_glCompileShader(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCopyTexImage2D)
extern "C" uintptr_t iostodroid_compat_glCopyTexImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCopyTexSubImage2D)
extern "C" uintptr_t iostodroid_compat_glCopyTexSubImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCreateProgram)
extern "C" uintptr_t iostodroid_compat_glCreateProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCreateShader)
extern "C" uintptr_t iostodroid_compat_glCreateShader(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glCullFace)
extern "C" uintptr_t iostodroid_compat_glCullFace(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteFramebuffers)
extern "C" uintptr_t iostodroid_compat_glDeleteFramebuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteProgram)
extern "C" uintptr_t iostodroid_compat_glDeleteProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteRenderbuffers)
extern "C" uintptr_t iostodroid_compat_glDeleteRenderbuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDeleteShader)
extern "C" uintptr_t iostodroid_compat_glDeleteShader(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDepthRangef)
extern "C" uintptr_t iostodroid_compat_glDepthRangef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glDisableVertexAttribArray)
extern "C" uintptr_t iostodroid_compat_glDisableVertexAttribArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glEnableVertexAttribArray)
extern "C" uintptr_t iostodroid_compat_glEnableVertexAttribArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFinish)
extern "C" uintptr_t iostodroid_compat_glFinish(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFlush)
extern "C" uintptr_t iostodroid_compat_glFlush(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFogf)
extern "C" uintptr_t iostodroid_compat_glFogf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFogfv)
extern "C" uintptr_t iostodroid_compat_glFogfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFramebufferRenderbuffer)
extern "C" uintptr_t iostodroid_compat_glFramebufferRenderbuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFramebufferTexture2D)
extern "C" uintptr_t iostodroid_compat_glFramebufferTexture2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glFrustumf)
extern "C" uintptr_t iostodroid_compat_glFrustumf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenFramebuffers)
extern "C" uintptr_t iostodroid_compat_glGenFramebuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenRenderbuffers)
extern "C" uintptr_t iostodroid_compat_glGenRenderbuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenerateMipmap)
extern "C" uintptr_t iostodroid_compat_glGenerateMipmap(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGenerateMipmapOES)
extern "C" uintptr_t iostodroid_compat_glGenerateMipmapOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetAttribLocation)
extern "C" uintptr_t iostodroid_compat_glGetAttribLocation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetError)
extern "C" uintptr_t iostodroid_compat_glGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetFloatv)
extern "C" uintptr_t iostodroid_compat_glGetFloatv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetProgramInfoLog)
extern "C" uintptr_t iostodroid_compat_glGetProgramInfoLog(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetProgramiv)
extern "C" uintptr_t iostodroid_compat_glGetProgramiv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetRenderbufferParameteriv)
extern "C" uintptr_t iostodroid_compat_glGetRenderbufferParameteriv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetShaderInfoLog)
extern "C" uintptr_t iostodroid_compat_glGetShaderInfoLog(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetShaderiv)
extern "C" uintptr_t iostodroid_compat_glGetShaderiv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetString)
extern "C" uintptr_t iostodroid_compat_glGetString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glGetUniformLocation)
extern "C" uintptr_t iostodroid_compat_glGetUniformLocation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glHint)
extern "C" uintptr_t iostodroid_compat_glHint(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glIsEnabled)
extern "C" uintptr_t iostodroid_compat_glIsEnabled(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glIsTexture)
extern "C" uintptr_t iostodroid_compat_glIsTexture(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glLightModelfv)
extern "C" uintptr_t iostodroid_compat_glLightModelfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glLinkProgram)
extern "C" uintptr_t iostodroid_compat_glLinkProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glLoadIdentity)
extern "C" uintptr_t iostodroid_compat_glLoadIdentity(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glLogicOp)
extern "C" uintptr_t iostodroid_compat_glLogicOp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glMaterialf)
extern "C" uintptr_t iostodroid_compat_glMaterialf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glMultMatrixf)
extern "C" uintptr_t iostodroid_compat_glMultMatrixf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glNormal3f)
extern "C" uintptr_t iostodroid_compat_glNormal3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glOrthof)
extern "C" uintptr_t iostodroid_compat_glOrthof(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glPointParameterf)
extern "C" uintptr_t iostodroid_compat_glPointParameterf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glPointParameterfv)
extern "C" uintptr_t iostodroid_compat_glPointParameterfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glPointSize)
extern "C" uintptr_t iostodroid_compat_glPointSize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glPolygonOffset)
extern "C" uintptr_t iostodroid_compat_glPolygonOffset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glPopMatrix)
extern "C" uintptr_t iostodroid_compat_glPopMatrix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glPushMatrix)
extern "C" uintptr_t iostodroid_compat_glPushMatrix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glReadPixels)
extern "C" uintptr_t iostodroid_compat_glReadPixels(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glRenderbufferStorage)
extern "C" uintptr_t iostodroid_compat_glRenderbufferStorage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glRotatef)
extern "C" uintptr_t iostodroid_compat_glRotatef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glScalef)
extern "C" uintptr_t iostodroid_compat_glScalef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glShadeModel)
extern "C" uintptr_t iostodroid_compat_glShadeModel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glShaderSource)
extern "C" uintptr_t iostodroid_compat_glShaderSource(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glStencilFunc)
extern "C" uintptr_t iostodroid_compat_glStencilFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glStencilMask)
extern "C" uintptr_t iostodroid_compat_glStencilMask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glStencilOp)
extern "C" uintptr_t iostodroid_compat_glStencilOp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexEnvf)
extern "C" uintptr_t iostodroid_compat_glTexEnvf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexEnvfv)
extern "C" uintptr_t iostodroid_compat_glTexEnvfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexParameterf)
extern "C" uintptr_t iostodroid_compat_glTexParameterf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTexParameterfv)
extern "C" uintptr_t iostodroid_compat_glTexParameterfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glTranslatef)
extern "C" uintptr_t iostodroid_compat_glTranslatef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glUniform1f)
extern "C" uintptr_t iostodroid_compat_glUniform1f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glUniform1i)
extern "C" uintptr_t iostodroid_compat_glUniform1i(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glUniform2f)
extern "C" uintptr_t iostodroid_compat_glUniform2f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glUniform3f)
extern "C" uintptr_t iostodroid_compat_glUniform3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glUniform4f)
extern "C" uintptr_t iostodroid_compat_glUniform4f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glUniformMatrix4fv)
extern "C" uintptr_t iostodroid_compat_glUniformMatrix4fv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glUseProgram)
extern "C" uintptr_t iostodroid_compat_glUseProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_glVertexAttribPointer)
extern "C" uintptr_t iostodroid_compat_glVertexAttribPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglGetDisplay)
extern "C" uintptr_t iostodroid_compat_eglGetDisplay(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_eglGetDisplay");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglInitialize)
extern "C" uintptr_t iostodroid_compat_eglInitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglChooseConfig)
extern "C" uintptr_t iostodroid_compat_eglChooseConfig(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglCreateWindowSurface)
extern "C" uintptr_t iostodroid_compat_eglCreateWindowSurface(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_eglCreateWindowSurface");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglCreateContext)
extern "C" uintptr_t iostodroid_compat_eglCreateContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_eglCreateContext");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglMakeCurrent)
extern "C" uintptr_t iostodroid_compat_eglMakeCurrent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglSwapBuffers)
extern "C" uintptr_t iostodroid_compat_eglSwapBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglDestroyContext)
extern "C" uintptr_t iostodroid_compat_eglDestroyContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglDestroySurface)
extern "C" uintptr_t iostodroid_compat_eglDestroySurface(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglTerminate)
extern "C" uintptr_t iostodroid_compat_eglTerminate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglGetError)
extern "C" uintptr_t iostodroid_compat_eglGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_eglGetProcAddress)
extern "C" uintptr_t iostodroid_compat_eglGetProcAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGColorSpaceCreateDeviceRGB)
extern "C" uintptr_t iostodroid_compat_CGColorSpaceCreateDeviceRGB(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGColorSpaceCreateDeviceRGB");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGColorSpaceCreateDeviceGray)
extern "C" uintptr_t iostodroid_compat_CGColorSpaceCreateDeviceGray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGColorSpaceCreateDeviceGray");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGColorSpaceRelease)
extern "C" uintptr_t iostodroid_compat_CGColorSpaceRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGColorSpaceRetain)
extern "C" uintptr_t iostodroid_compat_CGColorSpaceRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGBitmapContextCreate)
extern "C" uintptr_t iostodroid_compat_CGBitmapContextCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGBitmapContextCreate");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGBitmapContextGetData)
extern "C" uintptr_t iostodroid_compat_CGBitmapContextGetData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGBitmapContextGetWidth)
extern "C" uintptr_t iostodroid_compat_CGBitmapContextGetWidth(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 480u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGBitmapContextGetHeight)
extern "C" uintptr_t iostodroid_compat_CGBitmapContextGetHeight(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 320u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGBitmapContextGetBytesPerRow)
extern "C" uintptr_t iostodroid_compat_CGBitmapContextGetBytesPerRow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1920u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGBitmapContextCreateImage)
extern "C" uintptr_t iostodroid_compat_CGBitmapContextCreateImage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGBitmapContextCreateImage");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextRelease)
extern "C" uintptr_t iostodroid_compat_CGContextRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextRetain)
extern "C" uintptr_t iostodroid_compat_CGContextRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextClearRect)
extern "C" uintptr_t iostodroid_compat_CGContextClearRect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextFillRect)
extern "C" uintptr_t iostodroid_compat_CGContextFillRect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextDrawImage)
extern "C" uintptr_t iostodroid_compat_CGContextDrawImage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextTranslateCTM)
extern "C" uintptr_t iostodroid_compat_CGContextTranslateCTM(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextScaleCTM)
extern "C" uintptr_t iostodroid_compat_CGContextScaleCTM(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextRotateCTM)
extern "C" uintptr_t iostodroid_compat_CGContextRotateCTM(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextSaveGState)
extern "C" uintptr_t iostodroid_compat_CGContextSaveGState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextRestoreGState)
extern "C" uintptr_t iostodroid_compat_CGContextRestoreGState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextSetRGBFillColor)
extern "C" uintptr_t iostodroid_compat_CGContextSetRGBFillColor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGContextSetAlpha)
extern "C" uintptr_t iostodroid_compat_CGContextSetAlpha(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetWidth)
extern "C" uintptr_t iostodroid_compat_CGImageGetWidth(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 480u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetHeight)
extern "C" uintptr_t iostodroid_compat_CGImageGetHeight(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 320u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetBitsPerComponent)
extern "C" uintptr_t iostodroid_compat_CGImageGetBitsPerComponent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 8u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetBitsPerPixel)
extern "C" uintptr_t iostodroid_compat_CGImageGetBitsPerPixel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 32u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetBytesPerRow)
extern "C" uintptr_t iostodroid_compat_CGImageGetBytesPerRow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1920u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetAlphaInfo)
extern "C" uintptr_t iostodroid_compat_CGImageGetAlphaInfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetDataProvider)
extern "C" uintptr_t iostodroid_compat_CGImageGetDataProvider(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageGetColorSpace)
extern "C" uintptr_t iostodroid_compat_CGImageGetColorSpace(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageRelease)
extern "C" uintptr_t iostodroid_compat_CGImageRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGImageRetain)
extern "C" uintptr_t iostodroid_compat_CGImageRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGDataProviderCopyData)
extern "C" uintptr_t iostodroid_compat_CGDataProviderCopyData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGDataProviderCopyData");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGDataProviderCreateWithData)
extern "C" uintptr_t iostodroid_compat_CGDataProviderCreateWithData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGDataProviderCreateWithData");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGDataProviderRelease)
extern "C" uintptr_t iostodroid_compat_CGDataProviderRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGDataProviderRetain)
extern "C" uintptr_t iostodroid_compat_CGDataProviderRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformMake)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformMake(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformMakeTranslation)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformMakeTranslation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformMakeScale)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformMakeScale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformMakeRotation)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformMakeRotation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformTranslate)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformTranslate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformScale)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformScale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformRotate)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformRotate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CGAffineTransformConcat)
extern "C" uintptr_t iostodroid_compat_CGAffineTransformConcat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_msgSendSuper)
extern "C" uintptr_t iostodroid_compat_objc_msgSendSuper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_msgSendSuper_stret)
extern "C" uintptr_t iostodroid_compat_objc_msgSendSuper_stret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_msgSendSuper2_stret)
extern "C" uintptr_t iostodroid_compat_objc_msgSendSuper2_stret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_msgSend_fpret)
extern "C" uintptr_t iostodroid_compat_objc_msgSend_fpret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_getClass)
extern "C" uintptr_t iostodroid_compat_objc_getClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_getClass");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_lookUpClass)
extern "C" uintptr_t iostodroid_compat_objc_lookUpClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_lookUpClass");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_getMetaClass)
extern "C" uintptr_t iostodroid_compat_objc_getMetaClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_getMetaClass");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_getProtocol)
extern "C" uintptr_t iostodroid_compat_objc_getProtocol(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_allocateClassPair)
extern "C" uintptr_t iostodroid_compat_objc_allocateClassPair(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_registerClassPair)
extern "C" uintptr_t iostodroid_compat_objc_registerClassPair(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_retain)
extern "C" uintptr_t iostodroid_compat_objc_retain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_release)
extern "C" uintptr_t iostodroid_compat_objc_release(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_autorelease)
extern "C" uintptr_t iostodroid_compat_objc_autorelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_autoreleasePoolPush)
extern "C" uintptr_t iostodroid_compat_objc_autoreleasePoolPush(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_autoreleasePoolPush");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_autoreleasePoolPop)
extern "C" uintptr_t iostodroid_compat_objc_autoreleasePoolPop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_retainAutorelease)
extern "C" uintptr_t iostodroid_compat_objc_retainAutorelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_retainAutoreleaseReturnValue)
extern "C" uintptr_t iostodroid_compat_objc_retainAutoreleaseReturnValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_retainAutoreleasedReturnValue)
extern "C" uintptr_t iostodroid_compat_objc_retainAutoreleasedReturnValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_storeStrong)
extern "C" uintptr_t iostodroid_compat_objc_storeStrong(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_storeWeak)
extern "C" uintptr_t iostodroid_compat_objc_storeWeak(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_loadWeakRetained)
extern "C" uintptr_t iostodroid_compat_objc_loadWeakRetained(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_destroyWeak)
extern "C" uintptr_t iostodroid_compat_objc_destroyWeak(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_getProperty)
extern "C" uintptr_t iostodroid_compat_objc_getProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_copyStruct)
extern "C" uintptr_t iostodroid_compat_objc_copyStruct(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_sync_enter)
extern "C" uintptr_t iostodroid_compat_objc_sync_enter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_sync_exit)
extern "C" uintptr_t iostodroid_compat_objc_sync_exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_exception_throw)
extern "C" uintptr_t iostodroid_compat_objc_exception_throw(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_begin_catch)
extern "C" uintptr_t iostodroid_compat_objc_begin_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_objc_end_catch)
extern "C" uintptr_t iostodroid_compat_objc_end_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sel_registerName)
extern "C" uintptr_t iostodroid_compat_sel_registerName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_sel_registerName");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sel_getUid)
extern "C" uintptr_t iostodroid_compat_sel_getUid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_sel_getUid");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_sel_getName)
extern "C" uintptr_t iostodroid_compat_sel_getName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_class_getName)
extern "C" uintptr_t iostodroid_compat_class_getName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_class_getSuperclass)
extern "C" uintptr_t iostodroid_compat_class_getSuperclass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_class_getInstanceMethod)
extern "C" uintptr_t iostodroid_compat_class_getInstanceMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_class_getClassMethod)
extern "C" uintptr_t iostodroid_compat_class_getClassMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_class_addMethod)
extern "C" uintptr_t iostodroid_compat_class_addMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_class_replaceMethod)
extern "C" uintptr_t iostodroid_compat_class_replaceMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_class_createInstance)
extern "C" uintptr_t iostodroid_compat_class_createInstance(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_object_getClass)
extern "C" uintptr_t iostodroid_compat_object_getClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_object_getClassName)
extern "C" uintptr_t iostodroid_compat_object_getClassName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___MPMoviePlayerController)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___MPMoviePlayerController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_MPMoviePlayerController");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSDate)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSDate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSDate");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSLocale)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSLocale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSLocale");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSNotificationCenter)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSNotificationCenter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSNotificationCenter");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSUserDefaults)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSUserDefaults(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSUserDefaults");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIColor)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIColor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIColor");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIDevice)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIDevice(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIDevice");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIImage)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIImage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIImage");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIViewController)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIViewController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIViewController");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___AVAudioPlayer)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___AVAudioPlayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_AVAudioPlayer");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___AVAudioSession)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___AVAudioSession(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_AVAudioSession");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSArray)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSArray");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSMutableArray)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableArray");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSMutableDictionary)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableDictionary(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableDictionary");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSMutableString)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableString");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSData)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSData");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSMutableData)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableData");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSSet)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSSet(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSSet");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSMutableSet)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSMutableSet(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableSet");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSFileManager)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSFileManager(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSFileManager");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSTimer)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSTimer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSTimer");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSRunLoop)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSRunLoop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSRunLoop");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSProcessInfo)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSProcessInfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSProcessInfo");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSValue)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSValue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___NSError)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___NSError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSError");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIImageView)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIImageView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIImageView");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UILabel)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UILabel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UILabel");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIButton)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIButton(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIButton");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIScrollView)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIScrollView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIScrollView");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIAlertView)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIAlertView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIAlertView");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIActivityIndicatorView)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIActivityIndicatorView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIActivityIndicatorView");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIWebView)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIWebView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIWebView");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIFont)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIFont(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIFont");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UITouch)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UITouch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UITouch");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___UIEvent)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___UIEvent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIEvent");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___CALayer)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___CALayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CALayer");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___CATransaction)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___CATransaction(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CATransaction");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___CABasicAnimation)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___CABasicAnimation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CABasicAnimation");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___SKPaymentQueue)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___SKPaymentQueue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_SKPaymentQueue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___SKProductsRequest)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___SKProductsRequest(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_SKProductsRequest");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___GKLocalPlayer)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___GKLocalPlayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_GKLocalPlayer");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___CMMotionManager)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___CMMotionManager(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CMMotionManager");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_CLASS___GCController)
extern "C" uintptr_t iostodroid_compat_OBJC_CLASS___GCController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_GCController");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_METACLASS___UIViewController)
extern "C" uintptr_t iostodroid_compat_OBJC_METACLASS___UIViewController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_UIViewController");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_OBJC_METACLASS___UIApplication)
extern "C" uintptr_t iostodroid_compat_OBJC_METACLASS___UIApplication(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_UIApplication");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIGraphicsPushContext)
extern "C" uintptr_t iostodroid_compat_UIGraphicsPushContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIGraphicsPopContext)
extern "C" uintptr_t iostodroid_compat_UIGraphicsPopContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIGraphicsGetCurrentContext)
extern "C" uintptr_t iostodroid_compat_UIGraphicsGetCurrentContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIGraphicsGetCurrentContext");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIGraphicsBeginImageContext)
extern "C" uintptr_t iostodroid_compat_UIGraphicsBeginImageContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIGraphicsBeginImageContextWithOptions)
extern "C" uintptr_t iostodroid_compat_UIGraphicsBeginImageContextWithOptions(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIGraphicsGetImageFromCurrentImageContext)
extern "C" uintptr_t iostodroid_compat_UIGraphicsGetImageFromCurrentImageContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIGraphicsGetImageFromCurrentImageContext");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIGraphicsEndImageContext)
extern "C" uintptr_t iostodroid_compat_UIGraphicsEndImageContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIImagePNGRepresentation)
extern "C" uintptr_t iostodroid_compat_UIImagePNGRepresentation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIImagePNGRepresentation");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIImageJPEGRepresentation)
extern "C" uintptr_t iostodroid_compat_UIImageJPEGRepresentation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIImageJPEGRepresentation");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_UIImageWriteToSavedPhotosAlbum)
extern "C" uintptr_t iostodroid_compat_UIImageWriteToSavedPhotosAlbum(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSTemporaryDirectory)
extern "C" uintptr_t iostodroid_compat_NSTemporaryDirectory(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_NSTemporaryDirectory");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSHomeDirectory)
extern "C" uintptr_t iostodroid_compat_NSHomeDirectory(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_NSHomeDirectory");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSLog)
extern "C" uintptr_t iostodroid_compat_NSLog(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSStringFromClass)
extern "C" uintptr_t iostodroid_compat_NSStringFromClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSClassFromString)
extern "C" uintptr_t iostodroid_compat_NSClassFromString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSStringFromSelector)
extern "C" uintptr_t iostodroid_compat_NSStringFromSelector(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSSelectorFromString)
extern "C" uintptr_t iostodroid_compat_NSSelectorFromString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_NSPageSize)
extern "C" uintptr_t iostodroid_compat_NSPageSize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_async)
extern "C" uintptr_t iostodroid_compat_dispatch_async(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_sync)
extern "C" uintptr_t iostodroid_compat_dispatch_sync(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_after)
extern "C" uintptr_t iostodroid_compat_dispatch_after(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_once)
extern "C" uintptr_t iostodroid_compat_dispatch_once(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_async_f)
extern "C" uintptr_t iostodroid_compat_dispatch_async_f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_sync_f)
extern "C" uintptr_t iostodroid_compat_dispatch_sync_f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_once_f)
extern "C" uintptr_t iostodroid_compat_dispatch_once_f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_get_main_queue)
extern "C" uintptr_t iostodroid_compat_dispatch_get_main_queue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_get_main_queue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_get_global_queue)
extern "C" uintptr_t iostodroid_compat_dispatch_get_global_queue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_get_global_queue");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_queue_create)
extern "C" uintptr_t iostodroid_compat_dispatch_queue_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_queue_create");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_release)
extern "C" uintptr_t iostodroid_compat_dispatch_release(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_retain)
extern "C" uintptr_t iostodroid_compat_dispatch_retain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_time)
extern "C" uintptr_t iostodroid_compat_dispatch_time(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_semaphore_create)
extern "C" uintptr_t iostodroid_compat_dispatch_semaphore_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_semaphore_create");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_semaphore_wait)
extern "C" uintptr_t iostodroid_compat_dispatch_semaphore_wait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_semaphore_signal)
extern "C" uintptr_t iostodroid_compat_dispatch_semaphore_signal(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_group_create)
extern "C" uintptr_t iostodroid_compat_dispatch_group_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_group_create");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_group_async)
extern "C" uintptr_t iostodroid_compat_dispatch_group_async(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_group_enter)
extern "C" uintptr_t iostodroid_compat_dispatch_group_enter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_group_leave)
extern "C" uintptr_t iostodroid_compat_dispatch_group_leave(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_group_wait)
extern "C" uintptr_t iostodroid_compat_dispatch_group_wait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_dispatch_group_notify)
extern "C" uintptr_t iostodroid_compat_dispatch_group_notify(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__dispatch_main_q)
extern "C" uintptr_t iostodroid_compat__dispatch_main_q(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__dispatch_main_q");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SCNetworkReachabilityCreateWithAddress)
extern "C" uintptr_t iostodroid_compat_SCNetworkReachabilityCreateWithAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_SCNetworkReachabilityCreateWithAddress");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SCNetworkReachabilityCreateWithName)
extern "C" uintptr_t iostodroid_compat_SCNetworkReachabilityCreateWithName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_SCNetworkReachabilityCreateWithName");
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SCNetworkReachabilityGetFlags)
extern "C" uintptr_t iostodroid_compat_SCNetworkReachabilityGetFlags(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SCNetworkReachabilitySetCallback)
extern "C" uintptr_t iostodroid_compat_SCNetworkReachabilitySetCallback(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SCNetworkReachabilityScheduleWithRunLoop)
extern "C" uintptr_t iostodroid_compat_SCNetworkReachabilityScheduleWithRunLoop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SCNetworkReachabilityUnscheduleFromRunLoop)
extern "C" uintptr_t iostodroid_compat_SCNetworkReachabilityUnscheduleFromRunLoop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SCNetworkReachabilitySetDispatchQueue)
extern "C" uintptr_t iostodroid_compat_SCNetworkReachabilitySetDispatchQueue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SecRandomCopyBytes)
extern "C" uintptr_t iostodroid_compat_SecRandomCopyBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SecItemCopyMatching)
extern "C" uintptr_t iostodroid_compat_SecItemCopyMatching(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SecItemAdd)
extern "C" uintptr_t iostodroid_compat_SecItemAdd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SecItemUpdate)
extern "C" uintptr_t iostodroid_compat_SecItemUpdate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_SecItemDelete)
extern "C" uintptr_t iostodroid_compat_SecItemDelete(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CC_MD5)
extern "C" uintptr_t iostodroid_compat_CC_MD5(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CC_SHA1)
extern "C" uintptr_t iostodroid_compat_CC_SHA1(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat_CC_SHA256)
extern "C" uintptr_t iostodroid_compat_CC_SHA256(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_DeleteException)
extern "C" uintptr_t iostodroid_compat__Unwind_DeleteException(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_GetIP)
extern "C" uintptr_t iostodroid_compat__Unwind_GetIP(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_SetIP)
extern "C" uintptr_t iostodroid_compat__Unwind_SetIP(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_GetGR)
extern "C" uintptr_t iostodroid_compat__Unwind_GetGR(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_SetGR)
extern "C" uintptr_t iostodroid_compat__Unwind_SetGR(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_GetLanguageSpecificData)
extern "C" uintptr_t iostodroid_compat__Unwind_GetLanguageSpecificData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Unwind_GetRegionStart)
extern "C" uintptr_t iostodroid_compat__Unwind_GetRegionStart(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___gxx_personality_v0)
extern "C" uintptr_t iostodroid_compat___gxx_personality_v0(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___gcc_personality_v0)
extern "C" uintptr_t iostodroid_compat___gcc_personality_v0(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___udivdi3)
extern "C" uint64_t iostodroid_compat___udivdi3(uint64_t a, uint64_t b) {
    return b == 0 ? 0u : (a / b);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___umoddi3)
extern "C" uint64_t iostodroid_compat___umoddi3(uint64_t a, uint64_t b) {
    return b == 0 ? 0u : (a % b);
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___muldi3)
extern "C" uintptr_t iostodroid_compat___muldi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___fixsfdi)
extern "C" uintptr_t iostodroid_compat___fixsfdi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___fixunsdfdi)
extern "C" uintptr_t iostodroid_compat___fixunsdfdi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___fixunssfdi)
extern "C" uintptr_t iostodroid_compat___fixunssfdi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___floatundidf)
extern "C" uintptr_t iostodroid_compat___floatundidf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___floatundisf)
extern "C" uintptr_t iostodroid_compat___floatundisf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___ashldi3)
extern "C" uintptr_t iostodroid_compat___ashldi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___ashrdi3)
extern "C" uintptr_t iostodroid_compat___ashrdi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___lshrdi3)
extern "C" uintptr_t iostodroid_compat___lshrdi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cmpdi2)
extern "C" uintptr_t iostodroid_compat___cmpdi2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___ucmpdi2)
extern "C" uintptr_t iostodroid_compat___ucmpdi2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___clear_cache)
extern "C" uintptr_t iostodroid_compat___clear_cache(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Znaj)
extern "C" uintptr_t iostodroid_compat__Znaj(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat__Znwj)
extern "C" uintptr_t iostodroid_compat__Znwj(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_free_exception)
extern "C" uintptr_t iostodroid_compat___cxa_free_exception(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_rethrow)
extern "C" uintptr_t iostodroid_compat___cxa_rethrow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_guard_acquire)
extern "C" uintptr_t iostodroid_compat___cxa_guard_acquire(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_guard_release)
extern "C" uintptr_t iostodroid_compat___cxa_guard_release(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_guard_abort)
extern "C" uintptr_t iostodroid_compat___cxa_guard_abort(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___cxa_demangle)
extern "C" uintptr_t iostodroid_compat___cxa_demangle(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(IOSTODROID_API_REPLACEMENTS_ONLY) || defined(IOSTODROID_API_iostodroid_compat___dynamic_cast)
extern "C" uintptr_t iostodroid_compat___dynamic_cast(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

