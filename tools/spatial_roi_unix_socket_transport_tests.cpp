#include "spatial_roi_unix_socket_transport.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using orange::spatial_roi::ipc::SpatialRoiIpcTransportReadStatus;
using orange::spatial_roi::ipc::SpatialRoiIpcTransportReadResult;
using orange::spatial_roi::ipc::SpatialRoiUnixSocketLineTransport;
using orange::spatial_roi::ipc::SpatialRoiUnixSocketTransportConfig;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "[FAIL] " << message << "\n";
        std::exit(1);
    }
}

std::unique_ptr<SpatialRoiUnixSocketLineTransport> adopt(
    const int fd,
    SpatialRoiUnixSocketTransportConfig config = {})
{
    std::string error;
    auto result = SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
        fd, std::move(config), &error);
    require(result != nullptr, "socketpair adoption failed: " + error);
    return result;
}

std::array<int, 2> make_socketpair()
{
    std::array<int, 2> sockets{-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0,
            std::string("socketpair failed: ") + std::strerror(errno));
    return sockets;
}

void close_pair(std::array<int, 2>& sockets)
{
    for (int& fd : sockets) {
        if (fd >= 0) {
            (void)::close(fd);
            fd = -1;
        }
    }
}

void test_peer_credentials_and_partial_read()
{
    auto sockets = make_socketpair();
    SpatialRoiUnixSocketTransportConfig config;
    config.expected_peer_pid = ::getpid();
    config.expected_peer_uid = ::getuid();
    auto transport = adopt(sockets[0], std::move(config));
    sockets[0] = -1;

    require(transport->peer_credentials().pid == ::getpid(),
            "SO_PEERCRED pid was not captured");
    require(transport->peer_credentials().uid == ::getuid(),
            "SO_PEERCRED uid was not captured");

    const std::string first = "partial";
    require(::send(sockets[1], first.data(), first.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(first.size()),
            "partial test prefix send failed");
    const auto timed_out = transport->ReadLine(std::chrono::milliseconds(10), 64);
    require(timed_out.status == SpatialRoiIpcTransportReadStatus::kTimeout,
            "partial line should time out without becoming EOF");
    require(timed_out.line.empty(), "timeout must not materialize a partial line");

    const std::string suffix = "\n";
    require(::send(sockets[1], suffix.data(), suffix.size(), MSG_NOSIGNAL) == 1,
            "partial test suffix send failed");
    const auto line = transport->ReadLine(std::chrono::milliseconds(200), 64);
    require(line.status == SpatialRoiIpcTransportReadStatus::kLine,
            "partial line was not completed");
    require(line.line == "partial\n", "partial line contents changed");

    close_pair(sockets);
}

void test_write_partial_and_no_reconnect()
{
    auto sockets = make_socketpair();
    SpatialRoiUnixSocketTransportConfig config;
    config.max_wire_message_bytes = 256 * 1024;
    config.max_receive_buffer_bytes = 256 * 1024;
    config.write_timeout = std::chrono::milliseconds(1000);
    auto transport = adopt(sockets[0], std::move(config));
    sockets[0] = -1;

    int send_buffer = 1024;
    (void)::setsockopt(sockets[1],
                       SOL_SOCKET,
                       SO_RCVBUF,
                       &send_buffer,
                       sizeof(send_buffer));
    const std::string expected(200 * 1024, 'x');
    std::string line = expected + "\n";
    std::string received;
    std::thread reader([&] {
        std::array<char, 4096> buffer{};
        while (received.size() < line.size()) {
            const ssize_t count =
                ::recv(sockets[1], buffer.data(), buffer.size(), 0);
            require(count > 0, "peer failed while reading partial write");
            received.append(buffer.data(), static_cast<std::size_t>(count));
        }
    });

    std::string error;
    require(transport->WriteLine(line, &error),
            "partial write did not complete: " + error);
    reader.join();
    require(received == line, "partial write changed wire bytes");

    transport->Close();
    require(transport->closed(), "Close did not close adopted descriptor");
    error.clear();
    require(!transport->WriteLine("after-close\n", &error),
            "closed transport unexpectedly reconnected/wrote");
    require(!error.empty(), "closed write did not explain failure");
    const auto closed_read = transport->ReadLine(std::chrono::milliseconds(1), 64);
    require(closed_read.status == SpatialRoiIpcTransportReadStatus::kError,
            "closed transport should report read error");

    close_pair(sockets);
}

void test_cross_thread_shutdown_interrupts_owner_without_concurrent_close()
{
    auto sockets = make_socketpair();
    auto transport = adopt(sockets[0]);
    sockets[0] = -1;

    SpatialRoiIpcTransportReadResult read;
    std::thread owner([&] {
        read = transport->ReadLine(std::chrono::seconds(5), 64);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string error;
    require(transport->RequestShutdown(&error),
            "cross-thread shutdown request failed: " + error);
    require(transport->RequestShutdown(&error),
            "repeated shutdown request was not idempotent: " + error);
    owner.join();
    require(read.status == SpatialRoiIpcTransportReadStatus::kEof ||
                read.status == SpatialRoiIpcTransportReadStatus::kError,
            "shutdown request did not interrupt the transport owner");
    // The request itself never closes fd_; the awakened owner is allowed to
    // consume shutdown EOF and terminal-close the transport before it returns.
    transport->Close();
    require(transport->closed(),
            "post-join Close did not release the shutdown descriptor");

    close_pair(sockets);
}

void test_failed_shutdown_request_can_retry()
{
    auto sockets = make_socketpair();
    auto transport = adopt(sockets[0]);
    sockets[0] = -1;

    // Close removes the private cancellation capability. Both calls must
    // report failure; the second call specifically proves a failed request was
    // not permanently recorded as successful.
    transport->Close();
    std::string error;
    require(!transport->RequestShutdown(&error),
            "shutdown request on a closed capability unexpectedly succeeded");
    require(!error.empty(), "failed shutdown request omitted its diagnostic");
    error.clear();
    require(!transport->RequestShutdown(&error),
            "failed shutdown request was permanently suppressed");
    require(!error.empty(), "retried shutdown failure omitted its diagnostic");

    close_pair(sockets);
}

void test_eof_and_oversized_line()
{
    {
        auto sockets = make_socketpair();
        auto transport = adopt(sockets[0]);
        sockets[0] = -1;
        require(::shutdown(sockets[1], SHUT_WR) == 0, "shutdown failed");
        const auto eof = transport->ReadLine(std::chrono::milliseconds(100), 64);
        require(eof.status == SpatialRoiIpcTransportReadStatus::kEof,
                "peer shutdown should be distinguished from timeout");
        require(eof.line.empty(), "EOF must not return a line");
        close_pair(sockets);
    }

    {
        auto sockets = make_socketpair();
        auto transport = adopt(sockets[0]);
        sockets[0] = -1;
        const std::string partial = "unterminated";
        require(::send(sockets[1], partial.data(), partial.size(), MSG_NOSIGNAL) ==
                    static_cast<ssize_t>(partial.size()),
                "truncated-line prefix send failed");
        require(::shutdown(sockets[1], SHUT_WR) == 0,
                "truncated-line shutdown failed");
        const auto truncated =
            transport->ReadLine(std::chrono::milliseconds(100), 64);
        require(truncated.status == SpatialRoiIpcTransportReadStatus::kError,
                "unterminated EOF was classified as clean EOF");
        require(truncated.error.find("line terminator") != std::string::npos,
                "unterminated EOF diagnostic lost truncation reason");
        close_pair(sockets);
    }

    {
        auto sockets = make_socketpair();
        SpatialRoiUnixSocketTransportConfig config;
        config.max_wire_message_bytes = 8;
        config.max_receive_buffer_bytes = 8;
        auto transport = adopt(sockets[0], std::move(config));
        sockets[0] = -1;
        const std::string exact = "1234567\n";
        require(::send(sockets[1],
                       exact.data(),
                       exact.size(),
                       MSG_NOSIGNAL) == static_cast<ssize_t>(exact.size()),
                "exact-limit test send failed");
        const auto result =
            transport->ReadLine(std::chrono::milliseconds(100), 8);
        require(result.status == SpatialRoiIpcTransportReadStatus::kLine &&
                    result.line == exact,
                "line exactly at the wire bound was rejected");
        close_pair(sockets);
    }

    {
        auto sockets = make_socketpair();
        SpatialRoiUnixSocketTransportConfig config;
        config.max_wire_message_bytes = 8;
        config.max_receive_buffer_bytes = 8;
        auto transport = adopt(sockets[0], std::move(config));
        sockets[0] = -1;
        const std::string oversized = "12345678\n";
        require(::send(sockets[1],
                       oversized.data(),
                       oversized.size(),
                       MSG_NOSIGNAL) == static_cast<ssize_t>(oversized.size()),
                "oversized test send failed");
        const auto result =
            transport->ReadLine(std::chrono::milliseconds(100), 64);
        require(result.status == SpatialRoiIpcTransportReadStatus::kTooLarge,
                "oversized line was not rejected");
        require(result.line.empty(),
                "oversized line was materialized into the result");
        require(transport->closed(), "oversized line did not quarantine endpoint");
        close_pair(sockets);
    }

    {
        auto sockets = make_socketpair();
        SpatialRoiUnixSocketTransportConfig config;
        config.max_wire_message_bytes = 16;
        config.max_receive_buffer_bytes = 16;
        auto transport = adopt(sockets[0], std::move(config));
        sockets[0] = -1;
        const std::string line = "1234\n";
        require(::send(sockets[1], line.data(), line.size(), MSG_NOSIGNAL) ==
                    static_cast<ssize_t>(line.size()),
                "per-call-limit test send failed");
        const auto result =
            transport->ReadLine(std::chrono::milliseconds(100), 4);
        require(result.status == SpatialRoiIpcTransportReadStatus::kTooLarge,
                "per-call wire bound was not enforced");
        close_pair(sockets);
    }
}

void test_write_validation_and_credential_rejection()
{
    {
        auto sockets = make_socketpair();
        auto transport = adopt(sockets[0]);
        sockets[0] = -1;
        std::string error;
        require(!transport->WriteLine("missing-newline", &error),
                "line without newline was accepted");
        require(!transport->closed(), "invalid caller line closed transport");
        require(!error.empty(), "invalid caller line had no diagnostic");
        require(!transport->WriteLine("two\nlines\n", &error),
                "embedded newline was accepted");
        close_pair(sockets);
    }

    {
        auto sockets = make_socketpair();
        SpatialRoiUnixSocketTransportConfig config;
        config.expected_peer_pid = static_cast<pid_t>(::getpid() + 1);
        std::string error;
        auto transport = SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
            sockets[0], std::move(config), &error);
        sockets[0] = -1;
        require(transport == nullptr, "wrong SO_PEERCRED pid was accepted");
        require(error.find("pid") != std::string::npos,
                "credential mismatch diagnostic omitted pid");
        close_pair(sockets);
    }
}

void test_chunked_coalesced_read_and_hard_zero_deadline()
{
    auto sockets = make_socketpair();
    const int inspection_fd = ::dup(sockets[0]);
    require(inspection_fd >= 0, "failed to duplicate test endpoint");

    SpatialRoiUnixSocketTransportConfig config;
    config.max_wire_message_bytes = 64 * 1024;
    config.max_receive_buffer_bytes = 64 * 1024;
    auto transport = adopt(sockets[0], std::move(config));
    sockets[0] = -1;

    const std::string coalesced = "first\nsecond\n";
    require(::send(sockets[1],
                   coalesced.data(),
                   coalesced.size(),
                   MSG_NOSIGNAL) == static_cast<ssize_t>(coalesced.size()),
            "coalesced send failed");
    const auto first =
        transport->ReadLine(std::chrono::milliseconds(100), 64 * 1024);
    const auto second =
        transport->ReadLine(std::chrono::milliseconds(0), 64 * 1024);
    require(first.status == SpatialRoiIpcTransportReadStatus::kLine &&
                first.line == "first\n" &&
                second.status == SpatialRoiIpcTransportReadStatus::kLine &&
                second.line == "second\n",
            "coalesced lines were not split and retained exactly");

    const std::string unterminated(8192, 'z');
    require(::send(sockets[1],
                   unterminated.data(),
                   unterminated.size(),
                   MSG_NOSIGNAL) == static_cast<ssize_t>(unterminated.size()),
            "deadline adversary send failed");
    const auto timed_out =
        transport->ReadLine(std::chrono::milliseconds(0), 64 * 1024);
    require(timed_out.status == SpatialRoiIpcTransportReadStatus::kTimeout,
            "zero-timeout read must perform only one immediate bounded probe");
    int kernel_pending = 0;
    require(::ioctl(inspection_fd, FIONREAD, &kernel_pending) == 0,
            "failed to inspect pending test bytes");
    require(kernel_pending > 0,
            "expired read drained a continuously-ready socket past its deadline");

    require(::send(sockets[1], "\n", 1, MSG_NOSIGNAL) == 1,
            "deadline completion newline send failed");
    const auto completed =
        transport->ReadLine(std::chrono::milliseconds(200), 64 * 1024);
    require(completed.status == SpatialRoiIpcTransportReadStatus::kLine &&
                completed.line == unterminated + "\n",
            "resumed bounded read lost bytes after timeout");

    (void)::close(inspection_fd);
    close_pair(sockets);
}

void test_partial_write_timeout_and_peer_close()
{
    {
        auto sockets = make_socketpair();
        int send_buffer = 4096;
        require(::setsockopt(sockets[0],
                             SOL_SOCKET,
                             SO_SNDBUF,
                             &send_buffer,
                             sizeof(send_buffer)) == 0,
                "failed to constrain transport send buffer");
        SpatialRoiUnixSocketTransportConfig config;
        config.max_wire_message_bytes = 1024 * 1024;
        config.max_receive_buffer_bytes = 1024 * 1024;
        config.write_timeout = std::chrono::milliseconds(10);
        auto transport = adopt(sockets[0], std::move(config));
        sockets[0] = -1;

        std::string line(512 * 1024, 'w');
        line.push_back('\n');
        std::string error;
        require(!transport->WriteLine(line, &error),
                "undrained partial write did not hit its hard deadline");
        require(transport->closed() && error.find("timed out") != std::string::npos,
                "partial write timeout did not terminal-close with a diagnostic");
        char byte = '\0';
        require(::recv(sockets[1], &byte, 1, 0) == 1,
                "write-timeout test did not prove that a prefix was sent");
        close_pair(sockets);
    }

    {
        auto sockets = make_socketpair();
        auto transport = adopt(sockets[0]);
        sockets[0] = -1;
        require(::close(sockets[1]) == 0, "failed to close peer endpoint");
        sockets[1] = -1;
        std::string error;
        require(!transport->WriteLine("peer-closed\n", &error),
                "write to a closed peer unexpectedly succeeded");
        require(transport->closed(),
                "closed-peer write did not terminal-close transport");
    }
}

void test_adoption_rejects_and_closes_invalid_descriptors()
{
    {
        auto sockets = make_socketpair();
        SpatialRoiUnixSocketTransportConfig config;
        config.write_timeout = std::chrono::milliseconds::max();
        std::string error;
        auto transport = SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
            sockets[0], std::move(config), &error);
        require(transport == nullptr,
                "effectively infinite write timeout was accepted");
        sockets[0] = -1;
        close_pair(sockets);
    }

    {
        std::array<int, 2> pipe_fds{-1, -1};
        require(::pipe(pipe_fds.data()) == 0, "pipe creation failed");
        std::string error;
        auto transport = SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
            pipe_fds[0], {}, &error);
        require(transport == nullptr, "non-socket descriptor was adopted");
        errno = 0;
        require(::fcntl(pipe_fds[0], F_GETFD) == -1 && errno == EBADF,
                "failed adoption did not close descriptor exactly once");
        (void)::close(pipe_fds[1]);
    }

    {
        std::array<int, 2> datagrams{-1, -1};
        require(::socketpair(AF_UNIX, SOCK_DGRAM, 0, datagrams.data()) == 0,
                "datagram socketpair creation failed");
        std::string error;
        auto transport = SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
            datagrams[0], {}, &error);
        require(transport == nullptr, "datagram socket was adopted");
        errno = 0;
        require(::fcntl(datagrams[0], F_GETFD) == -1 && errno == EBADF,
                "rejected datagram descriptor was not closed");
        (void)::close(datagrams[1]);
    }

    {
        const int unconnected = ::socket(AF_UNIX, SOCK_STREAM, 0);
        require(unconnected >= 0, "unconnected socket creation failed");
        std::string error;
        auto transport = SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
            unconnected, {}, &error);
        require(transport == nullptr, "unconnected stream socket was adopted");
        errno = 0;
        require(::fcntl(unconnected, F_GETFD) == -1 && errno == EBADF,
                "rejected unconnected descriptor was not closed");
    }

    {
        auto sockets = make_socketpair();
        auto transport = adopt(sockets[0]);
        sockets[0] = -1;
        const auto result = transport->ReadLine(
            std::chrono::milliseconds::max(), 64);
        require(result.status == SpatialRoiIpcTransportReadStatus::kError,
                "effectively infinite read timeout was accepted");
        require(!transport->closed(),
                "invalid caller timeout terminal-closed a healthy endpoint");
        close_pair(sockets);
    }
}

}  // namespace

int main()
{
    test_peer_credentials_and_partial_read();
    test_write_partial_and_no_reconnect();
    test_cross_thread_shutdown_interrupts_owner_without_concurrent_close();
    test_failed_shutdown_request_can_retry();
    test_eof_and_oversized_line();
    test_write_validation_and_credential_rejection();
    test_chunked_coalesced_read_and_hard_zero_deadline();
    test_partial_write_timeout_and_peer_close();
    test_adoption_rejects_and_closes_invalid_descriptors();
    std::cout << "spatial_roi_unix_socket_transport_tests: all tests passed\n";
    return 0;
}
