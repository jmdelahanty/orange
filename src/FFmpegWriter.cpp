// src/FFmpegWriter.cpp

#include "FFmpegWriter.h"
#include "fsuid_guard.h"
#include <unistd.h>
#include <filesystem>

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
} // namespace

FFmpegWriter::FFmpegWriter(
    AVCodecID eCodecId,
    int nWidth,
    int nHeight,
    int nFps,
    const char *szOutFilePath,
    const char *metadata_file,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags) : nFps(nFps)
{
    // Ensure output files are created as the invoking user even when running under sudo.
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

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

    if (vpar->codec_id == AV_CODEC_ID_H264) {
        vpar->codec_tag = MKTAG('a', 'v', 'c', '1');
    } else if (vpar->codec_id == AV_CODEC_ID_HEVC) {
        vpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }

    if (avio_open(&oc->pb, szOutFilePath, AVIO_FLAG_WRITE) < 0) {
        printf("FFMPEG: Could not open %s", szOutFilePath);
        return;
    }

    if (avformat_write_header(oc, NULL)) {
        printf("FFMPEG: avformat_write_header error!");
        return;
    }
}

FFmpegWriter::~FFmpegWriter()
{
    if (oc) {
        write_keyframe_sidecar();
        // Send a NULL packet to muxer for flushing any internally buffered frames
        av_interleaved_write_frame(oc, NULL);
        av_write_trailer(oc);
        avio_close(oc->pb);
        avformat_free_context(oc);
    }
}

void FFmpegWriter::push_packet(uint8_t* pData, int nBytes, int nPts)
{
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, nBytes) < 0) {
        std::cout << "Error, av_new_packet..." << std::endl;
        return;
    }
    memcpy(pkt->data, pData, nBytes);
    
    const int64_t frame_index = sequential_frame_counter_;
    pkt->pts = av_rescale_q(sequential_frame_counter_++, AVRational{1, nFps}, vs->time_base);
    pkt->dts = pkt->pts;
    pkt->stream_index = vs->index;
    pkt->duration = av_rescale_q(1, AVRational{1, nFps}, vs->time_base);

    if (packet_has_idr(pData, static_cast<size_t>(nBytes))) {
        pkt->flags |= AV_PKT_FLAG_KEY;
        keyframe_frames_.push_back(frame_index);
    }
    m_queue.push(pkt);
}

void FFmpegWriter::create_thread()
{
    m_thread = std::thread(&FFmpegWriter::write_thread, this);
}

void FFmpegWriter::quit_thread()
{
    m_queue.push(nullptr);
}

void FFmpegWriter::join_thread()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FFmpegWriter::write_one_pkt(AVPacket* pkt)
{
    int ret = av_interleaved_write_frame(oc, pkt);
    if (ret < 0) {
        std::cout << "FFMPEG: Error while writing video frame" << std::endl;
    }
}

void FFmpegWriter::write_thread()
{
    while (true) {
        AVPacket* pkt = nullptr;
        if (m_queue.pop(pkt)) {
            if (pkt) {
                write_one_pkt(pkt);
                av_packet_free(&pkt);
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
