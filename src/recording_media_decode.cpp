#include "recording_media_decode.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace orange::recording {
namespace {
void need(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Decode {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::chrono::steady_clock::time_point deadline;
    ~Decode() {
        av_frame_free(&frame); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&format);
    }
    static int interrupted(void* p) {
        return std::chrono::steady_clock::now() > static_cast<Decode*>(p)->deadline;
    }
};
void open_container(Decode& d, const std::filesystem::path& path, int width, int height) {
    need(width > 0 && height > 0 && width <= 8192 && height <= 8192,
         "invalid authenticated crop raster");
    d.format = avformat_alloc_context();
    need(d.format != nullptr, "media demux allocation failed");
    d.format->interrupt_callback = {Decode::interrupted, &d};
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    auto* demuxer = av_find_input_format("mov");
    need(demuxer != nullptr, "MP4 demuxer unavailable");
    const auto opened = avformat_open_input(&d.format, path.c_str(), demuxer, &options);
    av_dict_free(&options);
    need(opened >= 0, "cannot open encoded crop container");
    // MP4 carries the parameters in its header. Do not run stream-info probing,
    // which can invoke an implicit decoder before our allocation/thread bounds.
    need(d.format->nb_streams == 1,
         "crop container must have exactly one readable video stream");
    const auto* parameters = d.format->streams[0]->codecpar;
    need(parameters->codec_type == AVMEDIA_TYPE_VIDEO && parameters->codec_id == AV_CODEC_ID_HEVC &&
         parameters->width == width && parameters->height == height, "crop container codec/raster mismatch");
}
}
void RequireCropMediaContainerRaster(const std::filesystem::path& path, int width, int height) {
    Decode d;
    d.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    open_container(d, path, width, height);
}
void RequireDecodedCropMedia(const std::filesystem::path& path, int width, int height, uint64_t frames) {
    need(frames > 0, "invalid authenticated crop frame count");
    Decode d;
    // Finite budget proportional to the frame domain, capped at six hours per
    // output. Decode errors/timeouts fail completion, never manufacture frames.
    d.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(
        60 + static_cast<int64_t>(std::min<uint64_t>(frames, 21540)));
    open_container(d, path, width, height);
    const auto* parameters = d.format->streams[0]->codecpar;
    const auto* decoder = avcodec_find_decoder_by_name("hevc");
    need(decoder != nullptr, "software HEVC decoder unavailable");
    d.codec = avcodec_alloc_context3(decoder);
    need(d.codec && avcodec_parameters_to_context(d.codec, parameters) >= 0, "crop decoder setup failed");
    d.codec->thread_count = 1;
    d.codec->err_recognition = AV_EF_EXPLODE | AV_EF_CAREFUL;
    // HEVC's internal CTU envelope is not the visible experimental raster.
    d.codec->max_pixels = static_cast<int64_t>((width + 63) / 64 * 64) * ((height + 63) / 64 * 64);
    need(avcodec_open2(d.codec, decoder, nullptr) >= 0, "crop decoder open failed");
    d.packet = av_packet_alloc(); d.frame = av_frame_alloc();
    need(d.packet && d.frame, "crop decoder frame allocation failed");
    uint64_t packet_count = 0, decoded = 0;
    auto receive = [&] {
        for (;;) {
            need(!Decode::interrupted(&d), "crop decode timed out");
            const int rc = avcodec_receive_frame(d.codec, d.frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
            need(rc >= 0, "crop frame decode failed");
            need(d.frame->width == width && d.frame->height == height &&
                 d.frame->decode_error_flags == 0 && !(d.frame->flags & AV_FRAME_FLAG_CORRUPT),
                 "decoded crop raster/corruption mismatch");
            need(++decoded <= frames, "extra decoded crop frame");
            av_frame_unref(d.frame);
        }
    };
    int rc;
    while ((rc = av_read_frame(d.format, d.packet)) >= 0) {
        need(d.packet->stream_index == 0 && !(d.packet->flags & AV_PKT_FLAG_CORRUPT), "invalid crop packet");
        need(++packet_count <= frames, "extra crop packet");
        need(avcodec_send_packet(d.codec, d.packet) >= 0, "crop decoder rejected packet");
        av_packet_unref(d.packet); receive();
    }
    need(rc == AVERROR_EOF, "crop container read failed");
    need(avcodec_send_packet(d.codec, nullptr) >= 0, "crop decoder flush failed");
    receive();
    need(packet_count == frames && decoded == frames, "crop packet/decoded count differs from master coverage");
}
}
