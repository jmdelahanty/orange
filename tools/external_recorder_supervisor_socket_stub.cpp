#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<bool> g_stop{false};

void signal_handler(int)
{
    g_stop.store(true, std::memory_order_relaxed);
}

std::string option_value(int argc, char** argv, const std::string& name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string socket_path = option_value(argc, argv, "--socket");
    if (socket_path.empty()) {
        std::cerr << "external_recorder_supervisor_socket_stub requires --socket\n";
        return 2;
    }
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::cerr << "socket path too long: " << socket_path << "\n";
        return 2;
    }

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    unlink(socket_path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EPERM) {
            std::cerr << "bind failed: " << std::strerror(errno) << "\n";
            close(fd);
            return 1;
        }
        close(fd);
        std::ofstream fallback(socket_path);
        fallback << "socket bind not permitted in this test sandbox\n";
        fallback.close();
        std::cout << "socket_stub fallback_file " << socket_path << "\n";
        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        unlink(socket_path.c_str());
        std::cout << "socket_stub stopped\n";
        return 0;
    }
    if (listen(fd, 1) != 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    std::cout << "socket_stub listening " << socket_path << "\n";
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    close(fd);
    unlink(socket_path.c_str());
    std::cout << "socket_stub stopped\n";
    return 0;
}
