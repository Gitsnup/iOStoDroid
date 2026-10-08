#pragma once

#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace iostodroid::compat_runtime {

/**
 * Abort-on-call traps for Darwin imports that have no tested adapter.
 *
 * The fail-closed loader reports such imports as unresolved and the default
 * runner refuses to execute. The boot-attempt path binds them to traps instead
 * so real guest instructions run until the first actually-called unimplemented
 * import. A trap never implements, emulates, or returns from the import: a
 * call stops execution with GUEST_EXCEPTION_RAISED and names the import, and a
 * data touch faults inside the trap range, which maps back to the import.
 *
 * Traps are not implementations. They are never counted as resolved symbols,
 * never registered in the smoke database, and never change the fail-closed
 * default path.
 */
class TrapShimAdapter {
    mutable std::mutex mutex_;
    std::map<std::string, GuestAddress> traps_;
    std::map<GuestAddress, std::string> byAddress_;
    GuestAddress nextCallout_ = kTrapBase;
    std::optional<std::string> lastTrapped_;
    std::uint64_t trapCalls_ = 0;

  public:
    static constexpr GuestAddress kTrapBase = 0xf0020000u;
    static constexpr GuestAddress kTrapEnd = 0xf0030000u;
    static constexpr const char *kAdapterName = "unresolved-import-trap";

    /**
     * Register (or reuse) the abort trap for one unimplemented import.
     * Returns the trap callout address written into the import slot.
     */
    GuestAddress bind(ShimRegistry &registry, const std::string &symbol,
                      const std::string &library);

    /** True when the symbol already has a trap binding. */
    bool has(const std::string &symbol) const;

    /**
     * Map a guest address inside the trap range back to its import. Accepts
     * trap+addend addresses from data relocations, not just slot bases.
     */
    std::optional<std::string> symbolForAddress(GuestAddress address) const;

    /** Import named by the most recent trap call, if any guest call landed. */
    std::optional<std::string> lastTrapped() const;

    /** Number of trap calls observed across all guest executions. */
    std::uint64_t trapCalls() const;

    std::size_t size() const;
    std::vector<std::string> symbols() const;
};

} // namespace iostodroid::compat_runtime
