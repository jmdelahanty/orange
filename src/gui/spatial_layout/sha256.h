#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout::checksum {

inline std::uint32_t rotate_right(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

inline std::string sha256_hex(const std::string& bytes)
{
    static constexpr std::array<std::uint32_t, 64> constants = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    std::uint32_t h0 = 0x6a09e667u;
    std::uint32_t h1 = 0xbb67ae85u;
    std::uint32_t h2 = 0x3c6ef372u;
    std::uint32_t h3 = 0xa54ff53au;
    std::uint32_t h4 = 0x510e527fu;
    std::uint32_t h5 = 0x9b05688cu;
    std::uint32_t h6 = 0x1f83d9abu;
    std::uint32_t h7 = 0x5be0cd19u;

    std::vector<unsigned char> padded(bytes.begin(), bytes.end());
    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(padded.size()) * 8u;
    padded.push_back(0x80u);
    while ((padded.size() % 64u) != 56u) padded.push_back(0u);
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<unsigned char>(
            (bit_length >> shift) & 0xffu));
    }

    for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64u) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = chunk + index * 4u;
            words[index] =
                (static_cast<std::uint32_t>(padded[offset]) << 24) |
                (static_cast<std::uint32_t>(padded[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(padded[offset + 2]) << 8) |
                static_cast<std::uint32_t>(padded[offset + 3]);
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotate_right(words[index - 15], 7) ^
                rotate_right(words[index - 15], 18) ^
                (words[index - 15] >> 3);
            const std::uint32_t s1 = rotate_right(words[index - 2], 17) ^
                rotate_right(words[index - 2], 19) ^
                (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;
        std::uint32_t f = h5;
        std::uint32_t g = h6;
        std::uint32_t h = h7;
        for (std::size_t index = 0; index < 64; ++index) {
            const std::uint32_t s1 = rotate_right(e, 6) ^
                rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 =
                h + s1 + choose + constants[index] + words[index];
            const std::uint32_t s0 = rotate_right(a, 2) ^
                rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::nouppercase;
    for (const std::uint32_t value : {h0, h1, h2, h3, h4, h5, h6, h7}) {
        out << std::setw(8) << value;
    }
    return out.str();
}

inline bool read_file(const std::filesystem::path& path,
                      std::string* bytes_out,
                      std::string* error_out)
{
    if (bytes_out == nullptr) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "Could not open file: " + path.string();
        return false;
    }
    bytes_out->assign(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        if (error_out) *error_out = "Could not read file: " + path.string();
        return false;
    }
    return true;
}

inline bool file_sha256(const std::filesystem::path& path,
                        std::string* checksum_out,
                        std::string* error_out)
{
    std::string bytes;
    if (checksum_out == nullptr || !read_file(path, &bytes, error_out)) return false;
    *checksum_out = "sha256:" + sha256_hex(bytes);
    return true;
}

}  // namespace orange::gui::spatial_layout::checksum
