#include "compat_runtime/trap_shims.hpp"

#include <stdexcept>
#include <utility>

namespace iostodroid::compat_runtime {

GuestAddress TrapShimAdapter::bind(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &library) {
    if (symbol.empty())
        throw std::invalid_argument("trap import symbol must not be empty");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = traps_.find(symbol);
        if (existing != traps_.end())
            return existing->second;
        if (nextCallout_ > kTrapEnd - 4u)
            throw std::overflow_error("unresolved-import trap range is exhausted");
    }

    GuestAddress callout = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-check under the lock: two threads may race to trap one import.
        const auto existing = traps_.find(symbol);
        if (existing != traps_.end())
            return existing->second;
        callout = nextCallout_;
        nextCallout_ += 4u;
        traps_.emplace(symbol, callout);
        byAddress_.emplace(callout, symbol);
    }

    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = library.empty() ? "unknown-dylib" : library;
    binding.adapterName = kAdapterName;
    binding.guestAddress = callout;
    binding.invokeException = [this, symbol](CpuRegisterState &registers,
                                             GuestAddressSpace & /*memory*/,
                                             std::string &reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastTrapped_ = symbol;
            ++trapCalls_;
        }
        constexpr char digits[] = "0123456789abcdef";
        std::string r0 = "0x";
        for (int shift = 28; shift >= 0; shift -= 4)
            r0.push_back(digits[(registers.r[0] >> shift) & 0xf]);
        reason = "unimplemented Darwin import '" + symbol + "' called by guest code " +
                 "(r0=" + r0 + "); boot stopped at the first unimplemented call";
        return true;
    };
    registry.registerBinding(std::move(binding));
    return callout;
}

bool TrapShimAdapter::has(const std::string &symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return traps_.find(symbol) != traps_.end();
}

std::optional<std::string> TrapShimAdapter::symbolForAddress(GuestAddress address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (address < kTrapBase || address >= nextCallout_ || byAddress_.empty())
        return std::nullopt;
    const auto slot = kTrapBase + ((address - kTrapBase) / 4u) * 4u;
    const auto exact = byAddress_.find(slot);
    if (exact != byAddress_.end())
        return exact->second;
    // Trap+addend data touches land between slots; attribute them to the
    // nearest lower slot as a best-effort fault mapping.
    auto lower = byAddress_.lower_bound(slot);
    if (lower == byAddress_.begin())
        return std::nullopt;
    --lower;
    return lower->second;
}

std::optional<std::string> TrapShimAdapter::lastTrapped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastTrapped_;
}

std::uint64_t TrapShimAdapter::trapCalls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trapCalls_;
}

std::size_t TrapShimAdapter::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return traps_.size();
}

std::vector<std::string> TrapShimAdapter::symbols() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(traps_.size());
    for (const auto &entry : traps_)
        result.push_back(entry.first);
    return result;
}

} // namespace iostodroid::compat_runtime
