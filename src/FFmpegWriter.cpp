// src/FFmpegWriter.cpp

#include "FFmpegWriter.h"
#include "fsuid_guard.h"
#include "nvtx_profiling.h"
#include <algorithm>
#include <unistd.h>
#include <filesystem>
#ifdef __linux__
#include <pthread.h>
#endif

namespace {
bool is_start_code(const uint8_t* data, size_t size, size_t* start_code_len) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        *start_code_len = 4;
        return true;
    }
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        *start_code_len = 3;
        return true;
    }
    return false;
}

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8)  |
           static_cast<uint32_t>(data[3]);
}

void update_peak(std::atomic<size_t>& peak, size_t value) {
    size_t observed = peak.load(std::memory_order_relaxed);
    while (value > observed &&
           !peak.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}
} // namespace

FFmpegWriterLatencyStats FFmpegWriter::latency_stats() const
{
    std::lock_guard<std::mutex> lock(latency_mutex_);
    return latency_stats_;
}

FFmpegWriter::FFmpegWriter(
    AVCodecID eCodecId,
    int nWidth,
    int nHeight,
    int nFps,
    const char *szOutFilePath,
    const char *metadata_file,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags,
    FFmpegWriterQueueConfig queue_config) : nFps(nFps), queue_config_(queue_config)
{
    // Ensure output files are created as the invoking user even when running under sudo.
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    if (szOutFilePath) {
        output_label_ = std::filesystem::path(szOutFilePath).stem().string();
    }
    codec_id_ = eCodecId;
    if (metadata_file) {
        keyframe_file_ = metadata_file;
    }
    oc = avformat_alloc_context();
    if (!oc) {
        printf("FFMPEG: avformat_alloc_context error");
        return;
    }

    AVOutputFormat *fmt = (AVOutputFormat *)av_guess_format("mp4", NULL, NULL);
    if (!fmt) {
        printf("Invalid format");
        return;
    }
    fmt->video_codec = eCodecId;
    oc->oformat = fmt;

    for (const auto& tag : metadata_tags) {
        if (!tag.first.empty() && !tag.second.empty()) {
            av_dict_set(&oc->metadata, tag.first.c_str(), tag.second.c_str(), 0);
        }
    }

    vs = avformat_new_stream(oc, NULL);
    if (!vs) {
        printf("FFMPEG: Could not alloc video stream");
        return;
    }
    vs->id = 0;
    vs->time_base = AVRational{1, 90000};
    vs->r_frame_rate = AVRational{nFps, 1};

    AVCodecParameters *vpar = vs->codecpar;
    vpar->codec_id = fmt->video_codec;
    vpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vpar->width = nWidth;
    vpar->height = nHeight;
    vpar->format = AV_PIX_FMT_YUV420P;
    vpar->color_range = AVCOL_RANGE_JPEG;

    if (vpar->codec_id == AV_CODEC_ID_H264) {
        vpar->codec_tag = MKTAG('a', 'v', 'c', '1');
    } else if (vpar->codec_id == AV_CODEC_ID_HEVC) {
        vpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }

    if (avio_open(&oc->pb, szOutFilePath, AVIO_FLAG_WRITE) < 0) {
        printf("FFMPEG: Could not open %s\n", szOutFilePath);
        return;
    }

    if (avformat_write_header(oc, NULL)) {
        printf("FFMPEG: avformat_write_header error!\n");
        return;
    }
    open_ = true;
}

FFmpegWriter::~FFmpegWriter()
{
    if (oc) {
        if (open_) {
            write_keyframe_sidecar();
            // Send a NULL packet to muxer for flushing any internally buffered frames.
            av_interleaved_write_frame(oc, NULL);
            av_write_trailer(oc);
        }
        if (oc->pb) {
            avio_closep(&oc->pb);
        }
        avformat_free_context(oc);
    }
}

void FFmpegWriter::push_packet(uint8_t* pData,
                               int nBytes,
                               int64_t nPts,
                               uint64_t gop_index,
                               bool is_last_packet_in_gop,
                               uint64_t gop_release_started_ns)
{
    if (!open_ || !vs) {
        return;
    }
    NVTX_ENCODE_DYNAMIC([&]() {
        return "FFmpegWriter push_packet label=" + output_label_ +
               " pts=" + std::to_string(nPts) +
               " gop=" + std::to_string(gop_index) +
               " bytes=" + std::to_string(nBytes);
    }());
    const uint64_t push_start_ns = steady_clock_now_ns();
    if (nBytes <= 0) {
        return;
    }

    const size_t queued_packets = queued_packets_.load(std::memory_order_relaxed);
    const size_t queued_bytes = queued_bytes_.load(std::memory_order_relaxed);
    const bool packets_limited = queue_config_.max_queued_packets > 0;
    const bool bytes_limited = queue_config_.max_queued_bytes > 0;
    const bool exceeds_packet_limit =
        packets_limited && queued_packets + 1 > queue_config_.max_queued_packets;
    const bool exceeds_byte_limit =
        bytes_limited && queued_bytes + static_cast<size_t>(nBytes) > queue_config_.max_queued_bytes;
    if (exceeds_packet_limit || exceeds_byte_limit) {
        const bool first_overflow = !queue_overflowed_.exchange(true, std::memory_order_relaxed);
        queue_overflow_events_.fetch_add(1, std::memory_order_relaxed);
        if (first_overflow) {
            std::cerr << "FFMPEG: packet queue overflow"
                      << " packets=" << queued_packets
                      << " bytes=" << queued_bytes
                      << " limit_packets=" << queue_config_.max_queued_packets
                      << " limit_bytes=" << queue_config_.max_queued_bytes
                      << std::endl;
        }
        return;
    }

    const uint64_t alloc_copy_start_ns = steady_clock_now_ns();
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, nBytes) < 0) {
        std::cout << "Error, av_new_packet..." << std::endl;
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, pData, nBytes);
    const uint64_t alloc_copy_end_ns = steady_clock_now_ns();

    const bool has_explicit_pts = nPts >= 0;
    const int64_t frame_index = has_explicit_pts ? nPts : sequential_frame_counter_;
    pkt->pts = av_rescale_q(frame_index, AVRational{1, nFps}, vs->time_base);
    pkt->dts = pkt->pts;
    pkt->stream_index = vs->index;
    pkt->duration = av_rescale_q(1, AVRational{1, nFps}, vs->time_base);
    if (has_explicit_pts) {
        sequential_frame_counter_ = std::max<int64_t>(sequential_frame_counter_, frame_index + 1);
    } else {
        sequential_frame_counter_++;
    }

    if (packet_has_idr(pData, static_cast<size_t>(nBytes))) {
        pkt->flags |= AV_PKT_FLAG_KEY;
        keyframe_frames_.push_back(frame_index);
    }
    const size_t new_packet_count = queued_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t new_byte_count =
        queued_bytes_.fetch_add(static_cast<size_t>(pkt->size), std::memory_order_relaxed) +
        static_cast<size_t>(pkt->size);
    update_peak(peak_queued_packets_, new_packet_count);
    update_peak(peak_queued_bytes_, new_byte_count);
    QueuedPacket queued_packet;
    queued_packet.packet = pkt;
    queued_packet.enqueued_at_ns = steady_clock_now_ns();
    queued_packet.gop_index = gop_index;
    queued_packet.is_last_packet_in_gop = is_last_packet_in_gop;
    queued_packet.gop_release_started_ns = gop_release_started_ns;
    const uint64_t queue_push_start_ns = steady_clock_now_ns();
    m_queue.push(queued_packet);
    const uint64_t push_end_ns = steady_clock_now_ns();
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        if (alloc_copy_end_ns >= alloc_copy_start_ns) {
            observe_latency_ns(
                &latency_stats_.packet_alloc_copy,
                alloc_copy_end_ns - alloc_copy_start_ns);
        }
        if (push_end_ns >= queue_push_start_ns) {
            observe_latency_ns(
                &latency_stats_.queue_push,
                push_end_ns - queue_push_start_ns);
        }
        if (push_end_ns >= push_start_ns) {
            observe_latency_ns(
                &latency_stats_.push_packet_total,
                push_end_ns - push_start_ns);
        }
    }
}

void FFmpegWriter::create_thread()
{
    if (!open_) {
        return;
    }
    m_thread = std::thread(&FFmpegWriter::write_thread, this);
#ifdef __linux__
    std::string thread_name = output_label_.empty()
        ? "FFmpegWriter"
        : "FFm_" + output_label_;
    if (thread_name.size() > 15) {
        thread_name.resize(15);
    }
    (void)pthread_setname_np(m_thread.native_handle(), thread_name.c_str());
#endif
}

void FFmpegWriter::quit_thread()
{
    m_queue.push(QueuedPacket{});
}

void FFmpegWriter::join_thread()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FFmpegWriter::write_one_pkt(AVPacket* pkt)
{
    if (!open_ || !oc) {
        return;
    }
    NVTX_ENCODE_DYNAMIC(std::string("FFmpegWriter av_interleaved_write_frame"));
    int ret = av_interleaved_write_frame(oc, pkt);
    if (ret < 0) {
        std::cout << "FFMPEG: Error while writing video frame" << std::endl;
    }
}

void FFmpegWriter::write_thread()
{
    // An exception escaping this thread would call std::terminate and kill
    // the whole process (stranding every encoder shard's in-flight GOPs).
    // Log it, latch the error flag and exit the thread cleanly instead so the
    // owner can still finalize the container.
    try {
        write_thread_loop();
    } catch (const std::exception& e) {
        writer_thread_error_.store(true, std::memory_order_release);
        std::cerr << "FFMPEG: writer thread [" << output_label_
                  << "] exception: " << e.what() << std::endl;
    } catch (...) {
        writer_thread_error_.store(true, std::memory_order_release);
        std::cerr << "FFMPEG: writer thread [" << output_label_
                  << "] non-std exception" << std::endl;
    }
}

void FFmpegWriter::write_thread_loop()
{
    while (true) {
        QueuedPacket queued_packet;
        if (m_queue.pop(queued_packet)) {
            if (queued_packet.packet) {
                const uint64_t dequeue_started_ns = steady_clock_now_ns();
                queued_packets_.fetch_sub(1, std::memory_order_relaxed);
                queued_bytes_.fetch_sub(
                    static_cast<size_t>(queued_packet.packet->size),
                    std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(latency_mutex_);
                    if (dequeue_started_ns >= queued_packet.enqueued_at_ns) {
                        observe_latency_ns(
                            &latency_stats_.queue_wait,
                            dequeue_started_ns - queued_packet.enqueued_at_ns);
                    }
                }

                const uint64_t write_started_ns = steady_clock_now_ns();
                NVTX_ENCODE_DYNAMIC([&]() {
                    return "FFmpegWriter write_packet label=" + output_label_ +
                           " gop=" + std::to_string(queued_packet.gop_index) +
                           " bytes=" + std::to_string(queued_packet.packet->size);
                }());
                write_one_pkt(queued_packet.packet);
                const uint64_t write_finished_ns = steady_clock_now_ns();
                {
                    std::lock_guard<std::mutex> lock(latency_mutex_);
                    observe_latency_ns(
                        &latency_stats_.packet_write,
                        write_finished_ns - write_started_ns);
                    if (queued_packet.is_last_packet_in_gop &&
                        queued_packet.gop_release_started_ns > 0 &&
                        write_finished_ns >= queued_packet.gop_release_started_ns) {
                        observe_latency_ns(
                            &latency_stats_.gop_release_to_last_write,
                            write_finished_ns - queued_packet.gop_release_started_ns);
                    }
                }
                av_packet_free(&queued_packet.packet);
            } else {
                break;
            }
        }
        else {
            usleep(100);
        }
    }
}

std::string FFmpegWriter::keyframe_sidecar_path() const
{
    if (keyframe_file_.empty()) {
        return {};
    }
    std::filesystem::path p(keyframe_file_);
    if (p.extension() == ".csv") {
        p.replace_extension(".json");
    } else if (p.extension().empty()) {
        p += ".json";
    }
    return p.string();
}

void FFmpegWriter::write_keyframe_sidecar()
{
    const std::string out_path = keyframe_sidecar_path();
    if (out_path.empty()) {
        return;
    }
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(out_path, std::ios::trunc);
    if (!out.is_open()) {
        std::cout << "FFMPEG: Failed to write keyframe sidecar " << out_path << std::endl;
        return;
    }

    const char* codec_name = "unknown";
    if (codec_id_ == AV_CODEC_ID_H264) {
        codec_name = "h264";
    } else if (codec_id_ == AV_CODEC_ID_HEVC) {
        codec_name = "hevc";
    }

    out << "{\n";
    out << "  \"codec\": \"" << codec_name << "\",\n";
    out << "  \"fps\": " << nFps << ",\n";
    out << "  \"total_frames\": " << static_cast<int64_t>(sequential_frame_counter_) << ",\n";
    out << "  \"keyframe_frames\": [";
    for (size_t i = 0; i < keyframe_frames_.size(); ++i) {
        if (i) {
            out << ", ";
        }
        out << keyframe_frames_[i];
    }
    out << "]\n";
    out << "}\n";
}

bool FFmpegWriter::packet_has_idr(const uint8_t* data, size_t size) const
{
    if (!data || size == 0) {
        return false;
    }
    if (codec_id_ == AV_CODEC_ID_H264) {
        return packet_has_idr_h264(data, size);
    }
    if (codec_id_ == AV_CODEC_ID_HEVC) {
        return packet_has_idr_hevc(data, size);
    }
    return false;
}

bool FFmpegWriter::packet_has_idr_h264(const uint8_t* data, size_t size) const
{
    bool found_start_code = false;
    for (size_t i = 0; i + 3 < size; ++i) {
        size_t start_len = 0;
        if (is_start_code(data + i, size - i, &start_len)) {
            found_start_code = true;
            size_t nal_start = i + start_len;
            if (nal_start >= size) {
                break;
            }
            uint8_t nal_type = data[nal_start] & 0x1F;
            if (nal_type == 5) {
                return true;
            }
            i = nal_start;
        }
    }
    if (found_start_code) {
        return false;
    }

    size_t offset = 0;
    while (offset + 4 <= size) {
        uint32_t nal_len = read_be32(data + offset);
        offset += 4;
        if (nal_len == 0 || offset + nal_len > size) {
            break;
        }
        uint8_t nal_type = data[offset] & 0x1F;
        if (nal_type == 5) {
            return true;
        }
        offset += nal_len;
    }
    return false;
}

bool FFmpegWriter::packet_has_idr_hevc(const uint8_t* data, size_t size) const
{
    bool found_start_code = false;
    for (size_t i = 0; i + 3 < size; ++i) {
        size_t start_len = 0;
        if (is_start_code(data + i, size - i, &start_len)) {
            found_start_code = true;
            size_t nal_start = i + start_len;
            if (nal_start >= size) {
                break;
            }
            uint8_t nal_type = (data[nal_start] >> 1) & 0x3F;
            if (nal_type == 19 || nal_type == 20) {
                return true;
            }
            i = nal_start;
        }
    }
    if (found_start_code) {
        return false;
    }

    size_t offset = 0;
    while (offset + 4 <= size) {
        uint32_t nal_len = read_be32(data + offset);
        offset += 4;
        if (nal_len == 0 || offset + nal_len > size) {
            break;
        }
        uint8_t nal_type = (data[offset] >> 1) & 0x3F;
        if (nal_type == 19 || nal_type == 20) {
            return true;
        }
        offset += nal_len;
    }
    return false;
}
