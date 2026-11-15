#include "empty_background.h"

#include "fsuid_guard.h"
#include "image_levels.h"
#include "orange_data_root.h"
#include "orange_outputs/orange_empty_background_capture_v1.h"
#include "orange_outputs/orange_recording_empty_background_v1.h"
#include "outputs/common/atomic_file.h"
#include "outputs/common/digest.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <zlib.h>

namespace orange::empty_background {

namespace fs = std::filesystem;
namespace capture_gen = ::orange::outputs::gen::empty_background_capture_v1;
namespace levels_gen = ::orange::outputs::gen::image_levels_v1;
namespace block_gen = ::orange::outputs::gen::recording_empty_background_v1;
using json = nlohmann::json;

namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("empty background: " + message); }

bool is_serial(const std::string& value) {
    static const std::regex pattern("^[0-9]{1,20}$");
    return std::regex_match(value, pattern);
}

bool is_capture_id(const std::string& value) {
    static const std::regex pattern("^[A-Za-z0-9_-]{1,128}$");
    return std::regex_match(value, pattern);
}

std::string image_name(const std::string& serial) { return "Cam" + serial + "_empty_background.png"; }

// Reads a regular file of at most max_bytes; false with a reason otherwise.
bool read_file(const fs::path& path, std::uint64_t max_bytes, std::string* bytes, std::string* error) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status)) {
        *error = path.string() + " is not a regular file";
        return false;
    }
    const auto size = fs::file_size(path, ec);
    if (ec || size > max_bytes) {
        *error = path.string() + (ec ? ": cannot stat" : " is larger than " + std::to_string(max_bytes) + " bytes");
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot open " + path.string();
        return false;
    }
    bytes->assign(static_cast<std::size_t>(size), '\0');
    in.read(bytes->data(), static_cast<std::streamsize>(size));
    if (!in || in.gcount() != static_cast<std::streamsize>(size) || in.peek() != std::char_traits<char>::eof()) {
        *error = "short or changing read of " + path.string();
        return false;
    }
    return true;
}

// ---- PNG helpers ----

constexpr std::array<unsigned char, 8> kPngSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

void put_u32(std::string* out, std::uint32_t v) {
    out->push_back(static_cast<char>((v >> 24) & 0xff));
    out->push_back(static_cast<char>((v >> 16) & 0xff));
    out->push_back(static_cast<char>((v >> 8) & 0xff));
    out->push_back(static_cast<char>(v & 0xff));
}

std::uint32_t get_u32(const std::string& bytes, std::size_t at) {
    const auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i])); };
    return (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
}

void put_chunk(std::string* out, const char type[5], const std::string& data) {
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out->size();
    out->append(type, 4);
    out->append(data);
    const auto crc = ::crc32(0L, reinterpret_cast<const Bytef*>(out->data() + start), static_cast<uInt>(data.size() + 4));
    put_u32(out, static_cast<std::uint32_t>(crc));
}

struct PngChunks {
    std::uint32_t width = 0, height = 0;
    std::string idat;
};

// Walks every chunk: CRCs, IHDR first and 8-bit greyscale without interlace,
// no colour-management or transparency chunk, IEND last. IDAT data is
// collected only when want_idat.
bool walk_png(const std::string& bytes, bool want_idat, PngChunks* out, std::string* error) {
    if (bytes.size() < 8 || std::memcmp(bytes.data(), kPngSignature.data(), 8) != 0) {
        *error = "not a PNG (signature)";
        return false;
    }
    std::size_t at = 8;
    bool saw_ihdr = false, saw_iend = false;
    while (at + 12 <= bytes.size()) {
        const std::uint32_t length = get_u32(bytes, at);
        if (length > bytes.size() - at - 12) {
            *error = "truncated chunk";
            return false;
        }
        const std::string type = bytes.substr(at + 4, 4);
        const auto crc = ::crc32(0L, reinterpret_cast<const Bytef*>(bytes.data() + at + 4), static_cast<uInt>(length + 4));
        if (static_cast<std::uint32_t>(crc) != get_u32(bytes, at + 8 + length)) {
            *error = "chunk " + type + " CRC mismatch";
            return false;
        }
        const std::size_t data = at + 8;
        if (!saw_ihdr) {
            if (type != "IHDR" || length != 13) {
                *error = "first chunk is not IHDR";
                return false;
            }
            out->width = get_u32(bytes, data);
            out->height = get_u32(bytes, data + 4);
            const auto u8 = [&](std::size_t i) { return static_cast<unsigned char>(bytes[data + i]); };
            if (u8(8) != 8 || u8(9) != 0 || u8(10) != 0 || u8(11) != 0 || u8(12) != 0) {
                *error = "IHDR is not 8-bit greyscale, deflate, non-interlaced";
                return false;
            }
            if (out->width == 0 || out->height == 0) {
                *error = "IHDR has a zero dimension";
                return false;
            }
            saw_ihdr = true;
        } else if (type == "gAMA" || type == "iCCP" || type == "sRGB" || type == "cHRM" || type == "tRNS" ||
                   type == "PLTE" || type == "sBIT") {
            *error = "forbidden chunk " + type;
            return false;
        } else if (type == "IDAT") {
            if (want_idat) out->idat.append(bytes, data, length);
        } else if (type == "IEND") {
            saw_iend = true;
            at += 12 + length;
            break;
        }
        at += 12 + length;
    }
    if (!saw_ihdr || !saw_iend || at != bytes.size()) {
        *error = "missing IHDR/IEND or trailing bytes";
        return false;
    }
    return true;
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

std::string utc_iso_millis(std::chrono::system_clock::time_point when) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buffer;
}

// Why the burst's frame identities are not 17 consecutive frames, or empty.
std::string burst_identity_problem(const std::vector<SourceFrame>& frames) {
    if (frames.size() != kFrameCount) return "burst has " + std::to_string(frames.size()) + " of 17 frame identities";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        if (f.stream_frame_id == 0 || f.camera_frame_id == 0 || f.camera_frame_id > 65535 ||
            f.camera_timestamp_ns == 0 || f.timestamp_sys_ns == 0)
            return "burst frame " + std::to_string(i) + " has an invalid identity or timestamp";
        if (i == 0) continue;
        const auto& p = frames[i - 1];
        const std::uint64_t next_camera = p.camera_frame_id == 65535 ? 1 : p.camera_frame_id + 1;
        if (f.stream_frame_id != p.stream_frame_id + 1 || f.camera_frame_id != next_camera)
            return "burst frames are not consecutive (stream " + std::to_string(p.stream_frame_id) + " -> " +
                   std::to_string(f.stream_frame_id) + ", camera " + std::to_string(p.camera_frame_id) + " -> " +
                   std::to_string(f.camera_frame_id) + ")";
    }
    return {};
}

gen::OrangeEmptyBackgroundCaptureV1CamerasItemCameraConfiguration to_gen(const CameraConfiguration& c) {
    gen::OrangeEmptyBackgroundCaptureV1CamerasItemCameraConfiguration out;
    out.width = c.width;
    out.height = c.height;
    out.offset_x = c.offset_x;
    out.offset_y = c.offset_y;
    out.frame_rate_hz = c.frame_rate_hz;
    out.exposure_us = c.exposure_us;
    out.gain = c.gain;
    out.focus = c.focus;
    out.iris = c.iris;
    return out;
}

// Consumer checks JSON Schema cannot express (schema descriptions).
void check_descriptor_rules(const gen::OrangeEmptyBackgroundCaptureV1& d) {
    std::set<std::string> serials;
    for (const auto& camera : d.cameras) {
        if (!serials.insert(camera.camera_serial).second) fail("duplicate camera_serial " + camera.camera_serial);
        if (camera.image && camera.image->relative_path != image_name(camera.camera_serial))
            fail("image " + camera.image->relative_path + " does not name Cam" + camera.camera_serial);
    }
}

gen::OrangeEmptyBackgroundCaptureV1 parse_descriptor(const std::string& bytes) {
    json doc;
    try {
        doc = json::parse(bytes);
    } catch (const std::exception& ex) {
        fail(std::string("descriptor is not JSON: ") + ex.what());
    }
    if (!doc.is_object() || doc.value("schema_id", std::string()) != kCaptureSchemaId ||
        !doc.contains("schema_version") || doc.at("schema_version") != kCaptureSchemaVersion)
        fail("descriptor is not orange.empty_background_capture v1");
    gen::OrangeEmptyBackgroundCaptureV1 descriptor;
    std::string error;
    if (!gen::parse(doc, &descriptor, &error)) fail("descriptor does not match its schema: " + error);
    check_descriptor_rules(descriptor);
    return descriptor;
}

}  // namespace

// ---- median and statistics ------------------------------------------------------------------

BurstMedian MedianOfBurst(const std::vector<std::vector<std::uint8_t>>& frames, std::size_t pixel_count) {
    if (frames.size() != kFrameCount)
        throw std::invalid_argument("median_17 needs exactly 17 frames, got " + std::to_string(frames.size()));
    std::array<const std::uint8_t*, kFrameCount> sources{};
    for (std::size_t f = 0; f < kFrameCount; ++f) {
        if (frames[f].size() < pixel_count)
            throw std::invalid_argument("frame " + std::to_string(f) + " is shorter than the raster");
        sources[f] = frames[f].data();
    }
    BurstMedian out;
    out.median.resize(pixel_count);
    constexpr std::size_t kMiddle = kFrameCount / 2;  // 8: nine values <= it, nine >= it
    // Pixel bands: the 17 samples of kBand pixels sorted lane-wise by an odd-even
    // transposition network (17 rounds sort 17 values exactly; min/max on byte rows
    // vectorizes). Afterwards row 8 is the median and row 16 the per-pixel maximum.
    constexpr std::size_t kBand = 256;
    alignas(64) std::uint8_t rows[kFrameCount][kBand];
    for (std::size_t base = 0; base < pixel_count; base += kBand) {
        const std::size_t n = std::min(kBand, pixel_count - base);
        for (std::size_t f = 0; f < kFrameCount; ++f) std::memcpy(rows[f], sources[f] + base, n);
        for (std::size_t round = 0; round < kFrameCount; ++round) {
            for (std::size_t r = round % 2; r + 1 < kFrameCount; r += 2) {
                std::uint8_t* lo = rows[r];
                std::uint8_t* hi = rows[r + 1];
                for (std::size_t i = 0; i < kBand; ++i) {
                    const std::uint8_t a = lo[i], b = hi[i];
                    lo[i] = a < b ? a : b;
                    hi[i] = a < b ? b : a;
                }
            }
        }
        std::memcpy(out.median.data() + base, rows[kMiddle], n);
        for (std::size_t i = 0; i < n; ++i) out.any_frame_saturated_pixels += rows[kFrameCount - 1][i] == 255 ? 1 : 0;
    }
    return out;
}

levels_gen::OrangeImageLevelsV1 ImageLevels(const std::uint8_t* pixels, std::size_t pixel_count) {
    const auto s = ::orange::image_levels::ComputeStatistics(pixels, pixel_count);
    levels_gen::OrangeImageLevelsV1 out;
    out.pixel_count = s.pixel_count;
    out.saturated_fraction = s.saturated_fraction;
    out.zero_fraction = s.zero_fraction;
    out.mean = s.mean;
    out.p01 = static_cast<std::uint64_t>(s.p01);
    out.p05 = static_cast<std::uint64_t>(s.p05);
    out.p50 = static_cast<std::uint64_t>(s.p50);
    out.p95 = static_cast<std::uint64_t>(s.p95);
    out.p99 = static_cast<std::uint64_t>(s.p99);
    out.p999 = static_cast<std::uint64_t>(s.p999);
    return out;
}

// ---- PNG ------------------------------------------------------------------------------------

std::string EncodePngGray8(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height) {
    if (!pixels || width == 0 || height == 0) throw std::invalid_argument("PNG needs a non-empty image");
    const std::size_t row = static_cast<std::size_t>(width) + 1;
    std::string raw(row * height, '\0');
    for (std::uint32_t y = 0; y < height; ++y)  // filter type 0 (None) on every row
        std::memcpy(raw.data() + y * row + 1, pixels + static_cast<std::size_t>(y) * width, width);
    uLongf compressed_size = ::compressBound(static_cast<uLong>(raw.size()));
    std::string compressed(compressed_size, '\0');
    if (::compress2(reinterpret_cast<Bytef*>(compressed.data()), &compressed_size,
                    reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), 6) != Z_OK)
        throw std::runtime_error("PNG deflate failed");
    compressed.resize(compressed_size);
    std::string out(reinterpret_cast<const char*>(kPngSignature.data()), kPngSignature.size());
    std::string ihdr;
    put_u32(&ihdr, width);
    put_u32(&ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(0);  // greyscale
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    put_chunk(&out, "IHDR", ihdr);
    put_chunk(&out, "IDAT", compressed);
    put_chunk(&out, "IEND", std::string());
    return out;
}

bool ReadPngGray8Header(const std::string& bytes, std::uint32_t* width, std::uint32_t* height, std::string* error) {
    PngChunks chunks;
    if (!walk_png(bytes, false, &chunks, error)) return false;
    *width = chunks.width;
    *height = chunks.height;
    return true;
}

bool DecodePngGray8(const std::string& bytes, std::vector<std::uint8_t>* pixels, std::uint32_t* width,
                    std::uint32_t* height, std::string* error) {
    PngChunks chunks;
    if (!walk_png(bytes, true, &chunks, error)) return false;
    const std::size_t row = static_cast<std::size_t>(chunks.width) + 1;
    const std::size_t expected = row * chunks.height;
    std::string raw(expected, '\0');
    uLongf raw_size = static_cast<uLongf>(expected);
    if (::uncompress(reinterpret_cast<Bytef*>(raw.data()), &raw_size, reinterpret_cast<const Bytef*>(chunks.idat.data()),
                     static_cast<uLong>(chunks.idat.size())) != Z_OK ||
        raw_size != expected) {
        *error = "IDAT does not inflate to the raster";
        return false;
    }
    pixels->assign(static_cast<std::size_t>(chunks.width) * chunks.height, 0);
    for (std::uint32_t y = 0; y < chunks.height; ++y) {
        const auto filter = static_cast<unsigned char>(raw[y * row]);
        const auto* in = reinterpret_cast<const std::uint8_t*>(raw.data() + y * row + 1);
        std::uint8_t* out = pixels->data() + static_cast<std::size_t>(y) * chunks.width;
        const std::uint8_t* up = y > 0 ? out - chunks.width : nullptr;
        for (std::uint32_t x = 0; x < chunks.width; ++x) {
            const int a = x > 0 ? out[x - 1] : 0, b = up ? up[x] : 0, c = (x > 0 && up) ? up[x - 1] : 0;
            int predicted = 0;
            switch (filter) {
                case 0: predicted = 0; break;
                case 1: predicted = a; break;
                case 2: predicted = b; break;
                case 3: predicted = (a + b) / 2; break;
                case 4: predicted = paeth(a, b, c); break;
                default: *error = "unknown PNG filter type"; return false;
            }
            out[x] = static_cast<std::uint8_t>((in[x] + predicted) & 0xff);
        }
    }
    *width = chunks.width;
    *height = chunks.height;
    return true;
}

// ---- capture --------------------------------------------------------------------------------

CaptureReceipt WriteCapture(const CapturePlan& plan, std::vector<CameraBurst>* bursts) {
    if (!bursts || bursts->empty() || bursts->size() > 64) fail("a capture needs 1 to 64 cameras");
    if (!is_capture_id(plan.capture_id)) fail("invalid capture_id '" + plan.capture_id + "'");
    if (plan.store_root.empty() || !plan.store_root.is_absolute()) fail("the store root must be an absolute path");
    std::set<std::string> serials;
    for (const auto& burst : *bursts) {
        if (!is_serial(burst.camera_serial)) fail("invalid camera serial '" + burst.camera_serial + "'");
        if (!serials.insert(burst.camera_serial).second) fail("duplicate camera " + burst.camera_serial);
        if (burst.configuration.pixel_format != "Mono8")
            fail("Cam" + burst.camera_serial + " pixel format " + burst.configuration.pixel_format + " is not Mono8");
        const auto& c = burst.configuration;
        if (c.width == 0 || c.height == 0 || c.width > 65535 || c.height > 65535 || c.offset_x > 65535 ||
            c.offset_y > 65535 || c.width * c.height > kMaxFrameBytes)
            fail("Cam" + burst.camera_serial + " raster is outside the burst limits");
    }

    orange::ScopedFsuid fsuid_guard;
    std::error_code ec;
    fs::create_directories(plan.store_root, ec);
    if (ec) fail("cannot create the store " + plan.store_root.string() + ": " + ec.message());
    const fs::path directory = plan.store_root / plan.capture_id;
    // A capture directory is created once; an existing one is never adopted.
    if (::mkdir(directory.c_str(), 0755) != 0)
        fail("cannot create " + directory.string() + ": " + std::strerror(errno));

    gen::OrangeEmptyBackgroundCaptureV1 descriptor;
    descriptor.capture_id = plan.capture_id;
    descriptor.captured_at_utc = plan.captured_at_utc;
    descriptor.scene_declaration = plan.scene_declaration;
    descriptor.registration_id = plan.registration_id;
    for (auto& burst : *bursts) {
        gen::OrangeEmptyBackgroundCaptureV1CamerasItem camera;
        camera.camera_serial = burst.camera_serial;
        camera.camera_configuration = to_gen(burst.configuration);
        const std::size_t pixel_count = static_cast<std::size_t>(burst.configuration.width * burst.configuration.height);
        std::string failure = burst.failure_reason;
        if (failure.empty()) failure = burst_identity_problem(burst.source_frames);
        if (failure.empty() && burst.frames.size() != kFrameCount)
            failure = "burst has " + std::to_string(burst.frames.size()) + " of 17 frames";
        if (failure.empty())
            for (const auto& frame : burst.frames)
                if (frame.size() != pixel_count) failure = "a burst frame does not match the configured raster";
        if (!failure.empty()) {
            camera.capture_status = "failed";
            camera.failure_reason = failure.substr(0, 1024);
        } else {
            BurstMedian median = MedianOfBurst(burst.frames, pixel_count);
            std::vector<std::vector<std::uint8_t>>().swap(burst.frames);  // source frames are not kept
            const std::string png = EncodePngGray8(median.median.data(), static_cast<std::uint32_t>(burst.configuration.width),
                                                   static_cast<std::uint32_t>(burst.configuration.height));
            if (png.size() > kMaxFileBytes) fail("Cam" + burst.camera_serial + " PNG exceeds " + std::to_string(kMaxFileBytes) + " bytes");
            std::string error;
            if (!outputs::write_file_atomic(directory / image_name(burst.camera_serial), png,
                                            outputs::AtomicWriteOptions::durable(), &error))
                fail("cannot write Cam" + burst.camera_serial + " image: " + error);
            camera.capture_status = "captured";
            std::vector<gen::OrangeEmptyBackgroundCaptureV1CamerasItemSourceFramesItem> frames;
            for (const auto& f : burst.source_frames)
                frames.push_back({f.stream_frame_id, f.camera_frame_id, f.camera_timestamp_ns, f.timestamp_sys_ns});
            camera.source_frames = std::move(frames);
            camera.image = gen::OrangeEmptyBackgroundCaptureV1CamerasItemImage{image_name(burst.camera_serial),
                                                                               png.size(), outputs::sha256_token(png)};
            camera.statistics = ImageLevels(median.median.data(), pixel_count);
            camera.any_frame_saturated_fraction =
                static_cast<double>(median.any_frame_saturated_pixels) / static_cast<double>(pixel_count);
        }
        burst.frames.clear();
        descriptor.cameras.push_back(std::move(camera));
    }

    const json doc = capture_gen::to_json(descriptor);
    const std::string bytes = outputs::json_file_text(doc);
    // The written document must read back under the published schema.
    CaptureReceipt receipt;
    receipt.descriptor = parse_descriptor(bytes);
    receipt.directory = directory;
    receipt.descriptor_path = directory / kDescriptorFileName;
    receipt.descriptor_size_bytes = bytes.size();
    receipt.descriptor_sha256 = outputs::sha256_token(bytes);
    std::string error;
    if (!outputs::write_file_atomic(receipt.descriptor_path, bytes, outputs::AtomicWriteOptions::durable(), &error))
        fail("cannot write the descriptor: " + error);
    return receipt;
}

std::string NewCaptureId(const fs::path& store_root, std::string* utc_iso) {
    const auto now = std::chrono::system_clock::now();
    if (utc_iso) *utc_iso = utc_iso_millis(now);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&seconds, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y_%m_%d_%H_%M_%S", &tm);
    std::string id = buffer;
    std::error_code ec;
    for (int suffix = 1; fs::exists(store_root / id, ec) && suffix < 1000; ++suffix)
        id = std::string(buffer) + "_" + std::to_string(suffix);
    return id;
}

fs::path DefaultStoreRoot(std::string* error) {
    const fs::path root = orange::data_root::resolve(error);
    return root.empty() ? fs::path() : root / kStoreDirectoryName;
}

// ---- reading a capture ----------------------------------------------------------------------

StoredCapture ReadCapture(const fs::path& descriptor_path) {
    if (!descriptor_path.is_absolute() || descriptor_path.filename() != kDescriptorFileName)
        fail("a capture is named by the absolute path of its background.json, not " + descriptor_path.string());
    StoredCapture out;
    out.descriptor_path = descriptor_path.lexically_normal();
    std::string error;
    if (!read_file(out.descriptor_path, kMaxFileBytes, &out.descriptor_bytes, &error)) fail(error);
    out.descriptor_sha256 = outputs::sha256_token(out.descriptor_bytes);
    out.descriptor = parse_descriptor(out.descriptor_bytes);
    if (out.descriptor_path.parent_path().filename() != out.descriptor.capture_id)
        fail("capture directory " + out.descriptor_path.parent_path().string() + " is not named after capture_id " +
             out.descriptor.capture_id);
    return out;
}

std::optional<StoredCapture> NewestCapture(const fs::path& store_root) {
    std::optional<StoredCapture> newest;
    std::error_code ec;
    if (!fs::is_directory(store_root, ec)) return newest;
    for (const auto& entry : fs::directory_iterator(store_root, ec)) {
        if (!entry.is_directory(ec)) continue;
        try {
            StoredCapture capture = ReadCapture(fs::absolute(entry.path()) / kDescriptorFileName);
            if (!newest || capture.descriptor.captured_at_utc > newest->descriptor.captured_at_utc ||
                (capture.descriptor.captured_at_utc == newest->descriptor.captured_at_utc &&
                 capture.descriptor.capture_id > newest->descriptor.capture_id))
                newest = std::move(capture);
        } catch (const std::exception&) {
            // Not a capture (or an incomplete one without background.json).
        }
    }
    return newest;
}

// ---- record-start binding -------------------------------------------------------------------

BindingResult BindToRecording(const fs::path& recording_folder, const BindingSelection& selection,
                              const std::vector<RecordingCamera>& cameras) {
    if (cameras.empty()) fail("a recording needs at least one camera");
    std::set<std::string> serials;
    for (const auto& camera : cameras) {
        if (!is_serial(camera.camera_serial)) fail("invalid recording camera serial '" + camera.camera_serial + "'");
        if (!serials.insert(camera.camera_serial).second) fail("duplicate recording camera " + camera.camera_serial);
    }
    BindingResult result;
    if (!selection.selected) {
        block_gen::OrangeRecordingEmptyBackgroundV1NoCapture none;
        for (const auto& camera : cameras) none.cameras[camera.camera_serial] = {};
        result.block = block_gen::to_json(block_gen::OrangeRecordingEmptyBackgroundV1{none});
        return result;
    }
    if (selection.selection_source != "gui_operator" && selection.selection_source != "headless_spec")
        fail("unknown selection source '" + selection.selection_source + "'");
    if (!selection.scene_unchanged_confirmed)
        fail("a selected capture needs the per-recording scene-unchanged confirmation");
    const StoredCapture capture = ReadCapture(selection.descriptor_path);
    const fs::path source_directory = capture.descriptor_path.parent_path();

    orange::ScopedFsuid fsuid_guard;
    const fs::path target = recording_folder / kRecordingDirectoryName;
    // The recording copy is created once; an existing directory is never adopted.
    if (::mkdir(target.c_str(), 0755) != 0) fail("cannot create " + target.string() + ": " + std::strerror(errno));
    // A selection-level failure below leaves no partial copy behind.
    struct RemoveUnlessKept {
        fs::path path;
        bool keep = false;
        ~RemoveUnlessKept() {
            std::error_code ec;
            if (!keep) fs::remove_all(path, ec);
        }
    } partial{target};
    std::string error;
    if (!outputs::write_file_atomic(target / kDescriptorFileName, capture.descriptor_bytes, {}, &error))
        fail("cannot copy the descriptor: " + error);
    std::string copied;
    if (!read_file(target / kDescriptorFileName, kMaxFileBytes, &copied, &error) ||
        outputs::sha256_token(copied) != capture.descriptor_sha256) {
        std::error_code ec;
        fs::remove(target / kDescriptorFileName, ec);
        fail("descriptor copy does not verify" + (error.empty() ? std::string() : ": " + error));
    }

    std::map<std::string, const gen::OrangeEmptyBackgroundCaptureV1CamerasItem*> by_serial;
    for (const auto& entry : capture.descriptor.cameras) by_serial[entry.camera_serial] = &entry;

    block_gen::OrangeRecordingEmptyBackgroundV1CaptureAttached attached;
    attached.selection_source = selection.selection_source;
    attached.capture_id = capture.descriptor.capture_id;
    attached.source_descriptor_path = capture.descriptor_path.string();
    attached.descriptor = {capture.descriptor_bytes.size(), capture.descriptor_sha256};
    for (const auto& camera : cameras) {
        block_gen::OrangeRecordingEmptyBackgroundV1CaptureAttachedCamerasValue status;
        const auto found = by_serial.find(camera.camera_serial);
        if (found == by_serial.end() || found->second->capture_status != "captured" || !found->second->image) {
            status.status = "missing_in_capture";
            if (found != by_serial.end() && found->second->failure_reason) status.failure_reason = found->second->failure_reason;
            result.warnings.push_back("Cam" + camera.camera_serial + ": no image in capture " + capture.descriptor.capture_id +
                                      (status.failure_reason ? " (" + *status.failure_reason + ")" : ""));
            attached.cameras[camera.camera_serial] = std::move(status);
            continue;
        }
        const auto& entry = *found->second;
        const auto& image = *entry.image;
        const auto copy_failed = [&](const std::string& reason) {
            block_gen::OrangeRecordingEmptyBackgroundV1CaptureAttachedCamerasValue failed;
            failed.status = "copy_failed";
            failed.failure_reason = reason.substr(0, 1024);
            result.warnings.push_back("Cam" + camera.camera_serial + ": image not attached: " + reason);
            std::error_code ec;
            fs::remove(target / image.relative_path, ec);
            attached.cameras[camera.camera_serial] = std::move(failed);
        };
        if (camera.configuration.pixel_format != "Mono8") {
            copy_failed("recording pixel format " + camera.configuration.pixel_format + " is not the capture's Mono8");
            continue;
        }
        std::string bytes;
        if (!read_file(source_directory / image.relative_path, kMaxFileBytes, &bytes, &error)) {
            copy_failed("cannot read the source image: " + error);
            continue;
        }
        if (bytes.size() != image.size_bytes || outputs::sha256_token(bytes) != image.sha256) {
            copy_failed("source image does not match the descriptor's size or sha256");
            continue;
        }
        std::uint32_t png_width = 0, png_height = 0;
        if (!ReadPngGray8Header(bytes, &png_width, &png_height, &error) ||
            png_width != entry.camera_configuration.width || png_height != entry.camera_configuration.height) {
            copy_failed("source image is not an 8-bit greyscale PNG of the descriptor's raster" +
                        (error.empty() ? std::string() : ": " + error));
            continue;
        }
        if (!outputs::write_file_atomic(target / image.relative_path, bytes, {}, &error)) {
            copy_failed("cannot write the copy: " + error);
            continue;
        }
        if (!read_file(target / image.relative_path, kMaxFileBytes, &copied, &error) ||
            outputs::sha256_token(copied) != image.sha256) {
            copy_failed("the copy does not verify against the descriptor's sha256");
            continue;
        }
        status.image = block_gen::OrangeRecordingEmptyBackgroundV1CaptureAttachedCamerasValueImage{
            image.relative_path, image.size_bytes, image.sha256};
        // Checked settings, in the schema's order; frame rate is recorded, not checked.
        const auto& was = entry.camera_configuration;
        const auto& now = camera.configuration;
        std::vector<block_gen::OrangeRecordingEmptyBackgroundV1CaptureAttachedCamerasValueSettingsDifferencesItem> diffs;
        const std::pair<const char*, std::pair<std::uint64_t, std::uint64_t>> checked[] = {
            {"exposure_us", {was.exposure_us, now.exposure_us}}, {"gain", {was.gain, now.gain}},
            {"iris", {was.iris, now.iris}},                      {"focus", {was.focus, now.focus}},
            {"width", {was.width, now.width}},                   {"height", {was.height, now.height}},
            {"offset_x", {was.offset_x, now.offset_x}},          {"offset_y", {was.offset_y, now.offset_y}}};
        std::string listed;
        for (const auto& [name, values] : checked) {
            if (values.first == values.second) continue;
            diffs.push_back({name, values.first, values.second});
            listed += std::string(listed.empty() ? "" : ", ") + name + " " + std::to_string(values.first) + " -> " +
                      std::to_string(values.second);
        }
        if (diffs.empty()) {
            status.status = "attached";
        } else {
            status.status = "settings_changed";
            status.settings_differences = std::move(diffs);
            result.warnings.push_back("Cam" + camera.camera_serial + ": settings changed since the background (" + listed +
                                      "); image copied and flagged");
        }
        attached.cameras[camera.camera_serial] = std::move(status);
    }
    result.block = block_gen::to_json(block_gen::OrangeRecordingEmptyBackgroundV1{attached});
    block_gen::OrangeRecordingEmptyBackgroundV1 check;
    if (!block_gen::parse(result.block, &check, &error)) fail("start-snapshot block does not match its schema: " + error);
    partial.keep = true;
    return result;
}

BindingSelection SelectionFromHeadlessSpec(const json& value) {
    if (!value.is_string())
        fail(std::string("fixed.") + kHeadlessSpecKey + " must be the absolute path of a capture's background.json");
    const fs::path path(value.get<std::string>());
    if (!path.is_absolute() || path.lexically_normal() != path)
        fail(std::string("fixed.") + kHeadlessSpecKey + " must be an absolute, normal path: " + path.string());
    ReadCapture(path);  // refuse an unreadable or invalid capture before any camera opens
    BindingSelection selection;
    selection.selected = true;
    selection.selection_source = "headless_spec";
    selection.scene_unchanged_confirmed = true;
    selection.descriptor_path = path;
    return selection;
}

BindingResult WriteRecordingBinding(const std::string& recording_folder, const BindingSelection& selection,
                                    const std::vector<RecordingCamera>& cameras,
                                    const SnapshotSessionUpdate& update_snapshot) {
    BindingResult result = BindToRecording(recording_folder, selection, cameras);
    if (!update_snapshot || !update_snapshot(recording_folder, {{kSnapshotSessionKey, result.block}}))
        fail("cannot write session.empty_background into the start snapshot");
    return result;
}

}  // namespace orange::empty_background
