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

  public:
    BootAttemptRunner(ShimRegistry &shims, const CpuBackend &cpu,
                      TrapShimAdapter &traps,
                      std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), traps_(traps), memoryLimit_(memoryLimit) {}

    radek::Json run(const std::vector<std::uint8_t> &mainBinary,
                    bool authorizationConfirmed);
};

} // namespace radek::compat_runtime
