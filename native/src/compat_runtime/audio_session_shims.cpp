#include "compat_runtime/audio_session_shims.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace radek::compat_runtime::audio {
namespace {
constexpr std::uint32_t kAudioSessionNotInitialized = 0x21696e69U; // '!ini'
}

bool ShimAdapter::initialize(CpuRegisterState &registers, std::string &) {
    const AudioSessionInitialization requested{
        registers.r[0], registers.r[1], registers.r[2], registers.r[3],
    };
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The legacy API is a process-wide one-time initializer. Preserve its
        // first callback/context; repeated calls remain harmless and idempotent.
        if (!initialization_) {
            initialization_ = requested;
            activeState_ = false;
        }
    }
    // OSStatus noErr. Audio interruptions are not synthesized by this adapter.
    registers.r[0] = 0;
    return true;
}

bool ShimAdapter::setActive(CpuRegisterState &registers, std::string &) {
    const auto requestedState = registers.r[0] != 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialization_) {
        registers.r[0] = kAudioSessionNotInitialized;
        return true;
    }
    activeState_ = requestedState;
    // This records the guest's requested state only; no audio device is touched.
    registers.r[0] = 0;
    return true;
}

void ShimAdapter::registerBindings(ShimRegistry &registry) {
    if (registered_)
        throw std::runtime_error("AudioSession shim adapter is already registered");
    ShimBinding binding;
    binding.darwinSymbol = "_AudioSessionInitialize";
    binding.library = "AudioToolbox";
    binding.adapterName = "audio-session-initialize-state-only";
    binding.guestAddress = 0xf0010000;
    binding.invoke = [this](CpuRegisterState &registers, GuestAddressSpace &,
                            std::string &reason) {
        return initialize(registers, reason);
    };
    registry.registerBinding(std::move(binding));

    ShimBinding setActiveBinding;
    setActiveBinding.darwinSymbol = "_AudioSessionSetActive";
    setActiveBinding.library = "AudioToolbox";
    setActiveBinding.adapterName = "audio-session-set-active-state-only";
    setActiveBinding.guestAddress = 0xf0010004;
    setActiveBinding.invoke = [this](CpuRegisterState &registers, GuestAddressSpace &,
                                    std::string &reason) {
        return setActive(registers, reason);
    };
    registry.registerBinding(std::move(setActiveBinding));
    registered_ = true;
}

std::optional<AudioSessionInitialization> ShimAdapter::initialization() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialization_;
}

std::optional<bool> ShimAdapter::activeState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeState_;
}

} // namespace radek::compat_runtime::audio
