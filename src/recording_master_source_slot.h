#pragma once
#include "recording_master_acquisition.h"

namespace orange::recording {
// One acquisition reader and one serialized control-plane owner. The slot
// outlives the camera thread. Sequential consistency implements hazard-pointer
// publication/recheck. No mutex, refcount, allocation or syscall per frame.
class MasterSourceSlot {
public:
    class Lease {
    public:
        explicit Lease(MasterSourceSlot* slot) noexcept : slot_(slot) {
            if (!slot_) return;
            for (;;) {
                owner_ = slot_->active_.load();
                if (!owner_) return;
                slot_->hazard_.store(owner_);
                if (owner_ == slot_->active_.load()) return;
                slot_->hazard_.store(nullptr);
            }
        }
        ~Lease() { if (slot_ && owner_) slot_->hazard_.store(nullptr); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        MasterAcquisitionJournal* Get() const noexcept { return owner_; }
    private:
        MasterSourceSlot* slot_;
        MasterAcquisitionJournal* owner_ = nullptr;
    };
    // Retain old storage until Idle(), and retire before attaching a replacement.
    void Detach() noexcept { active_.store(nullptr); }
    bool Idle() const noexcept { return hazard_.load() == nullptr; }
    void Attach(MasterAcquisitionJournal* owner) {
        if (active_.load() || !Idle())
            throw std::logic_error("master source slot is not retired");
        active_.store(owner);
    }
private:
    static_assert(std::atomic<MasterAcquisitionJournal*>::is_always_lock_free,
                  "master source handoff requires lock-free pointers");
    std::atomic<MasterAcquisitionJournal*> active_{nullptr}, hazard_{nullptr};
};
} // namespace orange::recording
