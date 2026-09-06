#include "spatial_roi_session_authority_store.h"

#include "gui/spatial_layout/sha256.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <limits>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxRootPathBytes = 4096;
constexpr std::size_t kMaxRootComponents = 128;
constexpr std::size_t kMaxAuthorityBytes = 64U * 1024U * 1024U;

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

std::atomic<std::uint64_t> g_staging_sequence{0};

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out != nullptr) {
        *error_out = message;
    }
    return false;
}

void clear_error(std::string* error_out)
{
    if (error_out != nullptr) {
        error_out->clear();
    }
}

void close_fd(int* fd) noexcept
{
    if (fd != nullptr && *fd >= 0) {
        const int saved_errno = errno;
        (void)::close(*fd);
        *fd = -1;
        errno = saved_errno;
    }
}

std::string errno_message(const std::string& operation)
{
    return operation + ": " + std::strerror(errno) +
        " (errno=" + std::to_string(errno) + ")";
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    for (std::size_t index = 7; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool identity_from_stat(
    const struct stat& value,
    SpatialRoiSessionAuthorityIdentity* identity_out,
    std::string* error_out)
{
    if (identity_out == nullptr) {
        return fail(error_out, "authority identity output is null");
    }
    if (value.st_ino == 0) {
        return fail(error_out, "authority filesystem identity has zero inode");
    }
    identity_out->device = static_cast<std::uint64_t>(value.st_dev);
    identity_out->inode = static_cast<std::uint64_t>(value.st_ino);
    return true;
}

bool directory_identity_for_fd(
    const int fd,
    SpatialRoiSessionAuthorityIdentity* identity_out,
    std::string* error_out)
{
    struct stat value {};
    if (::fstat(fd, &value) != 0) {
        return fail(error_out, errno_message("fstat authority root"));
    }
    if (!S_ISDIR(value.st_mode)) {
        return fail(error_out, "authority root is not a directory");
    }
    return identity_from_stat(value, identity_out, error_out);
}

bool safe_flat_leaf(
    const std::string& value,
    std::string* error_out)
{
    if (value.empty()) {
        return fail(error_out, "authority filename is empty");
    }
    if (value.size() > NAME_MAX) {
        return fail(error_out, "authority filename exceeds NAME_MAX");
    }
    if (value == "." || value == ".." || value.front() == '/' ||
        value.front() == '\\' || value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos ||
        value.find('\0') != std::string::npos) {
        return fail(error_out, "authority filename is not a safe flat relative leaf");
    }
    const fs::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        path.filename() != path || path.lexically_normal().generic_string() != value) {
        return fail(error_out, "authority filename is not a normalized flat leaf");
    }
    return true;
}

bool safe_absolute_root(
    const fs::path& root,
    std::vector<std::string>* components_out,
    std::string* error_out)
{
    if (components_out == nullptr) {
        return fail(error_out, "authority root component output is null");
    }
    components_out->clear();
    const std::string text = root.generic_string();
    if (text.empty() || !root.is_absolute() || text == "/") {
        return fail(error_out, "authority recording root must be a non-root absolute path");
    }
    if (text.size() > kMaxRootPathBytes ||
        text.find('\0') != std::string::npos ||
        root.lexically_normal().generic_string() != text) {
        return fail(error_out, "authority recording root path is not bounded and normalized");
    }
    auto iterator = root.begin();
    if (iterator == root.end() || iterator->generic_string() != "/") {
        return fail(error_out, "authority recording root has no absolute root component");
    }
    ++iterator;
    for (; iterator != root.end(); ++iterator) {
        const std::string component = iterator->generic_string();
        if (component.empty() || component == "." || component == ".." ||
            component.find('/') != std::string::npos ||
            component.find('\\') != std::string::npos ||
            component.find('\0') != std::string::npos ||
            component.size() > NAME_MAX) {
            return fail(error_out, "authority recording root has an unsafe component");
        }
        components_out->push_back(component);
        if (components_out->size() > kMaxRootComponents) {
            return fail(error_out, "authority recording root has too many components");
        }
    }
    if (components_out->empty()) {
        return fail(error_out, "authority recording root must not be '/'");
    }
    return true;
}

bool validate_allow_list(
    const std::vector<std::string>& paths,
    std::set<std::string>* allowed_out,
    std::string* error_out)
{
    if (allowed_out == nullptr) {
        return fail(error_out, "authority allow-list output is null");
    }
    allowed_out->clear();
    if (paths.empty()) {
        return fail(error_out, "authority allow-list must not be empty");
    }
    for (const std::string& path : paths) {
        if (!safe_flat_leaf(path, error_out) ||
            !allowed_out->insert(path).second) {
            if (error_out != nullptr && error_out->empty()) {
                *error_out = "authority allow-list contains a duplicate path";
            }
            return false;
        }
    }
    return true;
}

bool fsync_fd(const int fd, const std::string& description, std::string* error_out)
{
    int result = -1;
    do {
        result = ::fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0 || fail(error_out, errno_message("fsync " + description));
}

bool open_absolute_directory(
    const fs::path& root,
    const bool create_missing,
    int* fd_out,
    std::string* error_out)
{
    if (fd_out == nullptr) {
        return fail(error_out, "authority root descriptor output is null");
    }
    *fd_out = -1;
    std::vector<std::string> components;
    if (!safe_absolute_root(root, &components, error_out)) {
        return false;
    }

    int current = -1;
    do {
        current = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (current < 0 && errno == EINTR);
    if (current < 0) {
        return fail(error_out, errno_message("open authority filesystem root"));
    }
    for (const std::string& component : components) {
        int next = -1;
        do {
            next = ::openat(current,
                            component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (next < 0 && errno == EINTR);
        if (next < 0 && create_missing && errno == ENOENT) {
            if (::mkdirat(current, component.c_str(), 0700) != 0 &&
                errno != EEXIST) {
                const std::string message = errno_message(
                    "mkdirat authority root component '" + component + "'");
                close_fd(&current);
                return fail(error_out, message);
            }
            if (!fsync_fd(current,
                          "authority root parent after mkdirat",
                          error_out)) {
                close_fd(&current);
                return false;
            }
            do {
                next = ::openat(current,
                                component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            } while (next < 0 && errno == EINTR);
        }
        if (next < 0) {
            const std::string message = errno_message(
                "openat authority root component '" + component + "'");
            close_fd(&current);
            return fail(error_out, message);
        }
        close_fd(&current);
        current = next;
    }
    SpatialRoiSessionAuthorityIdentity unused;
    if (!directory_identity_for_fd(current, &unused, error_out)) {
        close_fd(&current);
        return false;
    }
    *fd_out = current;
    return true;
}

bool stat_leaf(
    const int root_fd,
    const std::string& leaf,
    struct stat* stat_out,
    std::string* error_out)
{
    if (stat_out == nullptr) {
        return fail(error_out, "authority leaf stat output is null");
    }
    if (::fstatat(root_fd, leaf.c_str(), stat_out, AT_SYMLINK_NOFOLLOW) != 0) {
        return fail(error_out, errno_message("fstatat authority leaf '" + leaf + "'"));
    }
    if (!S_ISREG(stat_out->st_mode)) {
        return fail(error_out, "authority leaf is not a regular file: " + leaf);
    }
    if (stat_out->st_nlink != 1) {
        return fail(error_out, "authority leaf has a hard-link alias: " + leaf);
    }
    return true;
}

bool verify_leaf_fd_binding(
    const int root_fd,
    const std::string& leaf,
    const int file_fd,
    std::string* error_out)
{
    struct stat current {};
    if (!stat_leaf(root_fd, leaf, &current, error_out)) {
        return false;
    }
    struct stat opened {};
    if (::fstat(file_fd, &opened) != 0) {
        return fail(error_out, errno_message("fstat opened authority leaf"));
    }
    return (current.st_dev == opened.st_dev && current.st_ino == opened.st_ino) ||
        fail(error_out, "authority leaf binding changed: " + leaf);
}

bool read_fd_exact(
    const int fd,
    std::string* bytes_out,
    std::string* error_out)
{
    if (bytes_out == nullptr) {
        return fail(error_out, "authority read output is null");
    }
    struct stat before {};
    if (::fstat(fd, &before) != 0) {
        return fail(error_out, errno_message("fstat authority file"));
    }
    if (!S_ISREG(before.st_mode) || before.st_nlink != 1) {
        return fail(error_out, "authority file is not an unaliased regular file");
    }
    if (before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > kMaxAuthorityBytes) {
        return fail(error_out, "authority file exceeds the byte bound");
    }
    bytes_out->assign(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes_out->size()) {
        const ssize_t read_count = ::pread(
            fd, bytes_out->data() + offset, bytes_out->size() - offset,
            static_cast<off_t>(offset));
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, errno_message("pread authority file"));
        }
        if (read_count == 0) {
            return fail(error_out, "authority file changed while reading");
        }
        offset += static_cast<std::size_t>(read_count);
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0) {
        return fail(error_out, errno_message("fstat authority file after read"));
    }
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_nlink != after.st_nlink) {
        return fail(error_out, "authority file changed while reading");
    }
    return true;
}

std::string bytes_sha256(const std::string& bytes)
{
    return "sha256:" +
        orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

bool write_all(
    const int fd,
    const std::string& bytes,
    std::string* error_out)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, errno_message("write authority staging file"));
        }
        if (written == 0) {
            return fail(error_out, "authority staging file write made no progress");
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool open_existing_leaf(
    const int root_fd,
    const std::string& leaf,
    int* fd_out,
    std::string* error_out)
{
    if (fd_out == nullptr) {
        return fail(error_out, "authority leaf descriptor output is null");
    }
    *fd_out = -1;
    do {
        *fd_out = ::openat(root_fd,
                           leaf.c_str(),
                           O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    } while (*fd_out < 0 && errno == EINTR);
    if (*fd_out >= 0 || errno == ENOENT) {
        return *fd_out >= 0;
    }
    return fail(error_out, errno_message("open authority leaf '" + leaf + "'"));
}

bool validate_existing_authorities(
    const int root_fd,
    const std::set<std::string>& allowed,
    std::string* error_out)
{
    std::set<std::pair<std::uint64_t, std::uint64_t>> identities;
    for (const std::string& leaf : allowed) {
        int fd = -1;
        if (!open_existing_leaf(root_fd, leaf, &fd, error_out)) {
            const int open_errno = errno;
            if (open_errno == ENOENT) {
                continue;
            }
            return false;
        }
        bool ok = verify_leaf_fd_binding(root_fd, leaf, fd, error_out);
        struct stat value {};
        if (ok && ::fstat(fd, &value) != 0) {
            ok = fail(error_out, errno_message("fstat existing authority leaf"));
        }
        if (ok) {
            SpatialRoiSessionAuthorityIdentity identity;
            ok = identity_from_stat(value, &identity, error_out) &&
                identities.insert({identity.device, identity.inode}).second;
            if (!ok && error_out != nullptr && error_out->empty()) {
                *error_out = "authority allow-list contains duplicate existing inode";
            }
        }
        close_fd(&fd);
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool atomic_publish(
    const int root_fd,
    const std::string& temporary,
    const std::string& leaf,
    bool* collision_out,
    std::string* error_out)
{
    if (collision_out != nullptr) {
        *collision_out = false;
    }
#if defined(__linux__) && defined(SYS_renameat2)
    const long renamed = ::syscall(
        SYS_renameat2, root_fd, temporary.c_str(), root_fd, leaf.c_str(),
        static_cast<unsigned int>(RENAME_NOREPLACE));
    if (renamed == 0) {
        return fsync_fd(root_fd, "authority root after publication", error_out);
    }
    if (errno == EEXIST) {
        if (collision_out != nullptr) {
            *collision_out = true;
        }
        return false;
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
        return fail(error_out, errno_message("renameat2 authority publication"));
    }
#endif
    if (::linkat(root_fd, temporary.c_str(), root_fd, leaf.c_str(), 0) != 0) {
        if (errno == EEXIST) {
            if (collision_out != nullptr) {
                *collision_out = true;
            }
        }
        return fail(error_out, errno_message("linkat authority publication"));
    }
    if (::unlinkat(root_fd, temporary.c_str(), 0) != 0) {
        return fail(error_out, errno_message("unlink authority staging leaf"));
    }
    return fsync_fd(root_fd, "authority root after publication", error_out);
}

}  // namespace

nlohmann::json SpatialRoiSessionAuthorityReceipt::ToJson() const
{
    return {{"relative_path", relative_path},
            {"size_bytes", size_bytes},
            {"sha256", sha256}};
}

SpatialRoiSessionAuthorityStore::SpatialRoiSessionAuthorityStore(
    const int recording_root_fd,
    fs::path recording_root_path,
    const SpatialRoiSessionAuthorityIdentity recording_root_identity,
    std::set<std::string> allowed_relative_paths)
    : recording_root_fd_(recording_root_fd),
      recording_root_path_(std::move(recording_root_path)),
      recording_root_identity_(recording_root_identity),
      allowed_relative_paths_(std::move(allowed_relative_paths))
{
}

SpatialRoiSessionAuthorityStore::~SpatialRoiSessionAuthorityStore()
{
    Reset();
}

SpatialRoiSessionAuthorityStore::SpatialRoiSessionAuthorityStore(
    SpatialRoiSessionAuthorityStore&& other) noexcept
{
    *this = std::move(other);
}

SpatialRoiSessionAuthorityStore& SpatialRoiSessionAuthorityStore::operator=(
    SpatialRoiSessionAuthorityStore&& other) noexcept
{
    if (this != &other) {
        Reset();
        recording_root_fd_ = std::exchange(other.recording_root_fd_, -1);
        recording_root_path_ = std::move(other.recording_root_path_);
        recording_root_identity_ = other.recording_root_identity_;
        allowed_relative_paths_ = std::move(other.allowed_relative_paths_);
        other.recording_root_identity_ = {};
    }
    return *this;
}

void SpatialRoiSessionAuthorityStore::Reset() noexcept
{
    close_fd(&recording_root_fd_);
    recording_root_path_.clear();
    recording_root_identity_ = {};
    allowed_relative_paths_.clear();
}

bool SpatialRoiSessionAuthorityStore::OpenExisting(
    const fs::path& recording_root,
    const std::vector<std::string>& allowed_relative_paths,
    std::unique_ptr<SpatialRoiSessionAuthorityStore>* store_out,
    std::string* error_out)
{
    return OpenImpl(recording_root, allowed_relative_paths, false, store_out,
                    error_out);
}

bool SpatialRoiSessionAuthorityStore::OpenOrCreate(
    const fs::path& recording_root,
    const std::vector<std::string>& allowed_relative_paths,
    std::unique_ptr<SpatialRoiSessionAuthorityStore>* store_out,
    std::string* error_out)
{
    return OpenImpl(recording_root, allowed_relative_paths, true, store_out,
                    error_out);
}

bool SpatialRoiSessionAuthorityStore::OpenImpl(
    const fs::path& recording_root,
    const std::vector<std::string>& allowed_relative_paths,
    const bool create_missing,
    std::unique_ptr<SpatialRoiSessionAuthorityStore>* store_out,
    std::string* error_out)
{
    clear_error(error_out);
    if (store_out == nullptr) {
        return fail(error_out, "authority store output is null");
    }
    store_out->reset();
    std::set<std::string> allowed;
    if (!validate_allow_list(allowed_relative_paths, &allowed, error_out)) {
        return false;
    }
    std::vector<std::string> ignored_components;
    if (!safe_absolute_root(recording_root, &ignored_components, error_out)) {
        return false;
    }
    int root_fd = -1;
    if (!open_absolute_directory(recording_root, create_missing, &root_fd,
                                 error_out)) {
        return false;
    }
    SpatialRoiSessionAuthorityIdentity identity;
    if (!directory_identity_for_fd(root_fd, &identity, error_out) ||
        !validate_existing_authorities(root_fd, allowed, error_out)) {
        close_fd(&root_fd);
        return false;
    }
    *store_out = std::unique_ptr<SpatialRoiSessionAuthorityStore>(
        new SpatialRoiSessionAuthorityStore(
            root_fd, recording_root.lexically_normal(), identity,
            std::move(allowed)));
    return true;
}

bool SpatialRoiSessionAuthorityStore::IsAllowed(
    const std::string& relative_path) const noexcept
{
    return allowed_relative_paths_.find(relative_path) !=
        allowed_relative_paths_.end();
}

bool SpatialRoiSessionAuthorityStore::VerifyRootBinding(
    std::string* error_out) const
{
    clear_error(error_out);
    if (!valid()) {
        return fail(error_out, "authority store root descriptor is closed");
    }
    SpatialRoiSessionAuthorityIdentity retained;
    if (!directory_identity_for_fd(recording_root_fd_, &retained, error_out)) {
        return false;
    }
    if (retained != recording_root_identity_) {
        return fail(error_out, "authority root descriptor identity changed");
    }
    int reopened_fd = -1;
    if (!open_absolute_directory(recording_root_path_, false, &reopened_fd,
                                 error_out)) {
        return false;
    }
    SpatialRoiSessionAuthorityIdentity reopened;
    const bool ok = directory_identity_for_fd(reopened_fd, &reopened, error_out);
    close_fd(&reopened_fd);
    if (!ok) {
        return false;
    }
    return reopened == recording_root_identity_ ||
        fail(error_out, "authority recording-root path binding changed");
}

bool SpatialRoiSessionAuthorityStore::ReadAndVerify(
    const SpatialRoiSessionAuthorityReceipt& expected,
    std::string* bytes_out,
    SpatialRoiSessionAuthorityReceipt* verified_out,
    std::string* error_out) const
{
    clear_error(error_out);
    if (!valid() || !IsAllowed(expected.relative_path) ||
        !safe_flat_leaf(expected.relative_path, error_out) ||
        expected.relative_path.empty() ||
        !canonical_sha256(expected.sha256)) {
        if (error_out != nullptr && error_out->empty()) {
            *error_out = "authority receipt or store is invalid";
        }
        return false;
    }
    if (!VerifyRootBinding(error_out)) {
        return false;
    }
    int fd = -1;
    if (!open_existing_leaf(recording_root_fd_, expected.relative_path, &fd,
                            error_out)) {
        return fail(error_out, errno_message("open authority leaf"));
    }
    bool ok = verify_leaf_fd_binding(recording_root_fd_, expected.relative_path,
                                     fd, error_out);
    std::string bytes;
    if (ok) {
        ok = read_fd_exact(fd, &bytes, error_out);
    }
    if (ok && !verify_leaf_fd_binding(recording_root_fd_, expected.relative_path,
                                      fd, error_out)) {
        ok = false;
    }
    close_fd(&fd);
    if (!ok) {
        return false;
    }
    const SpatialRoiSessionAuthorityReceipt actual = {
        expected.relative_path,
        static_cast<std::uint64_t>(bytes.size()),
        bytes_sha256(bytes)};
    if (actual.size_bytes != expected.size_bytes ||
        actual.sha256 != expected.sha256) {
        return fail(error_out, "authority receipt does not match exact file bytes");
    }
    if (bytes_out != nullptr) {
        *bytes_out = bytes;
    }
    if (verified_out != nullptr) {
        *verified_out = actual;
    }
    return VerifyRootBinding(error_out);
}

bool SpatialRoiSessionAuthorityStore::PublishBytes(
    const std::string& relative_path,
    const std::string& bytes,
    SpatialRoiSessionAuthorityReceipt* receipt_out,
    std::string* error_out) const
{
    clear_error(error_out);
    if (receipt_out == nullptr) {
        return fail(error_out, "authority receipt output is null");
    }
    *receipt_out = {};
    if (!valid() || !IsAllowed(relative_path) ||
        !safe_flat_leaf(relative_path, error_out)) {
        if (error_out != nullptr && error_out->empty()) {
            *error_out = "authority path is not allow-listed or safe";
        }
        return false;
    }
    if (bytes.size() > kMaxAuthorityBytes) {
        return fail(error_out, "authority bytes exceed the byte bound");
    }
    if (!VerifyRootBinding(error_out)) {
        return false;
    }

    int existing_fd = -1;
    if (open_existing_leaf(recording_root_fd_, relative_path, &existing_fd,
                           error_out)) {
        bool ok = verify_leaf_fd_binding(recording_root_fd_, relative_path,
                                         existing_fd, error_out);
        std::string existing_bytes;
        if (ok) {
            ok = read_fd_exact(existing_fd, &existing_bytes, error_out);
        }
        if (ok && !verify_leaf_fd_binding(recording_root_fd_, relative_path,
                                          existing_fd, error_out)) {
            ok = false;
        }
        close_fd(&existing_fd);
        if (!ok) {
            return false;
        }
        if (existing_bytes != bytes) {
            return fail(error_out,
                        "authority leaf already exists with different bytes");
        }
        *receipt_out = {relative_path,
                        static_cast<std::uint64_t>(existing_bytes.size()),
                        bytes_sha256(existing_bytes)};
        return VerifyRootBinding(error_out);
    }
    const int open_errno = errno;
    if (open_errno != ENOENT) {
        errno = open_errno;
        return fail(error_out, errno_message("open existing authority leaf"));
    }

    const std::string temporary =
        ".orange_authority_tmp." + std::to_string(static_cast<long long>(::getpid())) +
        "." + std::to_string(g_staging_sequence.fetch_add(1));
    int staging_fd = -1;
    do {
        staging_fd = ::openat(recording_root_fd_, temporary.c_str(),
                              O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                              0444);
    } while (staging_fd < 0 && errno == EINTR);
    if (staging_fd < 0) {
        return fail(error_out, errno_message("create authority staging leaf"));
    }
    bool ok = write_all(staging_fd, bytes, error_out) &&
        fsync_fd(staging_fd, "authority staging file", error_out);
    std::string written_bytes;
    if (ok) {
        ok = read_fd_exact(staging_fd, &written_bytes, error_out);
    }
    if (ok && written_bytes != bytes) {
        ok = fail(error_out, "authority staging readback differed from input bytes");
    }
    if (ok) {
        ok = VerifyRootBinding(error_out);
    }
    close_fd(&staging_fd);
    if (!ok) {
        (void)::unlinkat(recording_root_fd_, temporary.c_str(), 0);
        return false;
    }
    bool collision = false;
    if (!atomic_publish(recording_root_fd_, temporary, relative_path, &collision,
                        error_out)) {
        (void)::unlinkat(recording_root_fd_, temporary.c_str(), 0);
        if (collision) {
            SpatialRoiSessionAuthorityReceipt expected = {
                relative_path, static_cast<std::uint64_t>(bytes.size()),
                bytes_sha256(bytes)};
            return ReadAndVerify(expected, nullptr, receipt_out, error_out);
        }
        return false;
    }
    SpatialRoiSessionAuthorityReceipt expected = {
        relative_path, static_cast<std::uint64_t>(written_bytes.size()),
        bytes_sha256(written_bytes)};
    std::string verified_bytes;
    if (!ReadAndVerify(expected, &verified_bytes, receipt_out, error_out)) {
        return false;
    }
    return VerifyRootBinding(error_out);
}

bool SpatialRoiSessionAuthorityStore::PublishJson(
    const std::string& relative_path,
    const json& value,
    SpatialRoiSessionAuthorityReceipt* receipt_out,
    std::string* error_out) const
{
    clear_error(error_out);
    std::string bytes;
    try {
        bytes = value.dump(-1, ' ', false, json::error_handler_t::strict);
    } catch (const std::exception& exception) {
        return fail(error_out,
                    "authority JSON canonicalization failed: " +
                        std::string(exception.what()));
    }
    return PublishBytes(relative_path, bytes, receipt_out, error_out);
}

}  // namespace orange::session::spatial_roi
