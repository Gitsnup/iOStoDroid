#include "cad_display_link_compat.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>

#define CHECK(expression)                                                                    \
    do {                                                                                     \
        if (!(expression)) throw std::runtime_error("CHECK failed: " #expression);            \
    } while (false)

namespace {
struct FrameCounts {
    unsigned calls = 0;
    double timestamp = 0.0;
    double duration = 0.0;
    bool invalidateOnFrame = false;
};

void recordFrame(void *opaque, iostodroid_CADisplayLinkRef link, double timestamp, double duration) {
    auto *counts = static_cast<FrameCounts *>(opaque);
    ++counts->calls;
    counts->timestamp = timestamp;
    counts->duration = duration;
    if (counts->invalidateOnFrame) iostodroid_compat_CADisplayLinkInvalidate(link);
}

void testFrameDispatchAndPause() {
    FrameCounts counts;
    auto link = iostodroid_compat_CADisplayLinkCreate(recordFrame, &counts);
    CHECK(link != nullptr);
    CHECK(iostodroid_compat_CADisplayLinkActiveCount() == 1);
    CHECK(iostodroid_compat_CADisplayLinkIsPaused(link) == 0);

    iostodroid_compat_CADisplayLinkDispatchFrame(1'000'000'000LL, 16'666'667LL);
    CHECK(counts.calls == 1);
    CHECK(std::abs(counts.timestamp - 1.0) < 1e-9);
    CHECK(std::abs(counts.duration - 0.016666667) < 1e-9);
    CHECK(std::abs(iostodroid_compat_CADisplayLinkGetTimestamp(link) - 1.0) < 1e-9);

    iostodroid_compat_CADisplayLinkSetPaused(link, 1);
    CHECK(iostodroid_compat_CADisplayLinkIsPaused(link) == 1);
    iostodroid_compat_CADisplayLinkDispatchFrame(1'020'000'000LL, 20'000'000LL);
    CHECK(counts.calls == 1);

    iostodroid_compat_CADisplayLinkSetPaused(link, 0);
    CHECK(iostodroid_compat_CADisplayLinkIsPaused(link) == 0);
    CHECK(iostodroid_compat_CADisplayLinkSetPreferredFramesPerSecond(link, 30) == 1);
    CHECK(iostodroid_compat_CADisplayLinkGetPreferredFramesPerSecond(link) == 30);
    CHECK(iostodroid_compat_CADisplayLinkSetPreferredFramesPerSecond(link, 241) == 0);
    iostodroid_compat_CADisplayLinkDispatchFrame(1'020'000'000LL, 20'000'000LL);
    CHECK(counts.calls == 2); // first frame after resume is delivered immediately
    CHECK(std::abs(counts.timestamp - 1.02) < 1e-9);
    CHECK(std::abs(counts.duration - 0.02) < 1e-9);
    iostodroid_compat_CADisplayLinkDispatchFrame(1'040'000'000LL, 20'000'000LL);
    CHECK(counts.calls == 2); // preferred 30 Hz has not elapsed
    iostodroid_compat_CADisplayLinkDispatchFrame(1'060'000'000LL, 20'000'000LL);
    CHECK(counts.calls == 3);
    CHECK(std::abs(counts.timestamp - 1.06) < 1e-9);
    CHECK(std::abs(counts.duration - 0.04) < 1e-9);

    // A paused/invalidation state is respected even if the frame clock delivers
    // a duplicate or out-of-order timestamp.
    iostodroid_compat_CADisplayLinkDispatchFrame(1'050'000'000LL, 20'000'000LL);
    CHECK(counts.calls == 3);
    iostodroid_compat_CADisplayLinkInvalidate(link);
    CHECK(iostodroid_compat_CADisplayLinkActiveCount() == 0);
    iostodroid_compat_CADisplayLinkDispatchFrame(1'060'000'000LL, 20'000'000LL);
    CHECK(counts.calls == 3);
    iostodroid_compat_CADisplayLinkRelease(link);
    CHECK(iostodroid_compat_CADisplayLinkActiveCount() == 0);
}

void testSelfInvalidationAndNullHandling() {
    FrameCounts counts;
    counts.invalidateOnFrame = true;
    CHECK(iostodroid_compat_CADisplayLinkCreate(nullptr, &counts) == nullptr);
    auto link = iostodroid_compat_CADisplayLinkCreate(recordFrame, &counts);
    CHECK(link != nullptr);
    iostodroid_compat_CADisplayLinkDispatchFrame(5'000'000'000LL, 16'000'000LL);
    CHECK(counts.calls == 1);
    CHECK(iostodroid_compat_CADisplayLinkActiveCount() == 0);
    iostodroid_compat_CADisplayLinkDispatchFrame(5'016'000'000LL, 16'000'000LL);
    CHECK(counts.calls == 1);
    iostodroid_compat_CADisplayLinkRelease(link);

    iostodroid_compat_CADisplayLinkDispatchFrame(0, 16'000'000LL);
    CHECK(iostodroid_compat_CADisplayLinkIsPaused(nullptr) == 1);
    CHECK(iostodroid_compat_CADisplayLinkSetPreferredFramesPerSecond(nullptr, 60) == 0);
    CHECK(iostodroid_compat_CADisplayLinkGetTimestamp(nullptr) == 0.0);
}
} // namespace

int main() {
    testFrameDispatchAndPause();
    testSelfInvalidationAndNullHandling();
}
