#pragma once

#include "compat_runtime/shim_registry.hpp"

#include <mutex>
#include <optional>

namespace iostodroid::compat_runtime::audio {

/**
 * Narrow, state-only AudioSessionServices adapter.
 *
 * AudioSessionInitialize has a 32-bit guest ABI of four pointer-sized arguments
 * in r0-r3 and an OSStatus result in r0. AudioSessionSetActive takes its
 * Boolean in r0 and returns OSStatus in r0. These adapters retain initialization
 * and requested active/inactive state only; they do not activate an Android/iOS
 * device, schedule interruption callbacks, or produce audio output.
 */
struct AudioSessionInitialization {
    GuestAddress runLoop = 0;
    GuestAddress runLoopMode = 0;
    GuestAddress interruptionListener = 0;
    GuestAddress clientData = 0;
};

class ShimAdapter {
    mutable std::mutex mutex_;
    std::optional<AudioSessionInitialization> initialization_;
    std::optional<bool> activeState_;
    bool registered_ = false;

    bool initialize(CpuRegisterState &registers, std::string &reason);
    bool setActive(CpuRegisterState &registers, std::string &reason);

  public:
    /** Register the exact legacy AudioSessionInitialize/SetActive imports. */
    void registerBindings(ShimRegistry &registry);
    std::optional<AudioSessionInitialization> initialization() const;
    std::optional<bool> activeState() const;
};

} // namespace iostodroid::compat_runtime::audio
