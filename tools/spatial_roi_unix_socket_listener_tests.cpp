#include "spatial_roi_unix_socket_listener.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

using orange::spatial_roi::ipc::SpatialRoiIpcTransportReadStatus;
using orange::spatial_roi::ipc::SpatialRoiUnixSocketLineTransport;
using orange::spatial_roi::ipc::SpatialRoiUnixSocketListener;
using orange::spatial_roi::ipc::SpatialRoiUnixSocketListenerAcceptStatus;
using orange::spatial_roi::ipc::SpatialRoiUnixSocketTransportConfig;

SpatialRoiUnixSocketTransportConfig trusted_test_transport_config()
{
    SpatialRoiUnixSocketTransportConfig config;
    // The listener requires an explicit post-connect identity even in tests.
    // This models the supervisor's expected child identity without weakening
    // the production API with an unverified mode.
    config.expected_peer_pid = ::getpid();
    config.expected_peer_uid = ::geteuid();
    return config;
}

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempDirectory final {
public:
    TempDirectory()
    {
        std::array<char, 64> pattern{};
        const std::string prefix = "/tmp/orange_spatial_roi_listener_";
        require(prefix.size() + 7 < pattern.size(), "temp prefix is too long");
        std::memcpy(pattern.data(), prefix.data(), prefix.size());
        std::memcpy(pattern.data() + prefix.size(), "XXXXXX", 7);
        require(::mkdtemp(pattern.data()) != nullptr,
                std::string("mkdtemp failed: ") + std::strerror(errno));
        path_ = pattern.data();
    }

    ~TempDirectory()
    {
        if (!path_.empty()) {
            (void)::rmdir(path_.c_str());
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    std::string socket_path(const char* leaf = "gate2.sock") const
    {
        return path_ + "/" + leaf;
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

void remove_if_present(const std::string& path)
{
    if (::unlink(path.c_str()) != 0) {
        require(errno == ENOENT,
                "failed to remove test entry " + path + ": " +
                    std::strerror(errno));
    }
}

int connect_client(const std::string& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0,
            std::string("client socket failed: ") + std::strerror(errno));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(path.size() < sizeof(address.sun_path), "client path too long");
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    const socklen_t address_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::connect(fd,
                  reinterpret_cast<const sockaddr*>(&address),
                  address_length) != 0) {
        const std::string error = std::strerror(errno);
        (void)::close(fd);
        throw std::runtime_error("client connect failed: " + error);
    }
    return fd;
}

void close_fd(int* fd)
{
    if (fd && *fd >= 0) {
        (void)::close(*fd);
        *fd = -1;
    }
}

void require_absent(const std::string& path, const std::string& context)
{
    struct stat stat_value {
    };
    errno = 0;
    require(::lstat(path.c_str(), &stat_value) != 0 && errno == ENOENT,
            context + ": path unexpectedly exists");
}

void test_bind_flags_mode_and_round_trip()
{
    TempDirectory directory;
    const std::string path = directory.socket_path();
    SpatialRoiUnixSocketTransportConfig transport_config;
    transport_config.expected_peer_pid = ::getpid();
    transport_config.expected_peer_uid = ::geteuid();
    transport_config.max_wire_message_bytes = 128;
    transport_config.max_receive_buffer_bytes = 128;

    std::string error;
    auto listener = SpatialRoiUnixSocketListener::Create(
        path, std::move(transport_config), &error);
    require(listener != nullptr, "listener creation failed: " + error);
    require(listener->socket_path() == path, "listener changed exact socket path");
    require(listener->native_handle() >= 0, "listener has no native descriptor");
    require(listener->parent_directory_handle() >= 0,
            "listener did not retain its private parent descriptor");

    const int parent_descriptor_flags =
        ::fcntl(listener->parent_directory_handle(), F_GETFD);
    require(parent_descriptor_flags >= 0 &&
                (parent_descriptor_flags & FD_CLOEXEC) != 0,
            "parent directory descriptor is not FD_CLOEXEC");
    struct stat parent_stat {
    };
    require(::fstat(listener->parent_directory_handle(), &parent_stat) == 0 &&
                S_ISDIR(parent_stat.st_mode) &&
                parent_stat.st_uid == ::geteuid() &&
                (parent_stat.st_mode & 0777) == 0700,
            "listener retained an unauthorized parent directory");

    const int descriptor_flags = ::fcntl(listener->native_handle(), F_GETFD);
    require(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0,
            "listener descriptor is not FD_CLOEXEC");
    const int status_flags = ::fcntl(listener->native_handle(), F_GETFL);
    require(status_flags >= 0 && (status_flags & O_NONBLOCK) != 0,
            "listener descriptor is not O_NONBLOCK");

    struct stat socket_stat {
    };
    require(::lstat(path.c_str(), &socket_stat) == 0 &&
                S_ISSOCK(socket_stat.st_mode),
            "listener did not create a filesystem socket");
    require((socket_stat.st_mode & 0777) == 0600,
            "listener socket mode is not exactly 0600");

    int client_fd = connect_client(path);
    auto transport = listener->AcceptOne(std::chrono::milliseconds(500), &error);
    require(transport != nullptr, "accept failed: " + error);
    require(listener->closed(), "listener remained open after first accept");
    require(listener->accepted(), "listener did not record one-shot acceptance");
    require(transport->config().max_wire_message_bytes == 128,
            "listener did not propagate transport configuration");
    require_absent(path, "accepted listener cleanup");

    require(::send(client_fd, "hello\n", 6, MSG_NOSIGNAL) == 6,
            "client request send failed");
    const auto received =
        transport->ReadLine(std::chrono::milliseconds(500), 128);
    require(received.status == SpatialRoiIpcTransportReadStatus::kLine &&
                received.line == "hello\n",
            "accepted transport did not read the client line");
    require(transport->WriteLine("world\n", &error),
            "accepted transport write failed: " + error);
    std::array<char, 16> response{};
    require(::recv(client_fd, response.data(), response.size(), 0) == 6 &&
                std::string(response.data(), 6) == "world\n",
            "client did not receive server response");
    close_fd(&client_fd);

    // A second close, including the destructor's later close, must be inert.
    listener->Close();
    listener->Close();
}

void test_preexisting_entries_are_rejected()
{
    TempDirectory directory;

    {
        const std::string path = directory.socket_path("regular");
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(fd >= 0, "failed to create pre-existing regular file");
        require(::write(fd, "keep", 4) == 4, "failed to seed regular file");
        close_fd(&fd);
        struct stat before {
        };
        require(::lstat(path.c_str(), &before) == 0, "regular file lstat failed");
        std::string error;
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, trusted_test_transport_config(), &error);
        require(listener == nullptr, "pre-existing regular file was replaced");
        struct stat after {
        };
        require(::lstat(path.c_str(), &after) == 0 && after.st_ino == before.st_ino,
                "pre-existing regular file identity changed");
        remove_if_present(path);
    }

    {
        const std::string path = directory.socket_path("symlink");
        const std::string target = directory.socket_path("symlink-target");
        int target_fd =
            ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(target_fd >= 0, "failed to create symlink target");
        close_fd(&target_fd);
        require(::symlink(target.c_str(), path.c_str()) == 0,
                "failed to create pre-existing symlink");
        std::string error;
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, trusted_test_transport_config(), &error);
        require(listener == nullptr, "pre-existing symlink was replaced");
        struct stat link_stat {
        };
        require(::lstat(path.c_str(), &link_stat) == 0 && S_ISLNK(link_stat.st_mode),
                "pre-existing symlink identity changed");
        remove_if_present(path);
        remove_if_present(target);
    }

    {
        const std::string path = directory.socket_path("socket");
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        require(fd >= 0, "failed to create pre-existing socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.data(), path.size());
        const socklen_t address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path.size() + 1);
        require(::bind(fd,
                       reinterpret_cast<const sockaddr*>(&address),
                       address_length) == 0,
                "failed to bind pre-existing socket");
        require(::listen(fd, 1) == 0, "failed to listen on pre-existing socket");
        std::string error;
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, trusted_test_transport_config(), &error);
        require(listener == nullptr, "pre-existing socket was replaced");
        require(::fcntl(fd, F_GETFD) >= 0,
                "pre-existing socket descriptor was closed unexpectedly");
        (void)::close(fd);
        remove_if_present(path);
    }
}

void test_path_validation_and_timeout()
{
    std::string error;
    for (const std::string& path : {std::string(),
                                    std::string("relative.sock"),
                                    std::string("/"),
                                    std::string("//"),
                                    std::string("/tmp/./gate.sock"),
                                    std::string("/tmp/../gate.sock")}) {
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, trusted_test_transport_config(), &error);
        require(listener == nullptr, "unsafe socket path was accepted: " + path);
    }
    std::string embedded_nul("/tmp/gate.sock\0suffix", 21);
    auto nul_listener =
        SpatialRoiUnixSocketListener::Create(
            embedded_nul, trusted_test_transport_config(), &error);
    require(nul_listener == nullptr, "socket path containing NUL was accepted");
    std::string too_long = "/tmp/" + std::string(104, 'x');
    auto too_long_listener =
        SpatialRoiUnixSocketListener::Create(
            too_long, trusted_test_transport_config(), &error);
    require(too_long_listener == nullptr, "overlong socket path was accepted");

    TempDirectory directory;
    const std::string path = directory.socket_path("timeout");
    auto listener = SpatialRoiUnixSocketListener::Create(
        path, trusted_test_transport_config(), &error);
    require(listener != nullptr, "timeout listener creation failed: " + error);
    const auto start = std::chrono::steady_clock::now();
    const auto result = listener->AcceptOneResult(std::chrono::milliseconds(60));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(result.status == SpatialRoiUnixSocketListenerAcceptStatus::kTimeout,
            "accept without a client did not time out");
    require(elapsed >= std::chrono::milliseconds(40),
            "accept timeout returned substantially early");
    require(!listener->closed(), "timeout consumed the listener");
    require(::access(path.c_str(), F_OK) == 0,
            "timeout removed the live listener socket");
}

void test_exactly_once_and_path_replacement()
{
    TempDirectory directory;
    std::string error;
    const std::string path = directory.socket_path("once");
    auto listener = SpatialRoiUnixSocketListener::Create(
        path, trusted_test_transport_config(), &error);
    require(listener != nullptr, "one-shot listener creation failed: " + error);
    int client_fd = connect_client(path);
    auto transport = listener->AcceptOne(std::chrono::milliseconds(500), &error);
    require(transport != nullptr, "one-shot accept failed: " + error);
    close_fd(&client_fd);
    auto second = listener->AcceptOne(std::chrono::milliseconds(50), &error);
    require(second == nullptr && !error.empty(),
            "listener accepted or waited for a second connection");
    require(listener->closed(), "listener reopened after one-shot accept");
    const int reconnect_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(reconnect_fd >= 0, "failed to create reconnect probe socket");
    sockaddr_un reconnect_address{};
    reconnect_address.sun_family = AF_UNIX;
    std::memcpy(reconnect_address.sun_path, path.data(), path.size());
    const socklen_t reconnect_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    errno = 0;
    require(::connect(reconnect_fd,
                      reinterpret_cast<const sockaddr*>(&reconnect_address),
                      reconnect_length) != 0 && errno == ENOENT,
            "one-shot listener unexpectedly permitted reconnect");
    int reconnect_fd_mutable = reconnect_fd;
    close_fd(&reconnect_fd_mutable);
}

void test_credentials_and_adoption_fail_closed()
{
    TempDirectory directory;
    std::string error;

    {
        const std::string path = directory.socket_path("wrong_pid");
        auto config = trusted_test_transport_config();
        config.expected_peer_pid = static_cast<pid_t>(::getpid() + 1);
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, std::move(config), &error);
        require(listener != nullptr,
                "listener with wrong expected pid failed to bind: " + error);
        int client_fd = connect_client(path);
        const auto result =
            listener->AcceptOneResult(std::chrono::milliseconds(500));
        require(result.status == SpatialRoiUnixSocketListenerAcceptStatus::kError,
                "wrong peer pid was accepted by the listener");
        require(listener->closed() && listener->accepted(),
                "credential mismatch did not consume listener fail-closed");
        require_absent(path, "wrong-pid listener cleanup");
        close_fd(&client_fd);
    }

    {
        const std::string path = directory.socket_path("wrong_uid");
        auto config = trusted_test_transport_config();
        config.expected_peer_uid =
            ::geteuid() == static_cast<uid_t>(0) ? static_cast<uid_t>(1)
                                                 : static_cast<uid_t>(0);
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, std::move(config), &error);
        require(listener != nullptr,
                "listener with wrong expected uid failed to bind: " + error);
        int client_fd = connect_client(path);
        const auto result =
            listener->AcceptOneResult(std::chrono::milliseconds(500));
        require(result.status == SpatialRoiUnixSocketListenerAcceptStatus::kError,
                "wrong peer uid was accepted by the listener");
        require(listener->closed() && listener->accepted(),
                "uid mismatch did not consume listener fail-closed");
        require_absent(path, "wrong-uid listener cleanup");
        close_fd(&client_fd);
    }

    {
        const std::string path = directory.socket_path("bad_adoption");
        auto config = trusted_test_transport_config();
        config.max_wire_message_bytes = 0;
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, std::move(config), &error);
        require(listener != nullptr,
                "listener with invalid transport config failed to bind: " + error);
        int client_fd = connect_client(path);
        const auto result =
            listener->AcceptOneResult(std::chrono::milliseconds(500));
        require(result.status == SpatialRoiUnixSocketListenerAcceptStatus::kError,
                "invalid transport configuration was adopted");
        require(listener->closed() && listener->accepted(),
                "adoption failure did not consume listener fail-closed");
        require_absent(path, "adoption-failure listener cleanup");
        close_fd(&client_fd);
    }
}

void test_invalid_deadlines_fail_closed()
{
    TempDirectory directory;
    std::string error;

    {
        const std::string path = directory.socket_path("zero_timeout");
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, trusted_test_transport_config(), &error);
        require(listener != nullptr, "zero-timeout listener creation failed: " + error);
        const auto result =
            listener->AcceptOneResult(std::chrono::milliseconds(0));
        require(result.status == SpatialRoiUnixSocketListenerAcceptStatus::kError,
                "zero timeout was not rejected");
        require(listener->closed(), "invalid timeout left listener open");
        require_absent(path, "zero-timeout listener cleanup");
    }

    {
        const std::string path = directory.socket_path("expired_deadline");
        auto listener = SpatialRoiUnixSocketListener::Create(
            path, trusted_test_transport_config(), &error);
        require(listener != nullptr,
                "expired-deadline listener creation failed: " + error);
        const auto result = listener->AcceptOneResult(
            std::chrono::steady_clock::now());
        require(result.status == SpatialRoiUnixSocketListenerAcceptStatus::kError,
                "expired deadline was not rejected");
        require(listener->closed(), "invalid deadline left listener open");
        require_absent(path, "expired-deadline listener cleanup");
    }
}

void test_private_parent_authority()
{
    TempDirectory directory;
    std::string error;
    const std::string parent = directory.socket_path("unused").substr(
        0, directory.socket_path("unused").rfind('/'));

    require(::chmod(parent.c_str(), 0755) == 0,
            "failed to make parent directory non-private");
    auto non_private = SpatialRoiUnixSocketListener::Create(
        directory.socket_path("non_private"),
        trusted_test_transport_config(),
        &error);
    require(non_private == nullptr,
            "listener accepted a parent directory that was not mode 0700");
    require(::chmod(parent.c_str(), 0700) == 0,
            "failed to restore private parent mode");

    const std::string symlink_path = directory.socket_path("link");
    require(::symlink("/tmp", symlink_path.c_str()) == 0,
            "failed to create intermediate symlink");
    auto symlink_parent = SpatialRoiUnixSocketListener::Create(
        symlink_path + "/socket",
        trusted_test_transport_config(),
        &error);
    require(symlink_parent == nullptr,
            "listener followed a symlinked parent component");
    remove_if_present(symlink_path);
}

void test_retained_parent_fd_overload()
{
    TempDirectory directory;
    const std::string path = directory.socket_path("fd_overload");
    std::string error;

    int parent_fd = ::open(directory.path().c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(parent_fd >= 0,
            std::string("failed to open retained parent directory: ") +
                std::strerror(errno));
    struct stat parent_stat {
    };
    require(::fstat(parent_fd, &parent_stat) == 0 && S_ISDIR(parent_stat.st_mode),
            "retained parent descriptor is not a directory");
    const dev_t expected_parent_device = parent_stat.st_dev;
    const ino_t expected_parent_inode = parent_stat.st_ino;

    // The listener duplicates this descriptor.  Closing the caller's copy
    // immediately after Create proves that all subsequent authority comes
    // from the retained duplicate rather than an absolute-path re-walk.
    auto listener = SpatialRoiUnixSocketListener::Create(
        parent_fd,
        expected_parent_device,
        expected_parent_inode,
        path,
        "fd_overload",
        trusted_test_transport_config(),
        &error);
    close_fd(&parent_fd);
    require(listener != nullptr,
            "parent-fd listener creation failed: " + error);
    require(listener->parent_directory_handle() >= 0,
            "parent-fd listener did not retain its directory descriptor");
    require(::fstat(listener->parent_directory_handle(), &parent_stat) == 0 &&
                parent_stat.st_dev == expected_parent_device &&
                parent_stat.st_ino == expected_parent_inode,
            "retained parent descriptor cannot be inspected");

    int client_fd = connect_client(path);
    auto transport = listener->AcceptOne(std::chrono::milliseconds(500), &error);
    require(transport != nullptr,
            "parent-fd listener accept failed: " + error);
    close_fd(&client_fd);
    require_absent(path, "parent-fd listener cleanup");

    // A mismatched identity must fail before creating anything under the
    // caller-supplied descriptor.
    const std::string mismatch_path = directory.socket_path("fd_mismatch");
    parent_fd = ::open(directory.path().c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(parent_fd >= 0, "failed to reopen parent for identity mismatch");
    require(::fstat(parent_fd, &parent_stat) == 0,
            "failed to stat parent for identity mismatch");
    auto mismatch = SpatialRoiUnixSocketListener::Create(
        parent_fd,
        parent_stat.st_dev,
        parent_stat.st_ino + 1,
        mismatch_path,
        "fd_mismatch",
        trusted_test_transport_config(),
        &error);
    close_fd(&parent_fd);
    require(mismatch == nullptr,
            "parent-fd listener accepted a mismatched directory identity");
    require_absent(mismatch_path, "mismatched parent identity cleanup");
}

void test_replacement_is_preserved()
{
    TempDirectory directory;
    std::string error;
    const std::string path = directory.socket_path("replacement");
    auto listener = SpatialRoiUnixSocketListener::Create(
        path, trusted_test_transport_config(), &error);
    require(listener != nullptr, "replacement listener creation failed: " + error);
    struct stat original {
    };
    require(::lstat(path.c_str(), &original) == 0, "original socket lstat failed");
    require(::unlink(path.c_str()) == 0, "failed to unlink original socket");
    int replacement_fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(replacement_fd >= 0, "failed to create replacement file");
    close_fd(&replacement_fd);

    const auto result = listener->AcceptOneResult(std::chrono::milliseconds(100));
    require(result.status == SpatialRoiUnixSocketListenerAcceptStatus::kError,
            "path replacement did not fail closed before accept");
    require(listener->closed(), "replacement failure left listener open");
    struct stat replacement {
    };
    require(::lstat(path.c_str(), &replacement) == 0 &&
                replacement.st_ino != original.st_ino &&
                S_ISREG(replacement.st_mode),
            "listener removed or changed the replacement path");
    listener->Close();
    require(::lstat(path.c_str(), &replacement) == 0,
            "idempotent close removed replacement path");
    remove_if_present(path);
}

}  // namespace

int main()
{
    try {
        test_bind_flags_mode_and_round_trip();
        test_preexisting_entries_are_rejected();
        test_path_validation_and_timeout();
        test_exactly_once_and_path_replacement();
        test_credentials_and_adoption_fail_closed();
        test_invalid_deadlines_fail_closed();
        test_private_parent_authority();
        test_retained_parent_fd_overload();
        test_replacement_is_preserved();
        std::cout << "spatial_roi_unix_socket_listener_tests: all tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_unix_socket_listener_tests: FAIL: "
                  << exception.what() << "\n";
        return 1;
    }
}
