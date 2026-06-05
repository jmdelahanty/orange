// src/bounded_object_pool.h

#ifndef ORANGE_BOUNDED_OBJECT_POOL_H
#define ORANGE_BOUNDED_OBJECT_POOL_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

template <typename T>
struct DefaultBoundedObjectPoolReset {
    void operator()(T& object) const
    {
        object = T{};
    }
};

template <typename T, typename Reset = DefaultBoundedObjectPoolReset<T>>
class BoundedObjectPool {
public:
    struct Stats {
        size_t capacity = 0;
        size_t available = 0;
        size_t active = 0;
        size_t high_water = 0;
        uint64_t borrow_misses = 0;
        uint64_t invalid_returns = 0;
        uint64_t double_returns = 0;
    };

    BoundedObjectPool() = default;

    BoundedObjectPool(std::string name, size_t capacity, Reset reset = Reset{})
        : name_(std::move(name)),
          storage_(capacity),
          in_use_(capacity, 0),
          reset_(std::move(reset))
    {
        free_.reserve(capacity);
        for (auto& object : storage_) {
            free_.push_back(&object);
        }
    }

    BoundedObjectPool(const BoundedObjectPool&) = delete;
    BoundedObjectPool& operator=(const BoundedObjectPool&) = delete;

    T* Borrow()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty()) {
            ++borrow_misses_;
            return nullptr;
        }

        T* object = free_.back();
        free_.pop_back();
        const size_t index = static_cast<size_t>(object - storage_.data());
        in_use_[index] = 1;
        const size_t active = storage_.size() - free_.size();
        high_water_ = std::max(high_water_, active);
        return object;
    }

    bool Return(T* object)
    {
        if (!object) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_locked(object)) {
            ++invalid_returns_;
            return false;
        }

        const size_t index = static_cast<size_t>(object - storage_.data());
        if (!in_use_[index]) {
            ++double_returns_;
            return false;
        }

        reset_(*object);
        in_use_[index] = 0;
        free_.push_back(object);
        return true;
    }

    bool Owns(T* object) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return owns_locked(object);
    }

    Stats GetStats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats stats;
        stats.capacity = storage_.size();
        stats.available = free_.size();
        stats.active = storage_.size() - free_.size();
        stats.high_water = high_water_;
        stats.borrow_misses = borrow_misses_;
        stats.invalid_returns = invalid_returns_;
        stats.double_returns = double_returns_;
        return stats;
    }

    const std::string& name() const { return name_; }

private:
    bool owns_locked(T* object) const
    {
        if (!object || storage_.empty()) {
            return false;
        }
        const T* begin = storage_.data();
        const T* end = begin + storage_.size();
        return object >= begin && object < end;
    }

    std::string name_;
    std::vector<T> storage_;
    std::vector<uint8_t> in_use_;
    std::vector<T*> free_;
    Reset reset_;
    size_t high_water_ = 0;
    uint64_t borrow_misses_ = 0;
    uint64_t invalid_returns_ = 0;
    uint64_t double_returns_ = 0;
    mutable std::mutex mutex_;
};

#endif  // ORANGE_BOUNDED_OBJECT_POOL_H
