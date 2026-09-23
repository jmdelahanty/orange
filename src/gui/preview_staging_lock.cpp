// src/gui/preview_staging_lock.cpp — registry of per-staging-buffer mutexes
// (see preview_staging_lock.h). Separate translation unit so workers that
// exist in both the GUI and the headless client (crop preview worker) link
// without the GL texture code.
#include "gui/preview_staging_lock.h"

#include <map>
#include <memory>
#include <mutex>

namespace orange::gui {

namespace {
std::mutex& staging_mutex_for(const void* staging)
{
    static std::mutex registry_mutex;
    static std::map<const void*, std::unique_ptr<std::mutex>> registry;
    std::lock_guard<std::mutex> guard(registry_mutex);
    auto& slot = registry[staging];
    if (!slot) {
        slot = std::make_unique<std::mutex>();
    }
    return *slot;
}
}  // namespace

std::unique_lock<std::mutex> lock_preview_staging(const void* staging)
{
    if (!staging) {
        return {};
    }
    return std::unique_lock<std::mutex>(staging_mutex_for(staging));
}

std::unique_lock<std::mutex> try_lock_preview_staging(const void* staging)
{
    if (!staging) {
        return {};
    }
    return std::unique_lock<std::mutex>(staging_mutex_for(staging), std::try_to_lock);
}

}  // namespace orange::gui
