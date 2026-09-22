#pragma once

#include <mutex>

// Exclusive ownership of a preview staging buffer (the device buffer a
// display or crop-preview worker writes and the GUI thread copies into the
// OpenGL PBO). The worker holds the lock from its first write until its
// stream has completed; the GUI thread holds it across map -> copy -> unmap
// -> stream sync. Keyed by the staging pointer so workers need no new
// constructor arguments. A null pointer yields an empty lock.
namespace orange::gui {

std::unique_lock<std::mutex> lock_preview_staging(const void* staging);
// Non-blocking variant for the GUI thread: an empty (unowned) lock means the
// worker is mid-write and this upload should be skipped.
std::unique_lock<std::mutex> try_lock_preview_staging(const void* staging);

}  // namespace orange::gui
