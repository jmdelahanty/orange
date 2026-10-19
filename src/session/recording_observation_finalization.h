#pragma once

#include "json.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orange::session {

inline constexpr const char* kObservationBindingFinalizationSchemaId =
    "orange.recording.observation_binding_finalization";
inline constexpr const char* kObservationBindingFinalizationRelativePath =
    "recording_observation_bindings/finalized_collection.json";
// Revision N >= 2 of the finalized collection (schema_version 2) lives at
// <prefix><N>.json and supersedes revision N-1 by path and digest; its
// receipts live in receipts/r<N>/. Revision 1 is never rewritten.
inline constexpr const char* kObservationBindingRevisionPrefix =
    "recording_observation_bindings/finalized_collection.r";
inline constexpr const char* kObservationBindingRevisionReasonReceiptV2Upgrade =
    "citrus_receipt_v2_upgrade";

struct RecordingObservationFinalizationResult {
    bool ok = false;
    std::string error;
    nlohmann::json collection = nlohmann::json::object();
    nlohmann::json collection_reference = nlohmann::json::object();
    // Receipt v4 contexts whose receipt disagrees with the acceptance v3
    // declaration (subject_declaration_receipt_disagreement), one line each.
    // Recorded, not refused.
    std::vector<std::string> subject_declaration_disagreements;
};

// Called with the byte size of each file once verified (H5s, Citrus artifacts).
using FinalizationProgress = std::function<void(std::uint64_t bytes)>;

// Validate every Citrus receipt against the immutable Orange request and
// acceptance, independently verify the closed H5 byte size and SHA-256, and
// materialize create-once receipt/collection artifacts. This function is
// intentionally safe to retry with byte-identical evidence.
RecordingObservationFinalizationResult
finalize_recording_observation_bindings(
    const std::string& recording_folder,
    const nlohmann::json& params,
    const FinalizationProgress& progress = {});

// True when the head of the recording's finalized-collection chain is
// finalized and bound for params.experiment_id with exactly params.receipts
// (byte-identical to the collection's records and the stored receipt files).
// Reads only small JSON files: no hashing of H5s or artifacts, no writes.
// reference_out gets the same reference a first finalize returns.
bool find_finalized_recording_observation_reference(
    const std::string& recording_folder,
    const nlohmann::json& params,
    nlohmann::json* reference_out);

// Answers Citrus's finalize_recording_observation_bindings within a bounded
// time. A receipt set already finalized is answered from the stored
// collection. A new set is verified on a background thread (one per
// recording folder): Submit waits up to `inline_wait`, then answers
// "verifying" with progress; Citrus's resend of the same set is the poll.
// A refusal is kept for that receipt set (sticky); a different set while one
// is verifying is refused with finalization_verification_in_progress. The
// "verifying" effect never carries binding_status or context_count, which
// Citrus reads as done.
class RecordingObservationFinalizationJobs {
public:
    struct Answer {
        bool accepted = false;
        std::string error_code;
        std::string error;
        nlohmann::json effect = nlohmann::json::object();
    };
    using Finalizer = std::function<RecordingObservationFinalizationResult(
        const std::string& recording_folder,
        const nlohmann::json& params,
        const FinalizationProgress& progress)>;
    // Runs after a verification succeeds and before every stored answer
    // (the GUI refreshes recording_session.json, a no-op when unchanged).
    using AfterFinalized = std::function<bool(
        const std::string& recording_folder,
        const RecordingObservationFinalizationResult& result,
        std::string* error_out)>;
    struct Options {
        Finalizer finalizer;          // default: finalize_recording_observation_bindings
        AfterFinalized after_finalized;
        std::chrono::milliseconds inline_wait{500};
        int poll_after_ms = 2000;
    };

    RecordingObservationFinalizationJobs();
    explicit RecordingObservationFinalizationJobs(Options options);
    ~RecordingObservationFinalizationJobs();
    RecordingObservationFinalizationJobs(const RecordingObservationFinalizationJobs&) = delete;
    RecordingObservationFinalizationJobs& operator=(const RecordingObservationFinalizationJobs&) = delete;

    Answer Submit(const std::string& recording_folder, const nlohmann::json& params);

private:
    struct Job;
    Answer AnswerFor(const Job& job) const;

    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable done_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;  // by recording folder
};

// Re-mint the receipts of an already bound recording (sealer 3.1.0: Citrus
// receipt v2 for sessions recorded before it). Writes revision N+1 of the
// collection beside the head, create-once; receipts, experiment, contexts,
// requests, acceptances and H5 bytes must match the head except the receipts
// themselves. params: {experiment_id, receipts[v2], reason, revised_at_utc}.
// A retry with the head's exact receipts returns the head unchanged.
RecordingObservationFinalizationResult
upgrade_recording_observation_receipts(
    const std::string& recording_folder,
    const nlohmann::json& params);

// Add the finalized producer-native collection to recording_session.json.
// With an upgraded chain, the head is projected plus `revision_chain`.
// Missing or invalid finalization evidence is represented as explicit
// `unbound` state; it is never upgraded to `bound` by inference.
bool apply_recording_observation_finalization_to_manifest(
    const std::string& recording_folder,
    nlohmann::json* manifest,
    std::string* error_out = nullptr);

// If recording_session.json was already finalized before Citrus returned its
// receipts, atomically refresh only its observation-binding projection from
// the immutable finalized collection. A missing manifest is not an error;
// the ordinary finalizer will consume the collection when it writes one.
bool refresh_recording_session_observation_bindings(
    const std::string& recording_folder,
    std::string* error_out = nullptr);

}  // namespace orange::session
