#include "cad_display_link_compat.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct radek_CADisplayLink {
    radek_CADisplayLinkCallback callback = nullptr;
    void *context = nullptr;
    std::atomic<bool> paused{false};
    std::atomic<bool> invalidated{false};
    std::atomic<int> preferredFramesPerSecond{0};
    mutable std::mutex timingMutex;
    int64_t lastFrameTimeNanos = 0;
    double timestamp = 0.0;
    double duration = 0.0;
};

namespace {
std::mutex g_linksMutex;
std::unordered_map<radek_CADisplayLinkRef, std::shared_ptr<radek_CADisplayLink>> g_links;

std::shared_ptr<radek_CADisplayLink> findLink(radek_CADisplayLinkRef handle) {
    if (handle == nullptr) return {};
    std::lock_guard<std::mutex> lock(g_linksMutex);
    const auto found = g_links.find(handle);
    return found == g_links.end() ? std::shared_ptr<radek_CADisplayLink>{} : found->second;
}

bool shouldDeliver(radek_CADisplayLink &link,
                   int64_t frameTimeNanos,
                   int64_t frameIntervalNanos,
                   double &timestamp,
                   double &duration) {
    if (link.invalidated.load(std::memory_order_acquire) ||
        link.paused.load(std::memory_order_acquire) || link.callback == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(link.timingMutex);
    if (link.invalidated.load(std::memory_order_relaxed) ||
        link.paused.load(std::memory_order_relaxed)) {
        return false;
    }
    if (link.lastFrameTimeNanos != 0 && frameTimeNanos <= link.lastFrameTimeNanos) return false;

    const int preferred = link.preferredFramesPerSecond.load(std::memory_order_relaxed);
    const int64_t minimumInterval = preferred > 0 ? 1'000'000'000LL / preferred : 0;
    if (link.lastFrameTimeNanos != 0 && minimumInterval > 0 &&
        frameTimeNanos - link.lastFrameTimeNanos < minimumInterval) {
        return false;
    }

    int64_t elapsed = link.lastFrameTimeNanos == 0
                          ? frameIntervalNanos
                          : frameTimeNanos - link.lastFrameTimeNanos;
    if (elapsed <= 0) elapsed = 16'666'667LL;
    link.lastFrameTimeNanos = frameTimeNanos;
    link.timestamp = static_cast<double>(frameTimeNanos) / 1'000'000'000.0;
    link.duration = static_cast<double>(elapsed) / 1'000'000'000.0;
    timestamp = link.timestamp;
    duration = link.duration;
    return true;
}
} // namespace

extern "C" radek_CADisplayLinkRef radek_compat_CADisplayLinkCreate(
    radek_CADisplayLinkCallback callback, void *context) {
    if (callback == nullptr) return nullptr;
    auto link = std::make_shared<radek_CADisplayLink>();
    link->callback = callback;
    link->context = context;
    const auto handle = link.get();
    std::lock_guard<std::mutex> lock(g_linksMutex);
    g_links.emplace(handle, std::move(link));
    return handle;
}

extern "C" void radek_compat_CADisplayLinkInvalidate(radek_CADisplayLinkRef displayLink) {
    const auto link = findLink(displayLink);
    if (!link) return;
    link->invalidated.store(true, std::memory_order_release);
}

extern "C" void radek_compat_CADisplayLinkRelease(radek_CADisplayLinkRef displayLink) {
    if (displayLink == nullptr) return;
    std::shared_ptr<radek_CADisplayLink> link;
    {
        std::lock_guard<std::mutex> lock(g_linksMutex);
        const auto found = g_links.find(displayLink);
        if (found == g_links.end()) return;
        link = std::move(found->second);
        g_links.erase(found);
    }
    link->invalidated.store(true, std::memory_order_release);
}

extern "C" void radek_compat_CADisplayLinkSetPaused(radek_CADisplayLinkRef displayLink,
                                                      uint8_t paused) {
    const auto link = findLink(displayLink);
    if (!link || link->invalidated.load(std::memory_order_acquire)) return;
    const bool requested = paused != 0;
    const bool previous = link->paused.exchange(requested, std::memory_order_acq_rel);
    if (previous != requested) {
        std::lock_guard<std::mutex> lock(link->timingMutex);
        link->lastFrameTimeNanos = 0;
    }
}

extern "C" uint8_t radek_compat_CADisplayLinkIsPaused(radek_CADisplayLinkRef displayLink) {
    const auto link = findLink(displayLink);
    return !link || link->paused.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int radek_compat_CADisplayLinkSetPreferredFramesPerSecond(
    radek_CADisplayLinkRef displayLink, int framesPerSecond) {
    if (framesPerSecond < 0 || framesPerSecond > 240) return 0;
    const auto link = findLink(displayLink);
    if (!link || link->invalidated.load(std::memory_order_acquire)) return 0;
    link->preferredFramesPerSecond.store(framesPerSecond, std::memory_order_release);
    return 1;
}

extern "C" int radek_compat_CADisplayLinkGetPreferredFramesPerSecond(
    radek_CADisplayLinkRef displayLink) {
    const auto link = findLink(displayLink);
    return link ? link->preferredFramesPerSecond.load(std::memory_order_acquire) : 0;
}

extern "C" double radek_compat_CADisplayLinkGetTimestamp(radek_CADisplayLinkRef displayLink) {
    const auto link = findLink(displayLink);
    if (!link) return 0.0;
    std::lock_guard<std::mutex> lock(link->timingMutex);
    return link->timestamp;
}

extern "C" double radek_compat_CADisplayLinkGetDuration(radek_CADisplayLinkRef displayLink) {
    const auto link = findLink(displayLink);
    if (!link) return 0.0;
    std::lock_guard<std::mutex> lock(link->timingMutex);
    return link->duration;
}

extern "C" size_t radek_compat_CADisplayLinkActiveCount(void) {
    std::lock_guard<std::mutex> lock(g_linksMutex);
    size_t active = 0;
    for (const auto &entry : g_links) {
        if (!entry.second->invalidated.load(std::memory_order_acquire)) ++active;
    }
    return active;
}

extern "C" void radek_compat_CADisplayLinkDispatchFrame(int64_t frameTimeNanos,
                                                          int64_t frameIntervalNanos) {
    if (frameTimeNanos <= 0) return;
    if (frameIntervalNanos < 0) frameIntervalNanos = 0;

    std::vector<std::shared_ptr<radek_CADisplayLink>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_linksMutex);
        snapshot.reserve(g_links.size());
        for (const auto &entry : g_links) snapshot.push_back(entry.second);
    }

    for (const auto &link : snapshot) {
        double timestamp = 0.0;
        double duration = 0.0;
        if (!shouldDeliver(*link, frameTimeNanos, frameIntervalNanos, timestamp, duration)) continue;
        // A callback can invalidate/release itself; the dispatch snapshot owns a
        // shared reference until the callback returns, so that is safe.
        try {
            link->callback(link->context, link.get(), timestamp, duration);
        } catch (...) {
            // Exceptions must not escape an Android frame callback / C ABI.
            link->invalidated.store(true, std::memory_order_release);
        }
    }
}

#if defined(__ANDROID__)
#include <jni.h>

extern "C" JNIEXPORT void JNICALL
Java_dev_radek_generated_FrameClockBridge_nativeDispatchFrame(JNIEnv *, jclass, jlong frameTimeNanos,
                                                               jlong frameIntervalNanos) {
    radek_compat_CADisplayLinkDispatchFrame(static_cast<int64_t>(frameTimeNanos),
                                            static_cast<int64_t>(frameIntervalNanos));
}
#endif
