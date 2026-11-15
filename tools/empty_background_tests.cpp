// Empty-dish background core (src/empty_background.h) on synthetic Mono8 frames, in temporary
// folders: the exact 17-frame median, orange.image_levels.v1 statistics, the PNG form, the
// capture writer (images first, background.json last, digests), the store lookup, and the
// record-start binding (never selected, attached, drift per checked setting, frame rate not
// checked, missing camera, camera failed in the capture, copy failures, refusals).
//
//   empty_background_tests [<sample dir>]
// With a sample dir, a capture descriptor and start snapshots of every block shape are left
// there for tools/empty_background_schema_tests.py (jsonschema via validate_output_schemas.py).
#include "empty_background.h"
#include "orange_outputs/orange_empty_background_capture_v2.h"
#include "orange_outputs/orange_recording_empty_background_v1.h"
#include "orange_outputs/orange_recording_empty_background_v2.h"
#include "outputs/common/digest.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace eb = orange::empty_background;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
    failures += ok ? 0 : 1;
}

template <class F>
std::string throws(F&& f) {
    try {
        f();
    } catch (const std::exception& ex) {
        return ex.what();
    }
    return {};
}

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

fs::path temp_dir(const std::string& tag) {
    std::string pattern = (fs::temp_directory_path() / ("orange_empty_background_" + tag + "_XXXXXX")).string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    return fs::path(pattern);
}

eb::CameraConfiguration config(std::uint64_t width, std::uint64_t height) {
    eb::CameraConfiguration c;
    c.width = width;
    c.height = height;
    c.offset_x = 0;
    c.offset_y = 1360;
    c.pixel_format = "Mono8";
    c.frame_rate_hz = 250;
    c.exposure_us = 150;
    c.gain = 256;
    c.focus = 3185;
    c.iris = 19;
    return c;
}

std::vector<eb::SourceFrame> identities(std::uint64_t first_stream, std::uint64_t first_camera) {
    std::vector<eb::SourceFrame> out;
    std::uint64_t camera = first_camera;
    for (std::size_t i = 0; i < eb::kFrameCount; ++i) {
        out.push_back({first_stream + i, camera, 1793600000000000000ull + i * 4000000ull,
                       1793599963009100000ull + i * 4000000ull});
        camera = camera == 65535 ? 1 : camera + 1;
    }
    return out;
}

// 17 noisy frames around a gradient; some pixels clip in a few frames.
std::vector<std::vector<std::uint8_t>> noisy_frames(std::size_t width, std::size_t height, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, 6.0);
    std::vector<std::vector<std::uint8_t>> frames(eb::kFrameCount, std::vector<std::uint8_t>(width * height));
    for (auto& frame : frames)
        for (std::size_t i = 0; i < frame.size(); ++i) {
            const double base = 30.0 + 220.0 * static_cast<double>(i % width) / static_cast<double>(width);
            frame[i] = static_cast<std::uint8_t>(std::clamp(base + noise(rng), 0.0, 255.0));
        }
    return frames;
}

std::vector<std::uint8_t> brute_median(const std::vector<std::vector<std::uint8_t>>& frames) {
    std::vector<std::uint8_t> out(frames[0].size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::vector<std::uint8_t> values;
        for (const auto& f : frames) values.push_back(f[i]);
        std::sort(values.begin(), values.end());
        out[i] = values[8];
    }
    return out;
}

eb::gen::OrangeEmptyBackgroundCaptureV1SceneDeclaration scene() {
    eb::gen::OrangeEmptyBackgroundCaptureV1SceneDeclaration d;
    d.note = "water, no fish";
    d.stimulus_display.state = "on";
    eb::gen::OrangeEmptyBackgroundCaptureV1SceneDeclarationStimulusDisplayShown shown;
    shown.kind = "solid_color";
    shown.rgba8 = std::vector<std::uint64_t>{255, 0, 0, 255};
    d.stimulus_display.shown = shown;
    return d;
}

constexpr std::size_t kW = 64, kH = 48;

// A store with one capture: Cam2010093 and Cam2010094 captured, Cam2010095 failed.
eb::CaptureReceipt make_capture(const fs::path& store, const std::string& id, const std::string& at,
                                std::vector<std::vector<std::uint8_t>>* medians = nullptr) {
    std::vector<eb::CameraBurst> bursts;
    for (const char* serial : {"2010093", "2010094"}) {
        eb::CameraBurst b;
        b.camera_serial = serial;
        b.configuration = config(kW, kH);
        b.source_frames = identities(1000, 65530);  // wraps 65535 -> 1 inside the burst
        b.frames = noisy_frames(kW, kH, static_cast<unsigned>(std::stoul(serial) % 97));
        if (medians) medians->push_back(brute_median(b.frames));
        bursts.push_back(std::move(b));
    }
    eb::CameraBurst failed;
    failed.camera_serial = "2010095";
    failed.configuration = config(kW, kH);
    failed.failure_reason = "burst timeout after 2000 ms (9 of 17 frames)";
    bursts.push_back(std::move(failed));
    eb::CapturePlan plan;
    plan.schema_version = 1;
    plan.store_root = store;
    plan.capture_id = id;
    plan.captured_at_utc = at;
    plan.scene_declaration = scene();
    return eb::WriteCapture(plan, &bursts);
}

std::vector<eb::RecordingCamera> recording_cameras(std::initializer_list<const char*> serials) {
    std::vector<eb::RecordingCamera> out;
    for (const char* s : serials) out.push_back({s, config(kW, kH)});
    return out;
}

// The version 1 tests pin version 1, whatever Orange writes (kWriteSchemaVersion).
eb::BindingSelection v1_none() {
    eb::BindingSelection s;
    s.block_version = 1;
    return s;
}

eb::BindingSelection selected(const eb::CaptureReceipt& receipt) {
    eb::BindingSelection s;
    s.block_version = 1;
    s.selected = true;
    s.selection_source = "gui_operator";
    s.scene_unchanged_confirmed = true;
    s.descriptor_path = receipt.descriptor_path;
    return s;
}

bool block_valid(const json& block, std::string* error) {
    orange::outputs::gen::recording_empty_background_v1::OrangeRecordingEmptyBackgroundV1 parsed;
    return orange::outputs::gen::recording_empty_background_v1::parse(block, &parsed, error);
}

void write_snapshot_sample(const fs::path& samples, const std::string& name, const json& block) {
    if (samples.empty()) return;
    std::ofstream(samples / name) << json({{"session", {{"empty_background", block}}}}).dump(2) << "\n";
}

void test_median() {
    // Each pixel sees a permutation of 0..16: the median is 8 everywhere.
    std::vector<std::vector<std::uint8_t>> frames(17, std::vector<std::uint8_t>(17 * 3));
    std::mt19937 rng(7);
    for (std::size_t i = 0; i < frames[0].size(); ++i) {
        std::vector<std::uint8_t> values(17);
        for (int v = 0; v < 17; ++v) values[v] = static_cast<std::uint8_t>(v);
        std::shuffle(values.begin(), values.end(), rng);
        for (std::size_t f = 0; f < 17; ++f) frames[f][i] = values[f];
    }
    auto m = eb::MedianOfBurst(frames, frames[0].size());
    check(std::all_of(m.median.begin(), m.median.end(), [](std::uint8_t v) { return v == 8; }),
          "median of a permutation of 0..16 is 8 at every pixel");
    check(m.any_frame_saturated_pixels == 0, "no saturation counted without a 255");

    // 8 frames at 255, 9 at 10: the median is 10, the pixel counts as saturated in a frame.
    std::vector<std::vector<std::uint8_t>> clip(17, std::vector<std::uint8_t>(4, 10));
    for (std::size_t f = 0; f < 8; ++f) clip[f][0] = 255;
    for (std::size_t f = 0; f < 9; ++f) clip[f][1] = 255;  // 9 of 17 at 255: median 255
    clip[16][2] = 255;
    m = eb::MedianOfBurst(clip, 4);
    check(m.median[0] == 10 && m.median[1] == 255 && m.median[2] == 10 && m.median[3] == 10,
          "8 clipped frames leave the median at 10, 9 clipped make it 255");
    check(m.any_frame_saturated_pixels == 3, "any_frame_saturated counts pixels at 255 in any frame (3)");

    const auto noisy = noisy_frames(kW, kH, 3);
    check(eb::MedianOfBurst(noisy, kW * kH).median == brute_median(noisy), "median equals a full sort on noisy frames");

    check(!throws([] { eb::MedianOfBurst(std::vector<std::vector<std::uint8_t>>(16, std::vector<std::uint8_t>(4)), 4); }).empty(),
          "16 frames are refused");
    auto short_frame = std::vector<std::vector<std::uint8_t>>(17, std::vector<std::uint8_t>(4));
    short_frame[5].resize(3);
    check(!throws([&] { eb::MedianOfBurst(short_frame, 4); }).empty(), "a short frame is refused");
}

void test_statistics() {
    // 1000 pixels: 10 at 0, 980 at 100 + (i % 50), 10 at 255.
    std::vector<std::uint8_t> image;
    for (int i = 0; i < 10; ++i) image.push_back(0);
    for (int i = 0; i < 980; ++i) image.push_back(static_cast<std::uint8_t>(100 + i % 50));
    for (int i = 0; i < 10; ++i) image.push_back(255);
    const auto s = eb::ImageLevels(image.data(), image.size());
    const json j = orange::outputs::gen::image_levels_v1::to_json(s);
    // Ranks are ceil(k * 1000 / 1000) = k; pk() walks the histogram independently.
    std::uint64_t counts[256] = {};
    for (auto v : image) ++counts[v];
    const auto pk = [&](std::uint64_t rank) {
        std::uint64_t cumulative = 0;
        for (int v = 0; v < 256; ++v) {
            cumulative += counts[v];
            if (cumulative >= rank) return v;
        }
        return 255;
    };
    double sum = 0;
    for (auto v : image) sum += v;
    check(j.at("pixel_count") == 1000 && j.at("saturation_value") == 255, "pixel_count and saturation_value");
    check(j.at("zero_fraction") == 0.01 && j.at("saturated_fraction") == 0.01, "zero and saturated fractions (0.01)");
    check(j.at("mean") == std::round(sum / 1000.0 * 1000.0) / 1000.0, "mean rounded to 3 decimals");
    check(j.at("p01") == pk(10) && j.at("p05") == pk(50) && j.at("p50") == pk(500) && j.at("p95") == pk(950) &&
              j.at("p99") == pk(990) && j.at("p999") == pk(999),
          "percentiles are nearest rank ceil(k n / 1000) on the histogram");
    check(j.at("p01") == 0 && j.at("p999") == 255, "p01 = 0 (rank 10 is the last zero) and p999 = 255");
    // Rounding: 1 of 3 pixels saturated -> 0.33333333.
    const std::uint8_t three[3] = {255, 1, 2};
    const json r = orange::outputs::gen::image_levels_v1::to_json(eb::ImageLevels(three, 3));
    check(r.at("saturated_fraction") == 0.33333333 && r.at("mean") == 86.0, "fractions round to 8 decimals, mean to 3");
}

void test_png() {
    const auto frames = noisy_frames(kW, kH, 11);
    const std::string png = eb::EncodePngGray8(frames[0].data(), kW, kH);
    std::vector<std::uint8_t> decoded;
    std::uint32_t w = 0, h = 0;
    std::string error;
    check(eb::DecodePngGray8(png, &decoded, &w, &h, &error) && w == kW && h == kH && decoded == frames[0],
          "PNG round trip is exact " + error);
    check(eb::ReadPngGray8Header(png, &w, &h, &error), "header reads");
    // An sRGB chunk after IHDR (33 bytes in) is refused.
    std::string with_srgb = png.substr(0, 33);
    const std::string srgb = std::string("\0\0\0\1sRGB\0", 9);
    with_srgb += srgb + std::string(4, '\0') + png.substr(33);
    check(!eb::ReadPngGray8Header(with_srgb, &w, &h, &error), "a PNG with an sRGB chunk is refused: " + error);
    std::string corrupt = png;
    corrupt[40] ^= 0x55;
    check(!eb::DecodePngGray8(corrupt, &decoded, &w, &h, &error), "a corrupted PNG is refused: " + error);
}

void test_capture(const fs::path& samples) {
    const fs::path store = temp_dir("store");
    std::vector<std::vector<std::uint8_t>> medians;
    const auto receipt = make_capture(store, "2026_11_02_15_04_11", "2026-11-02T23:04:11.123Z", &medians);
    const fs::path dir = store / "2026_11_02_15_04_11";
    check(fs::exists(dir / "background.json") && fs::exists(dir / "Cam2010093_empty_background.png") &&
              fs::exists(dir / "Cam2010094_empty_background.png") && !fs::exists(dir / "Cam2010095_empty_background.png"),
          "store layout: background.json and the two captured images, none for the failed camera");
    std::size_t entries = 0;
    for (const auto& e : fs::directory_iterator(dir)) entries += 1 + 0 * e.path().empty();
    check(entries == 3, "no temporary or extra files in the capture directory");
    const std::string bytes = slurp(dir / "background.json");
    check(receipt.descriptor_sha256 == orange::outputs::sha256_token(bytes) && receipt.descriptor_size_bytes == bytes.size(),
          "receipt digest and size are the descriptor's bytes");
    const json doc = json::parse(bytes);
    check(doc.at("schema_id") == "orange.empty_background_capture" && doc.at("schema_version") == 1 &&
              doc.at("method") == json({{"name", "median_17"}, {"version", 1}, {"frame_count", 17}}) &&
              doc.at("pixel_declaration") ==
                  json({{"encoding", "raw_sensor_mono8"}, {"range", "full"}, {"conversion", "none"}}),
          "descriptor identity, method and pixel declaration");
    const auto& cams = doc.at("cameras");
    check(cams.size() == 3 && cams[2].at("capture_status") == "failed" &&
              cams[2].at("failure_reason") == "burst timeout after 2000 ms (9 of 17 frames)" && !cams[2].contains("image"),
          "the failed camera is listed with its reason and no image");
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& cam = cams[i];
        const fs::path png = dir / cam.at("image").at("relative_path").get<std::string>();
        const std::string png_bytes = slurp(png);
        std::vector<std::uint8_t> decoded;
        std::uint32_t w = 0, h = 0;
        std::string error;
        check(cam.at("image").at("sha256") == orange::outputs::sha256_token(png_bytes) &&
                  cam.at("image").at("size_bytes") == png_bytes.size(),
              cam.at("camera_serial").get<std::string>() + " image sha256 and size match the file");
        check(eb::DecodePngGray8(png_bytes, &decoded, &w, &h, &error) && decoded == medians[i],
              cam.at("camera_serial").get<std::string>() + " PNG holds exactly the 17-frame median");
        check(cam.at("statistics") ==
                  orange::outputs::gen::image_levels_v1::to_json(eb::ImageLevels(medians[i].data(), medians[i].size())),
              cam.at("camera_serial").get<std::string>() + " statistics are the median's image levels");
        check(cam.at("source_frames").size() == 17 && cam.at("source_frames")[5].at("camera_frame_id") == 65535 &&
                  cam.at("source_frames")[6].at("camera_frame_id") == 1,
              "17 source frames in stream order, camera_frame_id wrapping 65535 -> 1");
        check(cam.contains("any_frame_saturated_fraction"), "any_frame_saturated_fraction written");
    }
    if (!samples.empty()) fs::copy_file(dir / "background.json", samples / "background.json");

    // The directory is never adopted.
    check(throws([&] { make_capture(store, "2026_11_02_15_04_11", "2026-11-02T23:04:11.123Z"); }).find("cannot create") !=
              std::string::npos,
          "an existing capture directory is refused");

    // A burst with a dropped frame is written as failed; the other camera keeps its image.
    {
        std::vector<eb::CameraBurst> bursts(2);
        bursts[0].camera_serial = "2010093";
        bursts[0].configuration = config(kW, kH);
        bursts[0].source_frames = identities(1, 1);
        bursts[0].frames = noisy_frames(kW, kH, 1);
        bursts[1] = bursts[0];
        bursts[1].camera_serial = "2010094";
        bursts[1].source_frames[9].stream_frame_id += 1;
        bursts[1].source_frames[9].camera_frame_id += 1;
        eb::CapturePlan plan;
        plan.schema_version = 1;
    plan.schema_version = 1;
        plan.store_root = store;
        plan.capture_id = "gap";
        plan.captured_at_utc = "2026-11-02T23:05:00.000Z";
        plan.scene_declaration = scene();
        const auto r = eb::WriteCapture(plan, &bursts);
        check(r.descriptor.cameras[0].capture_status == "captured" && r.descriptor.cameras[1].capture_status == "failed" &&
                  r.descriptor.cameras[1].failure_reason->find("not consecutive") != std::string::npos,
              "a non-consecutive burst fails that camera only");
        check(bursts[0].frames.empty(), "source frames are released after the median");
    }
    // Malformed plans write nothing.
    {
        std::vector<eb::CameraBurst> bursts(1);
        bursts[0].camera_serial = "Cam93";
        bursts[0].configuration = config(kW, kH);
        eb::CapturePlan plan;
        plan.schema_version = 1;
    plan.schema_version = 1;
        plan.store_root = store;
        plan.capture_id = "bad_serial";
        plan.captured_at_utc = "2026-11-02T23:05:00.000Z";
        plan.scene_declaration = scene();
        check(!throws([&] { eb::WriteCapture(plan, &bursts); }).empty() && !fs::exists(store / "bad_serial"),
              "a non-numeric serial is refused before anything is written");
        bursts[0].camera_serial = "2010093";
        plan.capture_id = "bad scene";
        check(!throws([&] { eb::WriteCapture(plan, &bursts); }).empty(), "an unsafe capture_id is refused");
        plan.capture_id = "bad_scene";
        plan.scene_declaration.stimulus_display.state = "off";  // off with shown: schema refuses
        bursts[0].failure_reason = "x";
        check(throws([&] { eb::WriteCapture(plan, &bursts); }).find("schema") != std::string::npos &&
                  !fs::exists(store / "bad_scene" / "background.json"),
              "a scene declaration the schema refuses leaves no background.json");
    }
    // Newest capture.
    make_capture(store, "later", "2026-11-03T08:00:00.000Z");
    const auto newest = eb::NewestCapture(store);
    check(newest && newest->descriptor.capture_id == "later", "NewestCapture picks the newest captured_at_utc");
    check(!eb::NewestCapture(store / "absent"), "no store: no capture");
    fs::remove_all(store);
}

void test_binding(const fs::path& samples) {
    const fs::path store = temp_dir("bind_store");
    const auto receipt = make_capture(store, "cap", "2026-11-02T23:04:11.123Z");
    const fs::path source = store / "cap";
    const auto new_recording = [](const std::string& tag) { return temp_dir("rec_" + tag); };
    std::string error;

    // Never selected: every camera not_selected, nothing written.
    {
        const fs::path rec = new_recording("none");
        const auto r = eb::BindToRecording(rec, v1_none(), recording_cameras({"2010093", "2010094"}));
        check(r.block == json({{"capture_selected", false},
                               {"cameras", {{"2010093", {{"status", "not_selected"}}}, {"2010094", {{"status", "not_selected"}}}}}}),
              "never selected: capture_selected false, every camera not_selected");
        check(block_valid(r.block, &error) && r.warnings.empty(), "never-selected block validates " + error);
        check(!fs::exists(rec / "empty_background"), "never selected: no empty_background directory");
        write_snapshot_sample(samples, "recording_snapshot_none.json", r.block);
        json written;
        const auto w = eb::WriteRecordingBinding(rec.string(), v1_none(), recording_cameras({"2010093"}),
                                                 [&](const std::string&, const json& session) {
                                                     written = session;
                                                     return true;
                                                 });
        check(written == json({{"empty_background", w.block}}), "WriteRecordingBinding writes session.empty_background");
        check(!throws([&] {
                  eb::WriteRecordingBinding(rec.string(), v1_none(), recording_cameras({"2010093"}),
                                            [](const std::string&, const json&) { return false; });
              }).empty(),
              "a failed snapshot update throws");
        check(!throws([&] { eb::BindToRecording(rec, v1_none(), {}); }).empty(), "a recording without cameras is refused");
        fs::remove_all(rec);
    }
    // Attached: exact copies, descriptor digest.
    {
        const fs::path rec = new_recording("attached");
        const auto r = eb::BindToRecording(rec, selected(receipt), recording_cameras({"2010093", "2010094"}));
        check(block_valid(r.block, &error) && r.warnings.empty(), "attached block validates " + error);
        check(r.block.at("cameras").at("2010093").at("status") == "attached" &&
                  r.block.at("cameras").at("2010094").at("status") == "attached",
              "matching settings: attached");
        check(slurp(rec / "empty_background" / "background.json") == slurp(source / "background.json"),
              "descriptor copied byte for byte");
        check(r.block.at("descriptor") == json({{"relative_path", "empty_background/background.json"},
                                                {"size_bytes", receipt.descriptor_size_bytes},
                                                {"sha256", receipt.descriptor_sha256}}),
              "block carries the descriptor's path, size and sha256");
        check(r.block.at("capture_id") == "cap" && r.block.at("selection_source") == "gui_operator" &&
                  r.block.at("scene_unchanged_confirmed") == true &&
                  r.block.at("source_descriptor_path") == receipt.descriptor_path.string(),
              "block carries capture id, source, confirmation and provenance path");
        for (const char* serial : {"2010093", "2010094"}) {
            const std::string name = std::string("Cam") + serial + "_empty_background.png";
            check(slurp(rec / "empty_background" / name) == slurp(source / name), std::string(serial) + " image copied byte for byte");
            check(r.block.at("cameras").at(serial).at("image").at("sha256") ==
                      orange::outputs::sha256_token(slurp(rec / "empty_background" / name)),
                  std::string(serial) + " image sha256 verified");
        }
        write_snapshot_sample(samples, "recording_snapshot_attached.json", r.block);
        check(throws([&] { eb::BindToRecording(rec, selected(receipt), recording_cameras({"2010093"})); }).find("cannot create") !=
                  std::string::npos,
              "an existing empty_background directory is never adopted");
        fs::remove_all(rec);
    }
    // Drift per checked setting: copied and flagged, one difference each.
    const std::vector<std::pair<std::string, std::uint64_t eb::CameraConfiguration::*>> settings = {
        {"exposure_us", &eb::CameraConfiguration::exposure_us}, {"gain", &eb::CameraConfiguration::gain},
        {"iris", &eb::CameraConfiguration::iris},               {"focus", &eb::CameraConfiguration::focus},
        {"width", &eb::CameraConfiguration::width},             {"height", &eb::CameraConfiguration::height},
        {"offset_x", &eb::CameraConfiguration::offset_x},       {"offset_y", &eb::CameraConfiguration::offset_y}};
    for (const auto& [name, member] : settings) {
        const fs::path rec = new_recording("drift_" + name);
        auto cams = recording_cameras({"2010093", "2010094"});
        const std::uint64_t was = cams[0].configuration.*member;
        cams[0].configuration.*member = was + 7;
        const auto r = eb::BindToRecording(rec, selected(receipt), cams);
        const auto& cam = r.block.at("cameras").at("2010093");
        check(block_valid(r.block, &error) && cam.at("status") == "settings_changed" &&
                  cam.at("settings_differences") ==
                      json::array({{{"setting", name}, {"capture_value", was}, {"recording_value", was + 7}}}) &&
                  cam.contains("image") && fs::exists(rec / "empty_background" / "Cam2010093_empty_background.png") &&
                  r.block.at("cameras").at("2010094").at("status") == "attached" && r.warnings.size() == 1,
              "drift in " + name + ": image copied, settings_changed names it with both values");
        if (name == "iris") write_snapshot_sample(samples, "recording_snapshot_settings_changed.json", r.block);
        fs::remove_all(rec);
    }
    {
        const fs::path rec = new_recording("drift_all");
        auto cams = recording_cameras({"2010093"});
        for (const auto& [name, member] : settings) cams[0].configuration.*member += 1;
        cams[0].configuration.frame_rate_hz = 100;
        const auto r = eb::BindToRecording(rec, selected(receipt), cams);
        const auto& diffs = r.block.at("cameras").at("2010093").at("settings_differences");
        bool ordered = diffs.size() == settings.size();
        for (std::size_t i = 0; ordered && i < settings.size(); ++i) ordered = diffs[i].at("setting") == settings[i].first;
        check(block_valid(r.block, &error) && ordered, "all eight checked settings listed, in schema order; frame rate not among them");
        fs::remove_all(rec);
    }
    {
        const fs::path rec = new_recording("frame_rate");
        auto cams = recording_cameras({"2010093"});
        cams[0].configuration.frame_rate_hz = 100;
        const auto r = eb::BindToRecording(rec, selected(receipt), cams);
        check(r.block.at("cameras").at("2010093").at("status") == "attached", "a frame-rate change is not checked: attached");
        fs::remove_all(rec);
    }
    // Missing camera: absent from the capture, and failed in the capture.
    {
        const fs::path rec = new_recording("missing");
        const auto r = eb::BindToRecording(rec, selected(receipt), recording_cameras({"2010093", "2010095", "2010096"}));
        const auto& c = r.block.at("cameras");
        check(block_valid(r.block, &error) && c.at("2010096") == json({{"status", "missing_in_capture"}}),
              "a camera absent from the capture is missing_in_capture without a reason");
        check(c.at("2010095") == json({{"status", "missing_in_capture"},
                                       {"failure_reason", "burst timeout after 2000 ms (9 of 17 frames)"}}),
              "a camera failed in the capture is missing_in_capture with the capture's reason copied");
        check(!fs::exists(rec / "empty_background" / "Cam2010095_empty_background.png") && r.warnings.size() == 2,
              "no image for missing cameras; one warning each");
        write_snapshot_sample(samples, "recording_snapshot_missing.json", r.block);
        fs::remove_all(rec);
    }
    // Copy failures: a tampered source image, a deleted one; no image is left behind.
    {
        const std::string png = source / "Cam2010094_empty_background.png";
        const std::string original = slurp(png);
        std::string tampered = original;
        tampered[tampered.size() / 2] ^= 0x01;
        std::ofstream(png, std::ios::binary | std::ios::trunc) << tampered;
        const fs::path rec = new_recording("tampered");
        auto r = eb::BindToRecording(rec, selected(receipt), recording_cameras({"2010093", "2010094"}));
        const auto& c = r.block.at("cameras");
        check(block_valid(r.block, &error) && c.at("2010094").at("status") == "copy_failed" &&
                  c.at("2010094").at("failure_reason").get<std::string>().find("sha256") != std::string::npos &&
                  !fs::exists(rec / "empty_background" / "Cam2010094_empty_background.png") &&
                  c.at("2010093").at("status") == "attached",
              "a tampered source image is copy_failed (digest), no copy is left, the other camera attaches");
        write_snapshot_sample(samples, "recording_snapshot_copy_failed.json", r.block);
        fs::remove_all(rec);
        fs::remove(png);
        const fs::path rec2 = new_recording("deleted");
        r = eb::BindToRecording(rec2, selected(receipt), recording_cameras({"2010094"}));
        check(r.block.at("cameras").at("2010094").at("status") == "copy_failed" &&
                  r.block.at("cameras").at("2010094").at("failure_reason").get<std::string>().find("cannot read") !=
                      std::string::npos,
              "a deleted source image is copy_failed (cannot read)");
        fs::remove_all(rec2);
        std::ofstream(png, std::ios::binary | std::ios::trunc) << original;
        auto cams = recording_cameras({"2010094"});
        cams[0].configuration.pixel_format = "BayerRG8";
        const fs::path rec3 = new_recording("bayer");
        r = eb::BindToRecording(rec3, selected(receipt), cams);
        check(r.block.at("cameras").at("2010094").at("status") == "copy_failed", "a non-Mono8 recording camera is not attached");
        fs::remove_all(rec3);
    }
    // Selection-level refusals write nothing.
    {
        const fs::path rec = new_recording("refusals");
        auto s = selected(receipt);
        s.scene_unchanged_confirmed = false;
        check(!throws([&] { eb::BindToRecording(rec, s, recording_cameras({"2010093"})); }).empty() &&
                  !fs::exists(rec / "empty_background"),
              "a selected capture without the confirmation is refused, nothing written");
        s = selected(receipt);
        s.selection_source = "elsewhere";
        check(!throws([&] { eb::BindToRecording(rec, s, recording_cameras({"2010093"})); }).empty(), "an unknown selection source is refused");
        s = selected(receipt);
        s.descriptor_path = store / "absent" / "background.json";
        check(!throws([&] { eb::BindToRecording(rec, s, recording_cameras({"2010093"})); }).empty() &&
                  !fs::exists(rec / "empty_background"),
              "an unreadable descriptor is refused, nothing written");
        const std::string original = slurp(source / "background.json");
        std::ofstream(source / "background.json", std::ios::trunc) << json::parse(original).dump();  // same JSON, other bytes
        s = selected(receipt);
        const auto r = eb::BindToRecording(rec, s, recording_cameras({"2010093"}));
        check(r.block.at("descriptor").at("sha256") == orange::outputs::sha256_token(slurp(source / "background.json")) &&
                  slurp(rec / "empty_background" / "background.json") == slurp(source / "background.json"),
              "the copy is of the bytes on disk, and the block names their digest");
        fs::remove_all(rec / "empty_background");
        json broken = json::parse(original);
        broken["cameras"][1]["camera_serial"] = "2010093";  // duplicate serial
        std::ofstream(source / "background.json", std::ios::trunc) << broken.dump(2);
        check(throws([&] { eb::BindToRecording(rec, s, recording_cameras({"2010093"})); }).find("duplicate") != std::string::npos,
              "a descriptor with duplicate serials is refused");
        std::ofstream(source / "background.json", std::ios::trunc) << original;
        fs::permissions(rec, fs::perms::owner_read | fs::perms::owner_exec);
        const bool as_root = ::geteuid() == 0;
        check(as_root || (!throws([&] { eb::BindToRecording(rec, s, recording_cameras({"2010093"})); }).empty() &&
                          !fs::exists(rec / "empty_background")),
              "a recording folder that cannot take the copy is refused, nothing written");
        fs::permissions(rec, fs::perms::owner_all);
        fs::remove_all(rec);
    }
    // Headless spec key: the absolute path of background.json; naming it is the confirmation.
    {
        auto s = eb::SelectionFromHeadlessSpec(receipt.descriptor_path.string());
        s.block_version = 1;
        check(s.selected && s.selection_source == "headless_spec" && s.scene_unchanged_confirmed &&
                  s.descriptor_path == receipt.descriptor_path,
              "fixed.empty_background_capture selects the capture as headless_spec, confirmed");
        const fs::path rec = new_recording("headless");
        const auto r = eb::BindToRecording(rec, s, recording_cameras({"2010093"}));
        check(r.block.at("selection_source") == "headless_spec" && r.block.at("cameras").at("2010093").at("status") == "attached",
              "a headless selection binds like a GUI one");
        fs::remove_all(rec);
        check(!throws([] { eb::SelectionFromHeadlessSpec(nullptr); }).empty(), "a null spec value is refused");
        check(!throws([] { eb::SelectionFromHeadlessSpec(json::object()); }).empty(), "an object spec value is refused");
        check(!throws([] { eb::SelectionFromHeadlessSpec("backgrounds/cap/background.json"); }).empty(),
              "a relative path is refused");
        check(!throws([&] { eb::SelectionFromHeadlessSpec((source / ".." / "cap" / "background.json").string()); }).empty(),
              "a non-normal path is refused");
        check(!throws([&] { eb::SelectionFromHeadlessSpec(source.string()); }).empty(), "a directory instead of background.json is refused");
        check(!throws([&] { eb::SelectionFromHeadlessSpec((store / "absent" / "background.json").string()); }).empty(),
              "a missing capture is refused at spec load");
    }
    fs::remove_all(store);
}

// ---- version 2 (agent-contracts orange-palette/empty_background_v2) -------------------------

const json kGuiRequester = {{"transport", "gui_panel"}, {"user", "jeremy"}};
const json kLocalRequester = {{"transport", "local_control_socket"},
                              {"peer", {{"uid", 1000}, {"gid", 1000}, {"pid", 4242}, {"user", "jeremy"}}},
                              {"request_id", "req-1"}, {"operation_id", "op-1"}, {"command_source", "specsail"},
                              {"received_at_utc", "2026-11-15T18:02:11.204Z"}};

eb::CaptureReceipt make_capture_v2(const fs::path& store, const std::string& id, const json& requester) {
    std::vector<eb::CameraBurst> bursts;
    eb::CameraBurst b;
    b.camera_serial = "2010093";
    b.configuration = config(kW, kH);
    b.source_frames = identities(1000, 65530);
    b.frames = noisy_frames(kW, kH, 3);
    bursts.push_back(std::move(b));
    eb::CapturePlan plan;
    plan.schema_version = 2;
    plan.store_root = store;
    plan.capture_id = id;
    plan.captured_at_utc = "2026-11-15T18:00:00.000Z";
    plan.scene_declaration = scene();
    plan.requester = requester;
    plan.declared_by = "jeremy";
    return eb::WriteCapture(plan, &bursts);
}

bool block2_valid(const json& block, std::string* error) {
    orange::outputs::gen::recording_empty_background_v2::OrangeRecordingEmptyBackgroundV2 parsed;
    return orange::outputs::gen::recording_empty_background_v2::parse(block, &parsed, error);
}

void test_version2() {
    const fs::path store = temp_dir("v2_store");
    const fs::path recordings = temp_dir("v2_recordings");
    std::string error;

    const auto gui = make_capture_v2(store, "gui", kGuiRequester);
    const json gui_doc = json::parse(slurp(gui.descriptor_path));
    orange::outputs::gen::empty_background_capture_v2::OrangeEmptyBackgroundCaptureV2 parsed_v2;
    check(gui.schema_version == 2 && gui_doc.at("schema_version") == 2 &&
              orange::outputs::gen::empty_background_capture_v2::parse(gui_doc, &parsed_v2, &error),
          "v2 capture (gui_panel) is written and parses under v2 " + error);
    check(gui_doc.at("requester") == kGuiRequester && gui_doc.at("scene_declaration").at("declared_by") == "jeremy" &&
              gui_doc.at("scene_declaration").at("stimulus_display").at("source") == "operator_declared",
          "v2 gui_panel capture: requester, declared_by, operator_declared");
    const auto local = make_capture_v2(store, "local", kLocalRequester);
    const json local_doc = json::parse(slurp(local.descriptor_path));
    check(local_doc.at("scene_declaration").at("stimulus_display").at("source") == "request_declared" &&
              local_doc.at("requester").at("peer").at("uid") == 1000,
          "v2 local_control_socket capture: request_declared and the SO_PEERCRED peer");
    const eb::StoredCapture stored = eb::ReadCapture(local.descriptor_path);
    check(stored.schema_version == 2 && stored.requester == kLocalRequester && stored.declared_by == "jeremy" &&
              stored.descriptor.capture_id == "local" && stored.descriptor.cameras.size() == 1,
          "ReadCapture reads a v2 descriptor (version, requester, declared_by, shared fields)");
    check(throws([&] { make_capture_v2(store, "spec", {{"transport", "run_spec"}, {"spec_path", "/x.json"},
                                                       {"spec_sha256", std::string(64, 'a')}, {"user", nullptr}}); })
                  .find("gui_panel or local_control_socket") != std::string::npos &&
              !fs::exists(store / "spec"),
          "a run_spec capture requester is refused before any write");
    check(!throws([&] { make_capture_v2(store, "nopeer", json{{"transport", "local_control_socket"}, {"peer", nullptr}}); })
               .empty() && !fs::exists(store / "nopeer"),
          "a local requester without a peer is refused before any write");
    {
        std::vector<eb::CameraBurst> bursts(1);
        bursts[0].camera_serial = "2010093";
        bursts[0].configuration = config(kW, kH);
        bursts[0].source_frames = identities(1, 1);
        bursts[0].frames = noisy_frames(kW, kH, 4);
        eb::CapturePlan plan;
        plan.schema_version = 2;
        plan.store_root = store;
        plan.capture_id = "baddecl";
        plan.captured_at_utc = "2026-11-15T18:00:00.000Z";
        plan.scene_declaration = scene();
        plan.requester = kGuiRequester;
        plan.declared_by = "Jeremy D";
        check(throws([&] { eb::WriteCapture(plan, &bursts); }).find("declared_by") != std::string::npos,
              "a declared_by that is not an identity is refused");
        plan.schema_version = 1;
        plan.declared_by = "jeremy";
        plan.capture_id = "v1requester";
        check(!throws([&] { eb::WriteCapture(plan, &bursts); }).empty(), "a v1 capture with a requester is refused");
    }

    // The block: v2 no_capture, v1 block refuses a v2 descriptor, v2 block attaches v1 and v2.
    const auto cams = recording_cameras({"2010093"});
    eb::BindingSelection none;
    none.block_version = 2;
    const auto empty = eb::BindToRecording(recordings / "r0", none, cams);
    check(empty.block.at("schema_version") == 2 && empty.block.at("capture_selected") == false &&
              block2_valid(empty.block, &error),
          "v2 no_capture carries schema_version 2 " + error);
    fs::create_directories(recordings / "r1");
    eb::BindingSelection v1_with_v2 = selected(gui);
    v1_with_v2.block_version = 1;
    check(throws([&] { eb::BindToRecording(recordings / "r1", v1_with_v2, cams); }).find("version 1 block cannot attach") !=
                  std::string::npos && !fs::exists(recordings / "r1" / "empty_background"),
          "a v1 block never attaches a v2 descriptor (nothing written)");
    const auto v1_capture = make_capture(store, "old", "2026-11-02T23:04:11.123Z");
    fs::create_directories(recordings / "r2");
    eb::BindingSelection gui_v2 = selected(v1_capture);
    gui_v2.block_version = 2;
    gui_v2.requester = kGuiRequester;
    const auto attached_v1 = eb::BindToRecording(recordings / "r2", gui_v2, cams);
    check(attached_v1.block.at("schema_version") == 2 && attached_v1.block.at("requester") == kGuiRequester &&
              block2_valid(attached_v1.block, &error),
          "a v2 block attaches a v1 descriptor (gui_operator, gui_panel) " + error);
    fs::create_directories(recordings / "r3");
    eb::BindingSelection mismatch = gui_v2;
    mismatch.requester = kLocalRequester;
    check(throws([&] { eb::BindToRecording(recordings / "r3", mismatch, cams); }).find("does not match") != std::string::npos,
          "gui_operator with a local_control_socket requester is refused");

    // Local control: the request names the capture by id and sha256.
    const json good = {{"capture_id", "local"}, {"descriptor_sha256", local.descriptor_sha256},
                       {"scene_unchanged_confirmed", true}};
    const eb::BindingSelection remote = eb::SelectionFromLocalControl(good, kLocalRequester, store, 2);
    check(remote.selection_source == "local_control" && remote.descriptor_path == store / "local" / "background.json",
          "SelectionFromLocalControl resolves the named capture in the store");
    fs::create_directories(recordings / "r4");
    const auto bound = eb::BindToRecording(recordings / "r4", remote, cams);
    check(bound.block.at("selection_source") == "local_control" && bound.block.at("requester") == kLocalRequester &&
              bound.block.at("descriptor").at("sha256") == local.descriptor_sha256 &&
              bound.block.at("cameras").at("2010093").at("status") == "attached" && block2_valid(bound.block, &error),
          "a local_control start attaches exactly the requested capture " + error);
    check(slurp(recordings / "r4" / "empty_background" / "background.json") == slurp(local.descriptor_path),
          "the recording copy of the v2 descriptor is byte-exact");
    auto refused = [&](json params, const std::string& expect, const std::string& what, int version = 2,
                       json requester = kLocalRequester) {
        const std::string why = throws([&] { eb::SelectionFromLocalControl(params, requester, store, version); });
        check(why.find(expect) != std::string::npos, what + " (" + why + ")");
    };
    json p = good;
    p["descriptor_sha256"] = "sha256:" + std::string(64, '0');
    refused(p, "not the requested", "a different descriptor sha256 refuses the start");
    p = good;
    p["capture_id"] = "gui_missing";
    refused(p, "", "an unknown capture_id refuses the start");
    p = good;
    p["scene_unchanged_confirmed"] = false;
    refused(p, "scene_unchanged_confirmed must be true", "an unconfirmed scene refuses the start");
    p = good;
    p["newest"] = true;
    refused(p, "unknown field", "an unknown field refuses the start");
    p = good;
    p["capture_id"] = "../gui";
    refused(p, "capture_id must match", "a capture_id that is a path refuses the start");
    refused(good, "needs start-snapshot block version 2", "local control selection needs block version 2", 1);
    refused(good, "local_control_socket requester", "local control selection needs a socket requester", 2, kGuiRequester);
    fs::create_directories(recordings / "r5");
    eb::BindingSelection tampered = remote;
    tampered.expected_descriptor_sha256 = "sha256:" + std::string(64, 'f');
    check(throws([&] { eb::BindToRecording(recordings / "r5", tampered, cams); }).find("not the requested") !=
                  std::string::npos && !fs::exists(recordings / "r5" / "empty_background"),
          "the binding re-checks the requested sha256 at the copy (nothing written)");

    // A stored descriptor keeps its bytes: one written before the rounding fix
    // (src/outputs/common/decimal_round.h) carries e.g. "mean": 180.96200000000002.
    // It still reads, attaches and is copied byte for byte; its sha256 is over the
    // stored bytes, never a re-dump (Palette 2026-11-15).
    {
        const auto old_capture = make_capture_v2(store, "ulp", kGuiRequester);
        std::string bytes = slurp(old_capture.descriptor_path);
        const std::size_t at = bytes.find("\"mean\": ");
        const std::size_t end = bytes.find_first_of(",\n", at);
        check(at != std::string::npos && end != std::string::npos, "the descriptor has a mean to rewrite");
        bytes.replace(at, end - at, "\"mean\": 180.96200000000002");
        {
            std::ofstream out(old_capture.descriptor_path, std::ios::binary | std::ios::trunc);
            out << bytes;
        }
        const std::string stored_sha = orange::outputs::sha256_token(bytes);
        const eb::StoredCapture read = eb::ReadCapture(old_capture.descriptor_path);
        check(read.descriptor_bytes == bytes && read.descriptor_sha256 == stored_sha,
              "a stored descriptor with a 1-ulp mean reads, and its sha256 is over the stored bytes");
        fs::create_directories(recordings / "r6");
        const json old_request = {{"capture_id", "ulp"}, {"descriptor_sha256", stored_sha},
                                  {"scene_unchanged_confirmed", true}};
        const auto bound_old = eb::BindToRecording(
            recordings / "r6", eb::SelectionFromLocalControl(old_request, kLocalRequester, store, 2), cams);
        check(slurp(recordings / "r6" / "empty_background" / "background.json") == bytes &&
                  bound_old.block.at("descriptor").at("sha256") == stored_sha &&
                  bytes.find("180.96200000000002") != std::string::npos,
              "it attaches and is copied byte for byte (never re-serialized)");
    }

    // capture_empty_background params.
    const json capture_params = {{"declared_by", "jeremy"}, {"dishes_empty", true}, {"note", "water, no fish"},
                                 {"stimulus_display", {{"state", "on"}, {"shown", {{"kind", "canvas"},
                                                                                   {"canvas_name", "static_dots"}}}}}};
    const auto decl = eb::CaptureDeclarationFromLocalControl(capture_params);
    check(decl.declared_by == "jeremy" && decl.scene_declaration.stimulus_display.state == "on" &&
              decl.scene_declaration.stimulus_display.shown && decl.scene_declaration.note,
          "capture_empty_background params parse");
    auto bad_params = [&](json params, const std::string& what) {
        check(!throws([&] { eb::CaptureDeclarationFromLocalControl(params); }).empty(), what);
    };
    json q = capture_params;
    q["dishes_empty"] = false;
    bad_params(q, "dishes_empty false is refused");
    q = capture_params;
    q["stimulus_display"]["source"] = "operator_declared";
    bad_params(q, "a source in the request is refused (Orange sets request_declared)");
    q = capture_params;
    q.erase("declared_by");
    bad_params(q, "a missing declared_by is refused");
    q = capture_params;
    q["stimulus_display"] = {{"state", "off"}, {"shown", {{"kind", "other"}, {"note", "x"}}}};
    bad_params(q, "shown with state off is refused");
    q = capture_params;
    q["capture_id"] = "mine";
    bad_params(q, "an unknown field is refused");
    fs::remove_all(store);
    fs::remove_all(recordings);
}
}  // namespace

int main(int argc, char** argv) {
    const fs::path samples = argc > 1 ? fs::path(argv[1]) : fs::path();
    if (!samples.empty()) fs::create_directories(samples);
    test_median();
    test_statistics();
    test_png();
    test_capture(samples);
    test_binding(samples);
    test_version2();
    std::cout << (failures == 0 ? "empty_background_tests: PASS\n" : "empty_background_tests: FAIL\n");
    return failures == 0 ? 0 : 1;
}
