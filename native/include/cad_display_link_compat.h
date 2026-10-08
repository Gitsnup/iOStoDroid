#pragma once

/*
 * C callback service for frame-synchronised work in libioscompat.so.
 *
 * Android launchers drive iostodroid_compat_CADisplayLinkDispatchFrame from
 * android.view.Choreographer. This is a usable native frame scheduler for a
 * future static recompilation backend/runtime; it is not an Objective-C CADisplayLink class or
 * objc_msgSend bridge. The callback/context pair must outlive the active link.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iostodroid_CADisplayLink iostodroid_CADisplayLink;
typedef iostodroid_CADisplayLink *iostodroid_CADisplayLinkRef;

typedef void (*iostodroid_CADisplayLinkCallback)(void *context,
                                             iostodroid_CADisplayLinkRef displayLink,
                                             double timestamp,
                                             double duration);

iostodroid_CADisplayLinkRef iostodroid_compat_CADisplayLinkCreate(iostodroid_CADisplayLinkCallback callback,
                                                         void *context);
void iostodroid_compat_CADisplayLinkInvalidate(iostodroid_CADisplayLinkRef displayLink);
void iostodroid_compat_CADisplayLinkRelease(iostodroid_CADisplayLinkRef displayLink);
void iostodroid_compat_CADisplayLinkSetPaused(iostodroid_CADisplayLinkRef displayLink, uint8_t paused);
uint8_t iostodroid_compat_CADisplayLinkIsPaused(iostodroid_CADisplayLinkRef displayLink);

/* 0 follows the display refresh; 1..240 requests an interval no faster than that rate. */
int iostodroid_compat_CADisplayLinkSetPreferredFramesPerSecond(iostodroid_CADisplayLinkRef displayLink,
                                                            int framesPerSecond);
int iostodroid_compat_CADisplayLinkGetPreferredFramesPerSecond(iostodroid_CADisplayLinkRef displayLink);
double iostodroid_compat_CADisplayLinkGetTimestamp(iostodroid_CADisplayLinkRef displayLink);
double iostodroid_compat_CADisplayLinkGetDuration(iostodroid_CADisplayLinkRef displayLink);
size_t iostodroid_compat_CADisplayLinkActiveCount(void);

/* Called by the launcher once per Choreographer frame; timestamp/duration are ns. */
void iostodroid_compat_CADisplayLinkDispatchFrame(int64_t frameTimeNanos,
                                              int64_t frameIntervalNanos);

#ifdef __cplusplus
} /* extern "C" */
#endif
