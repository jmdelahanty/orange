#include "fnv1a64_fingerprint.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        require(
            orange::calibration::format_fnv1a64_fingerprint(
                UINT64_C(0x06608621ba48ecf2)) ==
                "fnv1a64:06608621ba48ecf2",
            "FNV-1a fingerprints must retain leading zeroes");
        require(
            orange::calibration::format_fnv1a64_fingerprint(UINT64_C(0)) ==
                "fnv1a64:0000000000000000",
            "zero must use the canonical 16-digit representation");
        require(
            orange::calibration::format_fnv1a64_fingerprint(UINT64_MAX) ==
                "fnv1a64:ffffffffffffffff",
            "fingerprints must be lowercase hexadecimal");
        std::cout << "fnv1a64_fingerprint_tests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fnv1a64_fingerprint_tests: FAIL: " << ex.what() << '\n';
        return 1;
    }
}
