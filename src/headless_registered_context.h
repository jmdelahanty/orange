#pragma once
#include "recording_registered_context.h"
#include "spatial_snapshot_worker.h"

namespace orange::recording {
// Camera-level one-shot workers live until acquisition has joined, even after
// their threads stop. Acquisition keeps only these stable non-owning pointers.
class HeadlessRegisteredContext {
public:
    RegisteredContextSet evidence;
    ~HeadlessRegisteredContext() { Stop(); }
    void AddCamera(int index, CameraParams* params, SafeQueue<WORKER_ENTRY*>& recycle, int cpu);
    SpatialSnapshotWorker* Worker(int index) const;
    bool Poll(); // throws on snapshot, worker or publication failure
    void Stop(); // after camera join on shutdown; also safe after all captures
private:
    struct Camera {
        std::unique_ptr<SpatialSnapshotWorker> worker;
        uint64_t request_id = 0;
        bool stopped = false;
    };
    std::map<int, Camera> cameras_;
};
}
