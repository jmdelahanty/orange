#include "media_content_digest.h"

#include "gui/spatial_layout/sha256.h"
#include "json.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <vector>

namespace orange::media_digest {

namespace {

std::string utc_now_iso8601()
{
    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
    gmtime_r(&ts.tv_sec, &tm);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000);
    return buffer;
}

std::string relative_video_path(const std::string& video_path, const std::string& recording_root)
{
    if (recording_root.empty()) return video_path;
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(recording_root, ec);
    if (ec) return video_path;
    const std::filesystem::path video = std::filesystem::weakly_canonical(video_path, ec);
    if (ec) return video_path;
    const std::filesystem::path rel = video.lexically_relative(root);
    if (rel.empty() || rel.string().rfind("..", 0) == 0) return video_path;
    return rel.generic_string();
}

// Plain read() in 4 MB chunks into a streaming SHA-256; the file is never
// mapped and the page cache is left as it is.
bool hash_file_readback(const std::string& path, std::string* hex, uint64_t* size, std::string* error)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *error = "open failed: " + std::string(std::strerror(errno));
        return false;
    }
    orange::gui::spatial_layout::checksum::StreamingSha256 hasher;
    std::vector<uint8_t> buffer(4u * 1024u * 1024u);
    uint64_t total = 0;
    while (true) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            *error = "read failed: " + std::string(std::strerror(errno));
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        hasher.update(buffer.data(), static_cast<size_t>(n));
        total += static_cast<uint64_t>(n);
    }
    ::close(fd);
    *hex = hasher.final_hex();
    *size = total;
    return true;
}

void lower_thread_priority()
{
    // SCHED_IDLE needs no privilege; it only runs when a core is otherwise
    // idle. nice 19 is the fallback if the policy change is refused.
    sched_param param{};
    param.sched_priority = 0;
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &param) != 0) {
        const pid_t tid = static_cast<pid_t>(::syscall(SYS_gettid));
        (void)::setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 19);
    }
}

}  // namespace

bool write_content_digest_receipt(const ReceiptRequest& request,
                                  std::string* sha256_out,
                                  std::string* error_out)
{
    std::string error;
    std::string hex;
    uint64_t size = 0;
    if (!hash_file_readback(request.video_path, &hex, &size, &error)) {
        if (error_out) *error_out = error;
        return false;
    }
    struct stat st{};
    if (::stat(request.video_path.c_str(), &st) != 0 || static_cast<uint64_t>(st.st_size) != size) {
        if (error_out) *error_out = "file size changed during read-back";
        return false;
    }
    const nlohmann::json receipt = {
        {"schema_id", "orange.media_content_digest"},
        {"schema_version", 1},
        {"algorithm", "sha256"},
        {"sha256", hex},
        {"size_bytes", size},
        {"computed", "after_finalization_readback"},
        {"video_path", relative_video_path(request.video_path, request.recording_root)},
        {"computed_at_utc", utc_now_iso8601()},
        {"session_id", request.session_id},
        {"producer_pid", static_cast<int>(::getpid())}
    };
    const std::string final_path = receipt_path_for(request.video_path);
    const std::string temp_path = final_path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(temp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) {
            if (error_out) *error_out = "could not open " + temp_path;
            return false;
        }
        out << receipt.dump(2) << '\n';
        out.flush();
        if (!out) {
            if (error_out) *error_out = "could not write " + temp_path;
            out.close();
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            return false;
        }
    }
    if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        if (error_out) *error_out = "rename failed: " + std::string(std::strerror(errno));
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
        return false;
    }
    if (sha256_out) *sha256_out = hex;
    return true;
}

ContentDigestHasher::ContentDigestHasher(size_t queue_capacity)
    : queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity)
{
}

ContentDigestHasher::~ContentDigestHasher()
{
    finish(std::chrono::milliseconds(0));
}

void ContentDigestHasher::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_.load(std::memory_order_acquire) || finished_) return;
    accepting_ = true;
    stop_ = false;
    thread_ = std::thread(&ContentDigestHasher::run, this);
    started_.store(true, std::memory_order_release);
}

bool ContentDigestHasher::try_enqueue(ReceiptRequest request)
{
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        // The hasher holds the lock only for queue bookkeeping, never while
        // hashing; still, never wait on it from a writer path.
        std::lock_guard<std::mutex> guard(mutex_);
        counters_.requested++;
        counters_.abandoned++;
        return false;
    }
    counters_.requested++;
    if (!accepting_ || queue_.size() >= queue_capacity_) {
        counters_.abandoned++;
        return false;
    }
    queue_.push_back(std::move(request));
    cv_.notify_one();
    return true;
}

void ContentDigestHasher::finish(std::chrono::milliseconds bounded_wait)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (finished_) return;
    accepting_ = false;
    if (!started_.load(std::memory_order_acquire)) {
        counters_.abandoned += queue_.size();
        queue_.clear();
        finished_ = true;
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + bounded_wait;
    cv_.wait_until(lock, deadline, [&] { return queue_.empty() && !busy_; });
    counters_.abandoned += queue_.size();
    queue_.clear();
    stop_ = true;
    finished_ = true;
    cv_.notify_all();
    lock.unlock();
    if (thread_.joinable()) thread_.join();
}

ReceiptCounters ContentDigestHasher::counters() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

void ContentDigestHasher::run()
{
    lower_thread_priority();
    while (true) {
        ReceiptRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                // stop_ set with an empty queue: done.
                return;
            }
            if (stop_) {
                // finish() already counted what is left as abandoned.
                return;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        std::string sha, error;
        const bool ok = write_content_digest_receipt(request, &sha, &error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (ok) counters_.written++;
            else counters_.failed++;
            cv_.notify_all();
        }
        if (!ok) {
            std::cerr << "[content_digest] receipt failed for " << request.video_path
                      << ": " << error << std::endl;
        }
    }
}

}  // namespace orange::media_digest
