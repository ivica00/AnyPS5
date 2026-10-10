#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <cstdio>

extern "C" {
int APS5_VABI sceLibcBacktraceGetBufferSize_nid_postfix(std::uint32_t, std::uint32_t*, std::size_t*);
int APS5_VABI sceLibcBacktraceSelf_nid_postfix(std::uint32_t, void*, std::size_t, std::uint32_t*);
}

void Require(bool value) {
    if (!value) throw std::runtime_error("backtrace check failed");
}

int main() {
    std::uint32_t count = 99;
    std::size_t size = 99;
    Require(sceLibcBacktraceGetBufferSize_nid_postfix(0, &count, &size) == 0 && count == 0 && size == 0);
    Require(sceLibcBacktraceGetBufferSize_nid_postfix(1, nullptr, &size) == 1);
    Require(sceLibcBacktraceGetBufferSize_nid_postfix(1, &count, nullptr) == 1);
    Require(sceLibcBacktraceGetBufferSize_nid_postfix(1, &count, &size) == 0 && count == 1 && size >= 24 && size % 8 == 0);
    alignas(8) std::array<unsigned char, 2048> buffer;
    buffer.fill(0xa5);
    Require(sceLibcBacktraceSelf_nid_postfix(1, nullptr, buffer.size(), &count) == 1);
    Require(sceLibcBacktraceSelf_nid_postfix(1, buffer.data(), buffer.size(), nullptr) == 1);
    Require(sceLibcBacktraceSelf_nid_postfix(0, buffer.data(), buffer.size(), &count) == 0 && count == 0);
    Require(buffer[0] == 0xa5);
    Require(sceLibcBacktraceSelf_nid_postfix(1, buffer.data(), 23, &count) == 0 && count == 0);
    Require(buffer[0] == 0xa5 && buffer[23] == 0xa5);
    Require(sceLibcBacktraceSelf_nid_postfix(1, buffer.data(), size, &count) == 0 && count == 1);
    std::uintptr_t next = 1;
    std::memcpy(&next, buffer.data(), sizeof(next));
    Require(next == 0 && buffer[size] == 0xa5);
    Require(std::memchr(buffer.data() + 16, 0, size - 16) != nullptr);
    Require(sceLibcBacktraceGetBufferSize_nid_postfix(UINT32_MAX, &count, &size) == 0 && count > 0);
    std::printf("backtrace: %u records, %zu bytes\n", count, size);
}
