#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace orange::calibration {

inline std::string format_fnv1a64_fingerprint(std::uint64_t hash)
{
    std::ostringstream out;
    out << "fnv1a64:"
        << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
        << hash;
    return out.str();
}

} // namespace orange::calibration
