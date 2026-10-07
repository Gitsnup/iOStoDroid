#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/macho_loader.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/trap_shims.hpp"
#include "json.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace radek::compat_runtime {

/** Loads one authorized IPA main executable and reports its first runtime blocker. */
class GuestRunner {
    const ShimRegistry &shims_;
    const CpuBackend &cpu_;
    std::size_t memoryLimit_;

  public:
    GuestRunner(const ShimRegistry &shims, const CpuBackend &cpu,
                std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), memoryLimit_(memoryLimit) {}

    radek::Json runMainBinary(const std::vector<std::uint8_t> &mainBinary,
                              bool authorizationConfirmed) const;
};

/**
 * Optional bounded application-lifecycle hooks for the boot attempt.
 *
 * A boot attempt that gets past `_UIApplicationMain` may leave a queued
 * background-thread entry (an `NSThread` detach) that the runtime cannot run as
 * a second host thread. These hooks let the caller describe the lifecycle trace
 * and hand the runner an ABI-correct entry point for that queued body so it can
 * be executed on the same single guest CPU.
 */
struct BootLifecycleHooks {
    std::function<bool(GuestAddressSpace &, CpuRegisterState &, GuestAddress &, std::string &)>
        prepareMainThreadEntry;
    std::function<void(GuestAddressSpace &, std::uint32_t frames)> setMainThreadServiceLimit;
    std::function<void(GuestAddressSpace &)> finishMainThreadEntry;
    std::function<void(GuestAddressSpace &, radek::Json &report)> describe;
};

/**
 * Boot-attempt runner: binds unimplemented imports to abort-on-call traps
 * and executes real guest instructions until the first actually-used missing
 * import. The registry gains trap bindings during the load. The report keeps
 * status "not_runnable" (a boot attempt is never gameplay evidence) and
 * carries the executed-instruction count plus the stopping import.
 */
class BootAttemptRunner {
    ShimRegistry &shims_;
    const CpuBackend &cpu_;
    TrapShimAdapter &traps_;
    std::size_t memoryLimit_;
    BootLifecycleHooks lifecycle_;
    std::uint64_t entryInstructionBudget_ = 2000000;
    std::uint64_t entryTimeLimitMicros_ = 20000000;
    std::uint64_t mainThreadInstructionBudget_ = 2000000;
    std::uint64_t mainThreadTimeLimitMicros_ = 20000000;

  public:
    BootAttemptRunner(ShimRegistry &shims, const CpuBackend &cpu,
                      TrapShimAdapter &traps,
                      std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), traps_(traps), memoryLimit_(memoryLimit) {}

    BootAttemptRunner(ShimRegistry &shims, const CpuBackend &cpu,
                      TrapShimAdapter &traps, BootLifecycleHooks lifecycle,
                      std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), traps_(traps), memoryLimit_(memoryLimit),
          lifecycle_(std::move(lifecycle)) {}

    /**
     * Bounded budget for the Mach-O entry point. The entry point of a UIKit app
     * runs `_main` -> `UIApplicationMain` -> the delegate's
     * `applicationDidFinishLaunching:`, so it needs far more than the probe's
     * historical one-million-instruction window. The bound stays explicit: the
     * report always states how many instructions were executed and whether the
     * budget stopped the attempt.
     */
    void setEntryBudget(std::uint64_t instructions, std::uint64_t timeLimitMicros) noexcept {
        entryInstructionBudget_ = instructions;
        entryTimeLimitMicros_ = timeLimitMicros;
    }

    /** Bounded budget for the queued background-thread body. */
    void setMainThreadInstructionBudget(std::uint64_t instructions,
                                        std::uint64_t timeLimitMicros) noexcept {
        mainThreadInstructionBudget_ = instructions;
        mainThreadTimeLimitMicros_ = timeLimitMicros;
    }

    radek::Json run(const std::vector<std::uint8_t> &mainBinary,
                    bool authorizationConfirmed);
};

} // namespace radek::compat_runtime
