#pragma once

/*
 * Dynamic symbol-resolution registry for libioscompat.so.
 *
 * Every Darwin/iOS import known to the converter resolves to exactly one of:
 *
 *  - Kind::Verified : a real, host-tested implementation body (the time shims
 *    and selected C/POSIX/CoreFoundation shims), or
 *  - Kind::Stub     : an explicitly unimplemented resolution handler. A stub
 *    owns a stable function address that records invocations and returns a
 *    documented safe default. A stub is a resolution target only; it is never
 *    an implementation of the Darwin API and must not be reported as one.
 *
 * The registry supports dynamic runtime registration (dlsym-visible C ABI in
 * ioscompat_registry.cpp) so downstream linkers can register the unmapped
 * import set of a specific IPA at load time.
 */

#include <cstddef>
#include <cstdint>

namespace iostodroid_compat {

enum class Kind : int {
    Verified = 1,
    Stub = 2,
};

struct Record {
    const char *darwinSymbol;  // e.g. "_CFAbsoluteTimeGetCurrent"
    const char *androidSymbol; // implementation or stub trampoline name
    Kind kind;
    void (*handler)(); // address only; callers cast to the real signature
};

// Number of distinct stub trampolines with individual call counters.
constexpr std::size_t kStubPoolSize = 2048;

// Look up a Darwin symbol (with its leading underscore). Returns nullptr when
// unknown. The pointer stays valid for the process lifetime.
const Record *lookup(const char *darwinSymbol);

// Register an unmapped Darwin symbol as an explicit stub. Idempotent: repeated
// registration of the same name keeps the first record and returns false.
// Returns false as well when the name is invalid.
bool registerStub(const char *darwinSymbol);

std::size_t size();
const Record *at(std::size_t index);
std::size_t verifiedCount();
std::size_t stubCount();

// Observability: how often stub trampolines were actually invoked.
std::uint64_t stubCallTotal();
std::uint64_t stubCallCount(const char *darwinSymbol);

} // namespace iostodroid_compat

// dlsym-visible C ABI (implemented in ioscompat_registry.cpp). Classification
// strings are "verified" (real implementation) or "stubbed" (explicit
// unimplemented resolution handler); "stubbed" is never an implementation.
#ifdef __cplusplus
extern "C" {
#endif

const char *iostodroid_compat_classify(const char *darwin_symbol);
void (*iostodroid_compat_resolve(const char *darwin_symbol))(void);
int iostodroid_compat_register_stub(const char *darwin_symbol);
unsigned long iostodroid_compat_entry_count(void);
int iostodroid_compat_entry_at(unsigned long index, const char **darwin_symbol, const char **android_symbol,
                          int *kind, void (**handler)(void));
unsigned long long iostodroid_compat_stub_call_total(void);
unsigned long long iostodroid_compat_stub_call_count(const char *darwin_symbol);

#ifdef __cplusplus
} // extern "C"
#endif
