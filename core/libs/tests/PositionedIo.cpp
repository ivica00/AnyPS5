#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern "C" {
std::int64_t APS5_VABI sceKernelPread(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI sceKernelPwrite(int, const void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
}

int main() {
    const auto require = [](bool condition) {
        if (!condition) throw std::runtime_error("positioned I/O check failed");
    };
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::tmpfile(), &std::fclose);
    require(file != nullptr);
#ifdef _WIN32
    const int descriptor = _fileno(file.get());
#else
    const int descriptor = fileno(file.get());
#endif
    constexpr std::size_t count = 8192;
    std::array<std::uint64_t, count> expected{};
    for (std::size_t i = 0; i < count; ++i) expected[i] = 0x9e3779b900000000ull + i;
    require(sceKernelPwrite(descriptor, expected.data(), sizeof(expected), 0) == sizeof(expected));
    require(sceKernelLseek(descriptor, 0, SEEK_SET) == 0);
    std::barrier start(5);
    std::atomic<bool> failed = false;
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < count; ++i) {
                const auto index = (i * 97 + worker * 131) % count;
                std::uint64_t value = 0;
                if (sceKernelPread(descriptor, &value, sizeof(value), index * sizeof(value)) != sizeof(value) || value != expected[index]) failed = true;
                const auto offset = sizeof(expected) + worker * sizeof(value);
                if (sceKernelPwrite(descriptor, &value, sizeof(value), offset) != sizeof(value)) failed = true;
            }
        });
    }
    start.arrive_and_wait();
    for (const auto value : expected) {
        std::uint64_t actual = 0;
        if (sceKernelRead(descriptor, &actual, sizeof(actual)) != sizeof(actual) || actual != value) failed = true;
    }
    workers.clear();
    require(!failed);
    require(sceKernelLseek(descriptor, 0, SEEK_CUR) == sizeof(expected));
    std::uint64_t value = 0;
    require(sceKernelPread(descriptor, &value, sizeof(value), sizeof(expected) + 32) == 0);
    require(sceKernelLseek(descriptor, 0, SEEK_CUR) == sizeof(expected));
    std::puts("positioned I/O: PASS");
}
