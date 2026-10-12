#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace ShaderRecompiler;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct ModuleFlags {
    bool earlyFragmentTests = false;
    bool kill = false;
};

ModuleFlags Inspect(std::span<const std::uint32_t> words) {
    Check(words.size() >= 5 && words[0] == spv::MagicNumber, "early fragment kill: missing SPIR-V module");
    ModuleFlags flags;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> spv::WordCountShift;
        Check(count != 0 && count <= words.size() - offset, "early fragment kill: malformed SPIR-V instruction");
        const auto opcode = words[offset] & spv::OpCodeMask;
        if (opcode == spv::OpExecutionMode && count >= 3 && words[offset + 2] == spv::ExecutionModeEarlyFragmentTests) flags.earlyFragmentTests = true;
        if (opcode == spv::OpKill) flags.kill = true;
        offset += count;
    }
    return flags;
}

SpirvTarget FragmentTarget() {
    static constexpr std::array<std::uint32_t, 1> capabilities{spv::CapabilityShader};
    static constexpr std::array<std::string_view, 1> extensions{"SPV_KHR_float_controls"};
    return {0x00401000u, 0x00010400u, 64, 0, capabilities, extensions, false, {1024, 1024, 64}, 1024, 32768, {}, {}};
}

ModuleFlags CompileFragment(bool validMask, bool earlyZ) {
    const std::uint32_t exportWord = validMask ? 0xf800180fu : 0xf800080fu;
    const std::array<std::uint32_t, 4> code{0x7e0e02f2u, exportWord, 0x07070707u, 0xbf810000u};
    const std::array<MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const std::array<std::uint32_t, 8> userData{};
    ShaderPixelStageInfo pixel{};
    pixel.wave32 = false;
    pixel.earlyZ = earlyZ;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}};
    request.context.waveSize = 64;
    request.context.userData = userData;
    request.context.pixel = pixel;
    request.context.memory = memory;
    request.target = FragmentTarget();
    request.layout = {0, 0, 0, 128};
    request.useCache = false;
    return Inspect(Recompile(request).spirv.Words());
}

}

int main() {
    try {
        const auto killed = CompileFragment(true, true);
        Check(killed.kill, "a vm export did not lower to OpKill");
        Check(!killed.earlyFragmentTests, "EarlyFragmentTests was declared on a fragment shader that contains OpKill");
        const auto early = CompileFragment(false, true);
        Check(!early.kill, "an export without VM lowered to OpKill");
        Check(early.earlyFragmentTests, "early-Z without a vm export did not declare EarlyFragmentTests");
        const auto late = CompileFragment(true, false);
        Check(late.kill && !late.earlyFragmentTests, "disabling early-Z changed the vm export lowering");
        std::printf("early fragment kill tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
