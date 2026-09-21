// src/async_line_sink.h
//
// Bounded, thread-backed line writer for per-frame diagnostics. Producers
// append formatted lines and never touch the filesystem; one low-priority
// thread does the write() calls and flushes. A write() on a real-time thread
// can block for tens of milliseconds while ext4 commits its journal under
// heavy writeback; on 2026-09-21 the cadence probe's per-row flush on the
// acquisition thread turned such a stall into a lost camera frame. No hot
// thread (acquisition, YOLO, pose, external handoff) may own an ofstream.
#pragma once

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace orange {

class AsyncLineSink {
public:
    explicit AsyncLineSink(size_t max_queued_lines = static_cast<size_t>(1) << 16)
        : max_queued_lines_(max_queued_lines) {}
    ~AsyncLineSink() { Close(); }
    AsyncLineSink(const AsyncLineSink&) = delete;
    AsyncLineSink& operator=(const AsyncLineSink&) = delete;

    // Truncates path, writes header (may be empty), starts the writer thread.
    bool Open(const std::string& path, const std::string& header) {
        Close();
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file) {
            return false;
        }
        if (!header.empty()) {
            file << header;
        }
        path_ = path;
        file_ = std::move(file);
        dropped_.store(0, std::memory_order_relaxed);
        written_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&AsyncLineSink::Run, this);
        return true;
    }

    bool IsOpen() const { return running_.load(std::memory_order_acquire); }
    const std::string& path() const { return path_; }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t written() const { return written_.load(std::memory_order_relaxed); }

    // Never blocks on I/O. Drops the line (counted) when the queue is full.
    void Append(std::string&& line) {
        if (!IsOpen()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= max_queued_lines_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue_.push_back(std::move(line));
        }
        cv_.notify_one();
    }

    // Drains the queue, flushes, closes the file, joins the thread.
    void Close() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        Drain();
        file_.flush();
        file_.close();
        path_.clear();
    }

private:
    // The sink thread is spawned by a real-time, possibly core-pinned
    // producer and would inherit its policy and affinity; move it to a normal
    // priority on the non-isolated CPUs so it never competes with the hot
    // thread or sits on an isolcpus core.
    static void DetachFromProducerScheduling() {
        sched_param param{};
        param.sched_priority = 0;
        (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
        cpu_set_t mask;
        CPU_ZERO(&mask);
        const long cpu_count = sysconf(_SC_NPROCESSORS_CONF);
        for (long cpu = 0; cpu < cpu_count && cpu < CPU_SETSIZE; ++cpu) {
            CPU_SET(static_cast<int>(cpu), &mask);
        }
        std::ifstream isolated("/sys/devices/system/cpu/isolated");
        std::string spec;
        if (isolated && std::getline(isolated, spec)) {
            size_t pos = 0;
            while (pos < spec.size()) {
                size_t comma = spec.find(',', pos);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                if (item.empty()) continue;
                const size_t dash = item.find('-');
                const int lo = std::atoi(item.substr(0, dash).c_str());
                const int hi = dash == std::string::npos ? lo : std::atoi(item.substr(dash + 1).c_str());
                for (int cpu = lo; cpu <= hi && cpu < CPU_SETSIZE; ++cpu) {
                    CPU_CLR(cpu, &mask);
                }
            }
        }
        if (CPU_COUNT(&mask) > 0) {
            (void)pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
        }
    }

    void Run() {
        DetachFromProducerScheduling();
        std::unique_lock<std::mutex> lock(mutex_);
        while (running_.load(std::memory_order_acquire)) {
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !queue_.empty() || !running_.load(std::memory_order_acquire);
            });
            if (queue_.empty()) {
                continue;
            }
            std::deque<std::string> batch;
            batch.swap(queue_);
            lock.unlock();
            for (const auto& line : batch) {
                file_ << line;
            }
            file_.flush();
            written_.fetch_add(batch.size(), std::memory_order_relaxed);
            lock.lock();
        }
    }

    void Drain() {
        std::deque<std::string> batch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batch.swap(queue_);
        }
        for (const auto& line : batch) {
            file_ << line;
        }
        written_.fetch_add(batch.size(), std::memory_order_relaxed);
    }

    const size_t max_queued_lines_;
    std::string path_;
    std::ofstream file_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> written_{0};
};

}  // namespace orange
