#include "prx/libc/include/General.hpp"
#include "prx/libc/include/HeapDiagnostics.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include "prx/libc/include/exceptions/Unwind.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" _Unwind_Reason_Code APS5_VABI _Unwind_Backtrace_nid_postfix(_Unwind_Trace_Fn, void*);

namespace {

std::uint64_t mspaceAtomicIdMask = 0;
std::array<std::uint64_t, 64> mstateTable{};

static_assert(sizeof(LibcHeapInfo) == 32);
static_assert(offsetof(LibcHeapInfo, mspace_atomic_id_mask) == 16);
static_assert(offsetof(LibcHeapInfo, mstate_table) == 24);

struct BacktraceFrame {
    std::string name;
    std::uint32_t offset;
    std::size_t Size() const { return (name.size() + 24) & ~std::size_t{7}; }
};

std::string frameName(std::uintptr_t region) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<void*>(region), &memory, sizeof(memory)) || memory.Type != MEM_IMAGE) return {};
    const auto* base = static_cast<const unsigned char*>(memory.AllocationBase);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return {};
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        if (ordinals[i] >= exports->NumberOfFunctions) continue;
        if (reinterpret_cast<std::uintptr_t>(base + functions[ordinals[i]]) == region)
            return std::string(reinterpret_cast<const char*>(base + names[i])).substr(0, 255);
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(region), &info) && info.dli_sname && reinterpret_cast<std::uintptr_t>(info.dli_saddr) == region)
        return std::string(info.dli_sname).substr(0, 255);
#endif
    return {};
}

struct BacktraceCapture {
    std::uintptr_t caller;
    std::uint32_t limit;
    bool started = false;
    std::vector<BacktraceFrame> frames;
};

_Unwind_Reason_Code captureFrame(_Unwind_Context* context, void* argument) {
    auto& capture = *static_cast<BacktraceCapture*>(argument);
    if (!capture.started) capture.started = context->registers[16] == capture.caller;
    if (!capture.started) return _URC_NO_REASON;
    if (capture.frames.size() >= capture.limit) return _URC_END_OF_STACK;
    auto name = frameName(context->region);
    if (name.empty() && !capture.frames.empty() && capture.frames.back().name.empty()) return _URC_NO_REASON;
    const auto offset = name.empty() ? 0 : static_cast<std::uint32_t>(context->registers[16] - context->region);
    capture.frames.push_back({std::move(name), offset});
    return _URC_NO_REASON;
}

bool captureBacktrace(BacktraceCapture& capture) {
    if (capture.limit == 0) return true;
    const auto status = _Unwind_Backtrace_nid_postfix(captureFrame, &capture);
    return capture.started && status == _URC_END_OF_STACK;
}

}

extern "C" {

void LibcHeapTraceInfo_nid_no_patch(LibcHeapInfo* info) {
    if (info == nullptr)
        throw std::invalid_argument("heap trace info: null output");
    if (info->size != sizeof(LibcHeapInfo))
        throw std::invalid_argument("heap trace info: unsupported structure size");
    info->unknown2 = 0;
    info->mspace_atomic_id_mask = &mspaceAtomicIdMask;
    info->mstate_table = mstateTable.data();
}

// unknown signature
void APS5_VABI sceLibcInternalBacktraceForGame_nid_postfix(const char* heapName) {
    (void)heapName;
    NotImplemented_nid_no_patch(__func__);
}

// unknown signature
void APS5_VABI sceLibcInternalHeapErrorReportForGame_nid_postfix(void* heap, void* block, std::uint32_t error) {
    (void)heap;
    (void)block;
    (void)error;
    NotImplemented_nid_no_patch(__func__);
}

int APS5_VABI sceLibcBacktraceGetBufferSize_nid_postfix(std::uint32_t limit, std::uint32_t* count, std::size_t* size) {
    if (count == nullptr || size == nullptr) return 1;
    BacktraceCapture capture{reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)), limit};
    if (!captureBacktrace(capture)) return 1;
    std::size_t bytes = 0;
    for (const auto& frame : capture.frames) bytes += frame.Size();
    *count = static_cast<std::uint32_t>(capture.frames.size());
    *size = bytes;
    return 0;
}

int APS5_VABI sceLibcBacktraceSelf_nid_postfix(std::uint32_t limit, void* buffer, std::size_t size, std::uint32_t* count) {
    if (buffer == nullptr || count == nullptr) return 1;
    BacktraceCapture capture{reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)), limit};
    if (!captureBacktrace(capture)) return 1;
    auto* output = static_cast<std::byte*>(buffer);
    std::byte* previous = nullptr;
    std::uint32_t written = 0;
    for (const auto& frame : capture.frames) {
        const auto bytes = frame.Size();
        if (bytes > size) break;
        std::memset(output, 0, bytes);
        std::memcpy(output + 8, &frame.offset, sizeof(frame.offset));
        std::memcpy(output + 16, frame.name.c_str(), frame.name.size() + 1);
        if (previous != nullptr) std::memcpy(previous, &output, sizeof(output));
        previous = output;
        output += bytes;
        size -= bytes;
        ++written;
    }
    *count = written;
    return 0;
}

}
