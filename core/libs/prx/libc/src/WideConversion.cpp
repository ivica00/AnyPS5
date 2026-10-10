#include "prx/libc/include/general/VabiMacros.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>

extern "C" std::size_t APS5_VABI wcstombs_nid_postfix(char* destination, const std::uint16_t* source, std::size_t capacity) {
    std::size_t count = 0;
    while (!destination || count < capacity) {
        const auto value = source[count];
        if (value > 255) {
            errno = 86;
            return static_cast<std::size_t>(-1);
        }
        if (destination) destination[count] = static_cast<char>(value);
        if (value == 0) break;
        ++count;
    }
    return count;
}

extern "C" std::size_t APS5_VABI mbstowcs_nid_postfix(std::uint16_t* destination, const char* source, std::size_t capacity) {
    std::size_t count = 0;
    while (!destination || count < capacity) {
        const auto value = static_cast<unsigned char>(source[count]);
        if (destination) destination[count] = value;
        if (value == 0) break;
        ++count;
    }
    return count;
}

