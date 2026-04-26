// src/modern_recording_pipeline.cpp

#include "modern_recording_pipeline.h"

#include <algorithm>
#include <iostream>

#include "encoder_hw_worker.h"
#include "encoder_preprocess_worker.h"
#include "project.h"
#include "recording_ingress.h"
#include "shared_recording_output.h"

namespace {
int default_preprocess_surface_pitch(const RecordingOutputConfig& output_config,
                                     const CameraParams* camera_params)
{
    if (output_config.resolved_width > 0) {
        return output_config.resolved_width;
    }
    return camera_params ? static_cast<int>(camera_params->width) : 0;
}

uint32_t recording_gop_length_or_fallback(const ResolvedRecordingConfig& config,
                                          const CameraParams* camera_params)
{
    if (config.encode.gop_length > 0) {
        return static_cast<uint32_t>(config.encode.gop_length);
    }
    if (camera_params && camera_params->frame_rate > 0) {
        return static_cast<uint32_t>(camera_params->frame_rate);
    }
    return 1;
}
}

ModernRecordingPipeline::ModernRecordingPipeline(
    CameraParams* camera_params,
    const ResolvedRecordingConfig& resolved_recording_config,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    CameraControl* camera_control,
    const std::string& recording_sink_mode
)
    : camera_params_(camera_params),
      recording_gpu_id_(
          resolved_recording_config.recording_gpu_id >= 0
              ? resolved_recording_config.recording_gpu_id
              : camera_params->gpu_id),
      resolved_recording_config_(resolved_recording_config)
{
    const std::string normalized_sink_mode = normalize_recording_sink_mode(recording_sink_mode);
    if (normalized_sink_mode == "preprocess_only") {
        const int preprocess_surface_pitch =
            default_preprocess_surface_pitch(resolved_recording_config_.output, camera_params_);

        const std::string preprocess_name = "Preprocess_Cam_" + camera_params_->camera_serial;
        preprocess_worker_ = std::make_unique<EncoderPreprocessWorker>(
            preprocess_name.c_str(),
            camera_params_,
            recording_gpu_id_,
            resolved_recording_config_.output,
            false,
            preprocess_surface_pitch,
            0,
            resolved_recording_config_.resources.encoder_entry_pool_size,
            recycle_queue,
            camera_control);

        recording_ingress_ = std::make_unique<RecordingIngress>(
            preprocess_worker_.get(),
            camera_params_->gpu_id,
            recording_gpu_id_,
            std::max<uint32_t>(1u, static_cast<uint32_t>(camera_params_->frame_rate)),
            resolved_recording_config_,
            &recycle_queue,
            "real",
            camera_params_->camera_serial);

        const RecordingStrategyConfig& resolved_strategy = resolved_recording_config_.strategy;
        const std::string& policy = resolved_strategy.split_gop.source_encoder_policy;
        const bool wants_helper_targets =
            resolved_strategy.split_gop_enabled() &&
            policy == "hybrid_split";
        if (wants_helper_targets) {
            for (int helper_gpu_id : resolved_strategy.split_gop.encoder_gpu_ids) {
                if (helper_gpu_id < 0 || helper_gpu_id == recording_gpu_id_) {
                    continue;
                }
                const bool already_registered = std::any_of(
                    helper_encode_targets_.begin(),
                    helper_encode_targets_.end(),
                    [helper_gpu_id](const HelperEncodeTarget& target) {
                        return target.gpu_id == helper_gpu_id;
                    });
                if (already_registered) {
                    continue;
                }

                HelperEncodeTarget helper_target;
                helper_target.gpu_id = helper_gpu_id;

                const std::string helper_preprocess_name =
                    "Preprocess_Cam_" + camera_params_->camera_serial + "_GPU_" + std::to_string(helper_gpu_id);
                helper_target.preprocess_worker = std::make_unique<EncoderPreprocessWorker>(
                    helper_preprocess_name.c_str(),
                    camera_params_,
                    helper_gpu_id,
                    resolved_recording_config_.output,
                    false,
                    preprocess_surface_pitch,
                    0,
                    resolved_recording_config_.resources.encoder_entry_pool_size,
                    recycle_queue,
                    camera_control);

                recording_ingress_->RegisterHelperPreprocessWorker(
                    helper_gpu_id,
                    helper_target.preprocess_worker.get());
                helper_target.preprocess_worker->PrepareCrossGpuInput(camera_params_->gpu_id);
                helper_encode_targets_.push_back(std::move(helper_target));
            }
        }
    } else if (!is_real_recording_sink_mode(normalized_sink_mode)) {
        recording_ingress_ = std::make_unique<RecordingIngress>(
            nullptr,
            camera_params_->gpu_id,
            recording_gpu_id_,
            recording_gop_length_or_fallback(resolved_recording_config_, camera_params_),
            resolved_recording_config_,
            &recycle_queue,
            normalized_sink_mode,
            camera_params_->camera_serial);
    } else {
        shared_recording_output_ = std::make_shared<SharedRecordingOutput>();

        const std::string hw_encoder_name = "HW_Encoder_Cam_" + camera_params_->camera_serial;
        hw_worker_ = std::make_unique<EncoderHwWorker>(
            hw_encoder_name.c_str(),
            camera_params_,
            recording_gpu_id_,
            resolved_recording_config_,
            resolved_recording_config_.base_folder_name,
            shared_recording_output_,
            true,
            nullptr,
            camera_control);

        const std::string preprocess_name = "Preprocess_Cam_" + camera_params_->camera_serial;
        preprocess_worker_ = std::make_unique<EncoderPreprocessWorker>(
            preprocess_name.c_str(),
            camera_params_,
            recording_gpu_id_,
            resolved_recording_config_.output,
            hw_worker_->direct_input_enabled(),
            hw_worker_->encoder_input_pitch(),
            hw_worker_->encoder_buffer_count(),
            resolved_recording_config_.resources.encoder_entry_pool_size,
            recycle_queue,
            camera_control);

        preprocess_worker_->SetHwWorker(hw_worker_.get());
        hw_worker_->SetPreprocessWorker(preprocess_worker_.get());
        recording_ingress_ = std::make_unique<RecordingIngress>(
            preprocess_worker_.get(),
            camera_params_->gpu_id,
            hw_worker_->encode_gpu_id(),
            hw_worker_->recording_gop_length(),
            resolved_recording_config_,
            &recycle_queue,
            normalized_sink_mode,
            camera_params_->camera_serial);

        const RecordingStrategyConfig& resolved_strategy = resolved_recording_config_.strategy;
        const std::string& policy = resolved_strategy.split_gop.source_encoder_policy;
        const bool wants_helper_targets =
            resolved_strategy.split_gop_enabled() &&
            policy == "hybrid_split";
        if (wants_helper_targets) {
            for (int helper_gpu_id : resolved_strategy.split_gop.encoder_gpu_ids) {
                if (helper_gpu_id < 0 || helper_gpu_id == hw_worker_->encode_gpu_id()) {
                    continue;
                }
                const bool already_registered = std::any_of(
                    helper_encode_targets_.begin(),
                    helper_encode_targets_.end(),
                    [helper_gpu_id](const HelperEncodeTarget& target) {
                        return target.gpu_id == helper_gpu_id;
                    });
                if (already_registered) {
                    continue;
                }

                HelperEncodeTarget helper_target;
                helper_target.gpu_id = helper_gpu_id;

                const std::string helper_hw_name =
                    "HW_Encoder_Cam_" + camera_params_->camera_serial + "_GPU_" + std::to_string(helper_gpu_id);
                ResolvedRecordingConfig helper_resolved_recording_config = resolved_recording_config_;
                helper_resolved_recording_config.recording_gpu_id = helper_gpu_id;
                helper_resolved_recording_config.pre_encoder_reference_capture = PreEncoderReferenceCaptureConfig{};
                helper_target.hw_worker = std::make_unique<EncoderHwWorker>(
                    helper_hw_name.c_str(),
                    camera_params_,
                    helper_gpu_id,
                    helper_resolved_recording_config,
                    resolved_recording_config_.base_folder_name,
                    shared_recording_output_,
                    false,
                    nullptr,
                    camera_control);

                const std::string helper_preprocess_name =
                    "Preprocess_Cam_" + camera_params_->camera_serial + "_GPU_" + std::to_string(helper_gpu_id);
                helper_target.preprocess_worker = std::make_unique<EncoderPreprocessWorker>(
                    helper_preprocess_name.c_str(),
                    camera_params_,
                    helper_gpu_id,
                    resolved_recording_config_.output,
                    helper_target.hw_worker->direct_input_enabled(),
                    helper_target.hw_worker->encoder_input_pitch(),
                    helper_target.hw_worker->encoder_buffer_count(),
                    helper_resolved_recording_config.resources.encoder_entry_pool_size,
                    recycle_queue,
                    camera_control);

                helper_target.preprocess_worker->SetHwWorker(helper_target.hw_worker.get());
                helper_target.hw_worker->SetPreprocessWorker(helper_target.preprocess_worker.get());
                recording_ingress_->RegisterHelperPreprocessWorker(
                    helper_gpu_id,
                    helper_target.preprocess_worker.get());
                helper_target.preprocess_worker->PrepareCrossGpuInput(camera_params_->gpu_id);
                helper_encode_targets_.push_back(std::move(helper_target));
            }
        }
    }

    initialize_split_gop_topology_static_snapshot();
    refresh_split_gop_runtime_topology_snapshot();
}

ModernRecordingPipeline::~ModernRecordingPipeline()
{
    shutdown();
}

void ModernRecordingPipeline::start()
{
    if (recording_ingress_) {
        recording_ingress_->start();
    }
    for (auto& helper_target : helper_encode_targets_) {
        if (helper_target.preprocess_worker) {
            helper_target.preprocess_worker->SetMaxQueueSize(240);
            helper_target.preprocess_worker->StartThread();
        }
        if (helper_target.hw_worker) {
            helper_target.hw_worker->SetMaxQueueSize(240);
            helper_target.hw_worker->StartThread();
        }
    }
    if (preprocess_worker_) {
        preprocess_worker_->SetMaxQueueSize(240);
        preprocess_worker_->StartThread();
    }
    if (hw_worker_) {
        hw_worker_->SetMaxQueueSize(240);
        hw_worker_->StartThread();
    }
}

void ModernRecordingPipeline::request_stop()
{
    refresh_split_gop_runtime_topology_snapshot();
    if (recording_ingress_) {
        recording_ingress_->request_stop();
    }
    for (auto& helper_target : helper_encode_targets_) {
        if (helper_target.hw_worker) {
            helper_target.hw_worker->StopThread();
        }
        if (helper_target.preprocess_worker) {
            helper_target.preprocess_worker->StopThread();
        }
    }
    if (hw_worker_) {
        hw_worker_->StopThread();
    }
    if (preprocess_worker_) {
        preprocess_worker_->StopThread();
    }
}

void ModernRecordingPipeline::shutdown()
{
    refresh_split_gop_runtime_topology_snapshot();
    request_stop();

    for (auto& helper_target : helper_encode_targets_) {
        if (helper_target.hw_worker) {
            std::cout << "Flushing final packets for helper HW encoder "
                      << camera_params_->camera_serial
                      << " on GPU " << helper_target.gpu_id << "..." << std::endl;
            helper_target.hw_worker->flush_and_close();
            helper_target.hw_worker.reset();
        }
        if (helper_target.preprocess_worker) {
            helper_target.preprocess_worker.reset();
        }
    }
    helper_encode_targets_.clear();

    if (hw_worker_) {
        std::cout << "Flushing final packets for main HW encoder "
                  << camera_params_->camera_serial << "..." << std::endl;
        hw_worker_->flush_and_close();
        hw_worker_.reset();
    }

    if (preprocess_worker_) {
        preprocess_worker_.reset();
    }

    if (recording_ingress_) {
        recording_ingress_->shutdown();
        recording_ingress_.reset();
    }
    shared_recording_output_.reset();
}

void ModernRecordingPipeline::initialize_split_gop_topology_static_snapshot()
{
    if (!hw_worker_) {
        return;
    }

    nlohmann::json topology = nlohmann::json::object();
    const int source_gpu_id = camera_params_ ? camera_params_->gpu_id : -1;
    const int primary_encode_gpu_id = hw_worker_->encode_gpu_id();
    topology["source_gpu_id"] = source_gpu_id;
    topology["primary_encode_gpu_id"] = primary_encode_gpu_id;
    topology["source_gpu"] = build_gpu_runtime_info(source_gpu_id);
    topology["primary_encode_gpu"] = build_gpu_runtime_info(primary_encode_gpu_id);
    topology["helper_gpus"] = nlohmann::json::array();
    topology["copy_paths"] = nlohmann::json::array();

    for (const auto& helper_target : helper_encode_targets_) {
        topology["helper_gpus"].push_back(build_gpu_runtime_info(helper_target.gpu_id));

        nlohmann::json pair_info =
            build_gpu_copy_path_static_topology_info(source_gpu_id, helper_target.gpu_id);
        topology["copy_paths"].push_back(std::move(pair_info));
    }

    hw_worker_->SetSplitGopTopologyStaticSnapshot(topology);
}

void ModernRecordingPipeline::refresh_split_gop_runtime_topology_snapshot()
{
    if (!hw_worker_) {
        return;
    }

    const SharedRecordingOutputStats shared_output_stats =
        shared_recording_output_ ? shared_recording_output_->stats() : SharedRecordingOutputStats{};
    nlohmann::json topology = nlohmann::json::object();
    const int source_gpu_id = camera_params_ ? camera_params_->gpu_id : -1;
    topology["source_to_helper_copy_samples_total"] =
        shared_output_stats.source_to_helper_copy.sample_count;
    topology["copy_paths"] = nlohmann::json::array();

    for (const auto& helper_target : helper_encode_targets_) {
        nlohmann::json pair_info = {
            {"source_gpu_id", source_gpu_id},
            {"target_gpu_id", helper_target.gpu_id}
        };
        nlohmann::json capability =
            build_gpu_copy_path_static_topology_info(source_gpu_id, helper_target.gpu_id);
        const auto capability_it = capability.find("peer_access_capability");
        if (capability_it != capability.end() && capability_it->is_object()) {
            pair_info["can_access_peer"] = capability_it->value("can_access_peer", false);
            pair_info["peer_access_required"] = capability_it->value("peer_access_required", false);
        }
        if (helper_target.preprocess_worker) {
            const std::vector<PeerAccessRouteState> observed_states =
                helper_target.preprocess_worker->peer_access_states();
            const PeerAccessRouteState* matched_state = nullptr;
            for (const PeerAccessRouteState& state : observed_states) {
                if (state.source_gpu_id == source_gpu_id) {
                    matched_state = &state;
                    break;
                }
            }
            if (!matched_state && observed_states.size() == 1) {
                matched_state = &observed_states.front();
            }
            if (matched_state) {
                pair_info["peer_access_observation"] = {
                    {"observation_source", "worker_state"},
                    {"can_access_peer", matched_state->can_access_peer},
                    {"enable_attempted", matched_state->peer_access_enable_attempted},
                    {"enabled", matched_state->peer_access_enabled},
                    {"observed_source_gpu_id",
                     matched_state->source_gpu_id}
                };
                if (!matched_state->enable_error.empty()) {
                    pair_info["peer_access_observation"]["enable_error"] =
                        matched_state->enable_error;
                }
            } else if (source_gpu_id != helper_target.gpu_id &&
                       pair_info.value("can_access_peer", false) &&
                       helper_encode_targets_.size() == 1 &&
                       shared_output_stats.source_to_helper_copy.sample_count > 0) {
                pair_info["peer_access_observation"] = {
                    {"observation_source", "successful_source_to_helper_copy_samples"},
                    {"enable_attempted", true},
                    {"enabled", true},
                    {"enable_attempted_inferred", true},
                    {"enabled_inferred", true},
                    {"copy_sample_count",
                     shared_output_stats.source_to_helper_copy.sample_count},
                    {"observed_source_gpu_id", source_gpu_id}
                };
            }
        }
        topology["copy_paths"].push_back(std::move(pair_info));
    }

    hw_worker_->SetSplitGopTopologyRuntimeSnapshot(topology);
}
