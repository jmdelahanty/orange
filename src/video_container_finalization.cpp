#include "video_container_finalization.h"

#include "fsuid_guard.h"
#include "json.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace OrangeVideoContainerFinalization {
namespace {

namespace fs = std::filesystem;

constexpr char kSchemaId[] = "orange.video_container_finalization";
constexpr int kSchemaVersion = 1;
constexpr std::uint32_t kMdtaUtf8Type = 1;
constexpr std::uint32_t kMdtaUnsignedIntegerType = 22;

constexpr std::uint32_t FourCc(char a, char b, char c, char d) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(d));
}

std::uint32_t ReadBigEndian32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t ReadBigEndian64(const std::uint8_t* bytes) {
    return (static_cast<std::uint64_t>(ReadBigEndian32(bytes)) << 32) |
           ReadBigEndian32(bytes + 4);
}

struct Mp4BoxLocation {
    std::uint64_t offset = 0;
    std::uint64_t header_size = 0;
    std::uint64_t size = 0;
};

bool ReadAt(std::fstream& file,
            std::uint64_t offset,
            std::uint8_t* bytes,
            std::size_t size) {
    if (offset >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(bytes),
              static_cast<std::streamsize>(size));
    return file.gcount() == static_cast<std::streamsize>(size);
}

bool ReadBoxAt(std::fstream& file,
               std::uint64_t offset,
               std::uint64_t parent_end,
               Mp4BoxLocation* box,
               std::uint32_t* type) {
    if (!box || !type || offset > parent_end || parent_end - offset < 8) {
        return false;
    }

    std::array<std::uint8_t, 16> header{};
    if (!ReadAt(file, offset, header.data(), 8)) {
        return false;
    }
    const std::uint32_t compact_size = ReadBigEndian32(header.data());
    *type = ReadBigEndian32(header.data() + 4);
    std::uint64_t header_size = 8;
    std::uint64_t box_size = compact_size;
    if (compact_size == 1) {
        if (parent_end - offset < 16 ||
            !ReadAt(file, offset + 8, header.data() + 8, 8)) {
            return false;
        }
        header_size = 16;
        box_size = ReadBigEndian64(header.data() + 8);
    } else if (compact_size == 0) {
        box_size = parent_end - offset;
    }
    if (box_size < header_size || box_size > parent_end - offset) {
        return false;
    }
    *box = {offset, header_size, box_size};
    return true;
}

bool FindChildBox(std::fstream& file,
                  std::uint64_t begin,
                  std::uint64_t end,
                  std::uint32_t wanted_type,
                  Mp4BoxLocation* found) {
    std::uint64_t offset = begin;
    while (offset < end) {
        Mp4BoxLocation box;
        std::uint32_t type = 0;
        if (!ReadBoxAt(file, offset, end, &box, &type)) {
            return false;
        }
        if (type == wanted_type) {
            if (found) {
                *found = box;
            }
            return true;
        }
        offset += box.size;
    }
    return false;
}

bool FindMetadataKeyIndex(std::fstream& file,
                          const Mp4BoxLocation& keys,
                          const std::string& wanted_key,
                          std::uint32_t* key_index) {
    const std::uint64_t payload = keys.offset + keys.header_size;
    const std::uint64_t end = keys.offset + keys.size;
    if (!key_index || end - payload < 8) {
        return false;
    }

    std::array<std::uint8_t, 8> header{};
    if (!ReadAt(file, payload, header.data(), header.size())) {
        return false;
    }
    const std::uint32_t entry_count = ReadBigEndian32(header.data() + 4);
    std::uint64_t offset = payload + 8;
    for (std::uint32_t index = 1; index <= entry_count; ++index) {
        if (offset > end || end - offset < 8 ||
            !ReadAt(file, offset, header.data(), header.size())) {
            return false;
        }
        const std::uint32_t entry_size = ReadBigEndian32(header.data());
        const std::uint32_t key_namespace = ReadBigEndian32(header.data() + 4);
        if (entry_size < 8 || entry_size > end - offset) {
            return false;
        }
        const std::size_t key_size = static_cast<std::size_t>(entry_size - 8);
        std::vector<std::uint8_t> key_bytes(key_size);
        if (key_size > 0 &&
            !ReadAt(file, offset + 8, key_bytes.data(), key_bytes.size())) {
            return false;
        }
        if (key_namespace == FourCc('m', 'd', 't', 'a') &&
            std::string(key_bytes.begin(), key_bytes.end()) == wanted_key) {
            *key_index = index;
            return true;
        }
        offset += entry_size;
    }
    return false;
}

std::string CurrentUtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool WriteAll(int fd, const std::string& bytes, std::string* error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count =
            ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (error) {
                *error = "temporary_write_failed:" + std::to_string(errno);
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool WriteJsonAtomically(const fs::path& path,
                         const nlohmann::json& value,
                         std::string* error) {
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    fs::path parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        if (error) {
            *error = "create_parent_failed:" + ec.message();
        }
        return false;
    }

    static std::atomic<std::uint64_t> sequence{0};
    const fs::path temporary =
        parent /
        ("." + path.filename().string() + "." +
         std::to_string(static_cast<unsigned long long>(::getpid())) + "." +
         std::to_string(sequence.fetch_add(1)) + ".tmp");
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (error) {
            *error = "temporary_open_failed:" + std::to_string(errno);
        }
        return false;
    }
    const std::string bytes = value.dump(2) + "\n";
    bool ok = WriteAll(fd, bytes, error);
    if (ok && ::fsync(fd) != 0) {
        ok = false;
        if (error) {
            *error = "temporary_fsync_failed:" + std::to_string(errno);
        }
    }
    if (::close(fd) != 0 && ok) {
        ok = false;
        if (error) {
            *error = "temporary_close_failed:" + std::to_string(errno);
        }
    }
    if (!ok) {
        fs::remove(temporary, ec);
        return false;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        if (error) {
            *error = "atomic_rename_failed:" + std::to_string(errno);
        }
        fs::remove(temporary, ec);
        return false;
    }
    const int parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (parent_fd >= 0) {
        ::fsync(parent_fd);
        ::close(parent_fd);
    }
    return true;
}

nlohmann::json NullableErrorCode(const std::optional<int>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json NullableError(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

}  // namespace

Status ClassifyTerminalStatus(const Outcome& outcome) {
    const bool container_finalized =
        outcome.header_written && outcome.trailer_written &&
        outcome.output_closed;
    if (!container_finalized) {
        return Status::ContainerFinalizationFailed;
    }
    if (!outcome.playback_intent_patch_applied) {
        return Status::DegradedPlaybackIntentUnpatched;
    }
    return Status::Complete;
}

const char* StatusName(Status status) {
    switch (status) {
        case Status::RecordingOpen:
            return "recording_open";
        case Status::Finalizing:
            return "finalizing";
        case Status::Complete:
            return "complete";
        case Status::DegradedPlaybackIntentUnpatched:
            return "degraded_playback_intent_unpatched";
        case Status::ContainerFinalizationFailed:
            return "container_finalization_failed";
    }
    return "container_finalization_failed";
}

bool IsTerminal(Status status) {
    return status == Status::Complete ||
           status == Status::DegradedPlaybackIntentUnpatched ||
           status == Status::ContainerFinalizationFailed;
}

fs::path SidecarPathFor(const fs::path& video_path) {
    return fs::path(video_path.string() + ".finalization.json");
}

bool PatchFullFrameRatePlaybackIntent(const fs::path& video_path,
                                      std::string* error) {
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    std::fstream file(video_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        if (error) {
            *error = "could not reopen finalized MP4";
        }
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff stream_size = file.tellg();
    if (stream_size < 0) {
        if (error) {
            *error = "could not determine finalized MP4 size";
        }
        return false;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(stream_size);

    Mp4BoxLocation moov;
    Mp4BoxLocation udta;
    Mp4BoxLocation meta;
    Mp4BoxLocation keys;
    Mp4BoxLocation ilst;
    if (!FindChildBox(file, 0, file_size, FourCc('m', 'o', 'o', 'v'), &moov) ||
        !FindChildBox(file, moov.offset + moov.header_size,
                      moov.offset + moov.size, FourCc('u', 'd', 't', 'a'),
                      &udta) ||
        !FindChildBox(file, udta.offset + udta.header_size,
                      udta.offset + udta.size, FourCc('m', 'e', 't', 'a'),
                      &meta)) {
        if (error) {
            *error = "could not locate moov/udta/meta";
        }
        return false;
    }

    const std::uint64_t meta_children =
        meta.offset + meta.header_size + 4;  // full-box version/flags
    if (meta_children > meta.offset + meta.size ||
        !FindChildBox(file, meta_children, meta.offset + meta.size,
                      FourCc('k', 'e', 'y', 's'), &keys) ||
        !FindChildBox(file, meta_children, meta.offset + meta.size,
                      FourCc('i', 'l', 's', 't'), &ilst)) {
        if (error) {
            *error = "could not locate QuickTime metadata keys/ilst";
        }
        return false;
    }

    std::uint32_t key_index = 0;
    if (!FindMetadataKeyIndex(file, keys, kFullFrameRatePlaybackIntentKey,
                              &key_index)) {
        if (error) {
            *error = "could not locate full-frame-rate playback-intent key";
        }
        return false;
    }

    Mp4BoxLocation item;
    Mp4BoxLocation data;
    if (!FindChildBox(file, ilst.offset + ilst.header_size,
                      ilst.offset + ilst.size, key_index, &item) ||
        !FindChildBox(file, item.offset + item.header_size,
                      item.offset + item.size, FourCc('d', 'a', 't', 'a'),
                      &data)) {
        if (error) {
            *error = "could not locate full-frame-rate playback-intent data atom";
        }
        return false;
    }

    const std::uint64_t data_payload = data.offset + data.header_size;
    if (data.size != data.header_size + 9) {
        if (error) {
            *error = "unexpected full-frame-rate playback-intent payload size";
        }
        return false;
    }
    std::array<std::uint8_t, 9> existing{};
    if (!ReadAt(file, data_payload, existing.data(), existing.size()) ||
        ReadBigEndian32(existing.data()) != kMdtaUtf8Type ||
        ReadBigEndian32(existing.data() + 4) != 0 || existing[8] != '1') {
        if (error) {
            *error = "unexpected full-frame-rate playback-intent source value";
        }
        return false;
    }

    const std::array<std::uint8_t, 9> typed_value = {
        0, 0, 0, static_cast<std::uint8_t>(kMdtaUnsignedIntegerType),
        0, 0, 0, 0,
        1,
    };
    file.clear();
    file.seekp(static_cast<std::streamoff>(data_payload), std::ios::beg);
    file.write(reinterpret_cast<const char*>(typed_value.data()),
               static_cast<std::streamsize>(typed_value.size()));
    file.flush();
    if (!file) {
        if (error) {
            *error = "failed to write typed full-frame-rate playback intent";
        }
        return false;
    }
    std::array<std::uint8_t, 9> verified{};
    if (!ReadAt(file, data_payload, verified.data(), verified.size()) ||
        verified != typed_value) {
        if (error) {
            *error = "failed to verify typed full-frame-rate playback intent";
        }
        return false;
    }
    return true;
}

bool Persist(const fs::path& video_path,
             int recording_fps,
             Status status,
             const Outcome& outcome,
             fs::path* sidecar_path,
             std::string* error) {
    try {
        if (video_path.empty()) {
            if (error) {
                *error = "empty_video_path";
            }
            return false;
        }
        const fs::path destination = SidecarPathFor(video_path);
        if (sidecar_path) {
            *sidecar_path = destination;
        }

        std::error_code file_size_error;
        const std::uintmax_t file_size =
            fs::file_size(video_path, file_size_error);
        const bool container_finalized =
            outcome.header_written && outcome.trailer_written &&
            outcome.output_closed;
        nlohmann::json document = {
            {"schema_id", kSchemaId},
            {"schema_version", kSchemaVersion},
            {"generated_at_utc", CurrentUtcTimestamp()},
            {"status", StatusName(status)},
            {"terminal", IsTerminal(status)},
            {"video_path", video_path.string()},
            {"sidecar_path", destination.string()},
            {"recording_fps", recording_fps},
            {"container",
             {
                 {"header_written", outcome.header_written},
                 {"trailer_attempted", outcome.trailer_attempted},
                 {"trailer_written", outcome.trailer_written},
                 {"output_close_attempted", outcome.output_close_attempted},
                 {"output_closed", outcome.output_closed},
                 {"finalized", container_finalized},
                 {"trailer_error_code",
                  NullableErrorCode(outcome.trailer_error_code)},
                 {"trailer_error", NullableError(outcome.trailer_error)},
                 {"output_close_error_code",
                  NullableErrorCode(outcome.output_close_error_code)},
                 {"output_close_error",
                  NullableError(outcome.output_close_error)},
                 {"file_size_bytes",
                  file_size_error ? nlohmann::json(nullptr)
                                  : nlohmann::json(file_size)},
                 {"file_size_error",
                  file_size_error
                      ? nlohmann::json(file_size_error.message())
                      : nlohmann::json(nullptr)},
             }},
            {"quicktime_full_frame_rate_playback_intent",
             {
                 {"key", kFullFrameRatePlaybackIntentKey},
                 {"requested_value", 1},
                 {"required_data_type", "UInt8"},
                 {"quicktime_data_atom_type", 22},
                 {"patch_attempted",
                  outcome.playback_intent_patch_attempted},
                 {"patch_applied", outcome.playback_intent_patch_applied},
                 {"error",
                  NullableError(outcome.playback_intent_patch_error)},
             }},
        };
        return WriteJsonAtomically(destination, document, error);
    } catch (const std::exception& exception) {
        if (error) {
            *error = std::string("sidecar_exception:") + exception.what();
        }
        return false;
    } catch (...) {
        if (error) {
            *error = "sidecar_exception:unknown";
        }
        return false;
    }
}

}  // namespace OrangeVideoContainerFinalization
