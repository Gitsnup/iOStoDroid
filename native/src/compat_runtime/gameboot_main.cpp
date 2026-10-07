// Host boot-attempt probe: loads one 32-bit ARM Mach-O main executable, binds
// unimplemented imports to abort-on-call traps, and executes real guest
// instructions until the first actually-used missing import.
//
// This is a measurement tool, not a game: the JSON report keeps status
// "not_runnable" and carries the executed-instruction count plus the stopping
// import. Exit code 0 means a report was written (even when blocked); 1 means
// the probe itself failed (usage or I/O error).
#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/cpu.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/runner.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/sjlj_unwind.hpp"
#include "compat_runtime/trap_shims.hpp"

#include "json.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaximumMainBinaryBytes = 256u * 1024u * 1024u;

std::vector<std::uint8_t> readMainBinary(const char *path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error(std::string("cannot open Mach-O input: ") + path);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumMainBinaryBytes)
        throw std::runtime_error("Mach-O input is empty or exceeds the 256 MiB limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char *>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read Mach-O input");
    return bytes;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: radek-gameboot <macho-main-executable>\n";
        return 1;
    }
    try {
        const auto bytes = readMainBinary(argv[1]);
        radek::compat_runtime::ShimRegistry shims;
        radek::compat_runtime::objc::ShimAdapter objcShims;
        radek::compat_runtime::audio::ShimAdapter audioShims;
        radek::compat_runtime::SjLjUnwindAdapter sjljUnwind;
        objcShims.registerBindings(shims);
        audioShims.registerBindings(shims);
        sjljUnwind.registerBindings(shims);
        radek::compat_runtime::TrapShimAdapter traps;
        const auto cpu = radek::compat_runtime::createArm32CpuBackend();
        radek::compat_runtime::BootAttemptRunner runner(shims, *cpu, traps);
        const radek::Json report = runner.run(bytes, true);
        std::cout << report.dump() << "\n";

        const auto &fields = report.fields;
        const auto status = fields.find("status");
        const auto reason = fields.find("reason");
        const auto execution = fields.find("execution");
        std::string instructions = "?";
        if (execution != fields.end()) {
            const auto count = execution->second.fields.find("instructions");
            if (count != execution->second.fields.end())
                instructions = count->second.value;
        }
        std::cerr << "gameboot: status="
                  << (status != fields.end() ? status->second.value : "?")
                  << " instructions=" << instructions << " reason="
                  << (reason != fields.end() ? reason->second.value : "?") << "\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "gameboot failed: " << error.what() << "\n";
        return 1;
    }
}
