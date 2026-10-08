#pragma once

#include <array>
#include <cstdint>

namespace iostodroid::compat_runtime {

// Full ARM user-mode register state. The integer bank is what the shim
// callouts see; the VFP bank travels with it because the guest ABI makes
// `d8`-`d15` callee-saved and the converted binaries do use scalar VFP for
// their own float math, so a value the guest left in a callee-saved VFP
// register must survive a native callout exactly like `r4`-`r11` do.
struct CpuRegisterState {
    std::array<std::uint32_t, 16> r{};
    std::uint32_t cpsr = 0;
    // VFP double banks: `d[i]` holds `s[2i]` in its low half and `s[2i+1]` in
    // its high half. Held as raw bits so the struct stays trivially copyable
    // and no host floating-point rounding can alter a guest value.
    std::array<std::uint64_t, 16> d{};
    std::uint32_t fpscr = 0;
};

} // namespace iostodroid::compat_runtime
