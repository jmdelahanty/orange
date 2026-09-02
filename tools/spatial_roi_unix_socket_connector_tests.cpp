#include "spatial_roi_unix_socket_connector.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

namespace ipc = orange::spatial_roi::ipc;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ServerFixture final {
public:
    ServerFixture()
    {
        std::array<char, 64> pattern{};
        const std::string prefix = "/tmp/orange_spatial_roi_connector_";
        std::memcpy(pattern.data(), prefix.data(), prefix.size());
        std::memcpy(pattern.data() + prefix.size(), "XXXXXX", 7);
        require(::mkdtemp(pattern.data()) != nullptr,
                std::string("mkdtemp failed: ") + std::strerror(errno));
        directory_ = pattern.data();
        path_ = directory_ + "/endpoint.sock";

        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd_ >= 0, "server socket creation failed");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.data(), path_.size());
        address.sun_path[path_.size()] = '\0';
        const socklen_t length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path_.size() + 1);
        require(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), length) == 0,
                std::string("server bind failed: ") + std::strerror(errno));
        require(::chmod(path_.c_str(), 0600) == 0,
                "could not set exact endpoint mode");
        require(::listen(fd_, 1) == 0, "server listen failed");
    }

    ~ServerFixture()
    {
        if (fd_ >= 0) (void)::close(fd_);
        if (!path_.empty()) (void)::unlink(path_.c_str());
        if (!directory_.empty()) (void)::rmdir(directory_.c_str());
    }

    const std::string& path() const noexcept { return path_; }
    int accept_one()
    {
        const int accepted = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        require(accepted >= 0,
                std::string("server accept failed: ") + std::strerror(errno));
        return accepted;
    }

private:
    int fd_ = -1;
    std::string directory_;
    std::string path_;
};

ipc::SpatialRoiUnixSocketConnectorConfig config_for(const std::string& path)
{
    ipc::SpatialRoiUnixSocketConnectorConfig config;
    config.socket_path = path;
    config.connect_timeout = std::chrono::milliseconds(500);
    config.transport_config.expected_peer_pid = ::getpid();
    config.transport_config.expected_peer_uid = ::geteuid();
    return config;
}

void test_connect_and_roundtrip()
{
    ServerFixture server;
    std::string error;
    auto transport =
        ipc::SpatialRoiUnixSocketConnector::Connect(config_for(server.path()),
                                                     &error);
    require(transport != nullptr, "connector failed: " + error);
    require(transport->peer_credentials().pid == ::getpid() &&
                transport->peer_credentials().uid == ::geteuid(),
            "connector lost exact server credentials");
    int peer = server.accept_one();
    require(transport->WriteLine("producer\n", &error),
            "connector transport write failed: " + error);
    std::array<char, 16> bytes{};
    require(::recv(peer, bytes.data(), bytes.size(), 0) == 9 &&
                std::string(bytes.data(), 9) == "producer\n",
            "server did not receive connector bytes");
    require(::send(peer, "recorder\n", 9, MSG_NOSIGNAL) == 9,
            "server response failed");
    const auto received =
        transport->ReadLine(std::chrono::milliseconds(500), 64);
    require(received.status == ipc::SpatialRoiIpcTransportReadStatus::kLine &&
                received.line == "recorder\n",
            "connector transport did not receive server response");
    (void)::close(peer);
}

void test_requires_exact_endpoint_and_credentials()
{
    std::string error;
    auto config = config_for("relative.sock");
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "relative endpoint was accepted");

    std::array<char, 64> pattern{};
    const std::string prefix = "/tmp/orange_spatial_roi_connector_file_";
    std::memcpy(pattern.data(), prefix.data(), prefix.size());
    std::memcpy(pattern.data() + prefix.size(), "XXXXXX", 7);
    const int regular_fd = ::mkstemp(pattern.data());
    require(regular_fd >= 0, "regular-file fixture creation failed");
    (void)::close(regular_fd);
    config = config_for(pattern.data());
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "regular file endpoint was accepted");
    (void)::unlink(pattern.data());

    ServerFixture server;
    config = config_for(server.path());
    config.transport_config.expected_peer_pid.reset();
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "connector without expected recorder pid was accepted");

    config = config_for(server.path());
    config.transport_config.expected_peer_uid.reset();
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "connector without expected recorder uid was accepted");

    config = config_for(server.path());
    config.connect_timeout = std::chrono::milliseconds(0);
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "connector accepted a zero timeout");

    config = config_for(server.path());
    config.connect_timeout = std::chrono::minutes(5) +
                             std::chrono::milliseconds(1);
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "connector accepted an unbounded timeout");

    require(::chmod(server.path().c_str(), 0660) == 0,
            "could not create wrong-mode endpoint fixture");
    config = config_for(server.path());
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "connector accepted a socket whose mode was not exactly 0600");

    config = config_for("/tmp/../tmp/unsafe.sock");
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "connector accepted a path containing dot-dot");

    config = config_for("/tmp/" + std::string(104, 'x'));
    require(!ipc::SpatialRoiUnixSocketConnector::Connect(config, &error),
            "connector accepted a path exceeding sockaddr_un");
}

void test_credential_mismatch_consumes_no_transport()
{
    ServerFixture server;
    auto config = config_for(server.path());
    config.transport_config.expected_peer_pid = ::getpid() + 1;
    std::string error;
    auto transport = ipc::SpatialRoiUnixSocketConnector::Connect(config, &error);
    require(!transport && error.find("pid") != std::string::npos,
            "wrong recorder pid was accepted or lost its diagnostic");
    const int peer = server.accept_one();
    std::array<char, 1> byte{};
    require(::recv(peer, byte.data(), byte.size(), 0) == 0,
            "credential-rejected connector did not close its endpoint");
    (void)::close(peer);
}

}  // namespace

int main()
{
    try {
        test_connect_and_roundtrip();
        test_requires_exact_endpoint_and_credentials();
        test_credential_mismatch_consumes_no_transport();
        std::cout << "spatial_roi_unix_socket_connector_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_unix_socket_connector_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
