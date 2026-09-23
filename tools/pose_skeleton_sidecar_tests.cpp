// tools/pose_skeleton_sidecar_tests.cpp — Palette skeleton sidecar reader.
#include "pose_skeleton_sidecar.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using namespace orange::pose;

void require(bool c, const std::string& m) { if (!c) { std::cerr << "FAIL: " << m << std::endl; std::exit(1); } }

const char* kGood = R"({"schema_id":"palette.pose_model_skeleton","schema_version":1,"skeleton_id":"pose_schema:traditional_v1",
 "nodes":[{"id":1,"name":"eye_left"},{"id":0,"name":"swim_bladder"},{"id":2,"name":"eye_right"}],
 "edges":[[0,1],[0,2],[1,2]],"kpt_shape":[3,3],"source":{"run_id":"run_x","onnx_sha256":"abc"}})";

void test_good()
{
    PoseSkeletonSidecar s; std::string err;
    require(parse_pose_skeleton_sidecar(kGood, "inline", &s, &err), "good sidecar parses: " + err);
    require(s.labels.size() == 3 && s.labels[0] == "swim_bladder" && s.labels[1] == "eye_left" && s.labels[2] == "eye_right",
            "labels ordered by node id, not by document order");
    require(s.edges.size() == 3 && s.edges[2] == std::make_pair(1, 2), "edges kept in order");
    require(s.kpt_k == 3 && s.kpt_d == 3, "kpt_shape read");
    require(s.skeleton_id == "pose_schema:traditional_v1" && s.source_run_id == "run_x", "identity fields");
    require(s.file_sha256.size() == 64 && s.file_sha256_prefix64 != 0, "content hash computed");
    require(sha256_prefix64(s.file_sha256) == s.file_sha256_prefix64, "prefix64 consistent");
}

void test_rejections()
{
    struct Case { const char* name; std::string json; };
    const std::string base(kGood);
    auto with = [&](const std::string& from, const std::string& to) { std::string j = base; j.replace(j.find(from), from.size(), to); return j; };
    const Case cases[] = {
        {"wrong schema", with("palette.pose_model_skeleton", "other.schema")},
        {"wrong version", with("\"schema_version\":1", "\"schema_version\":2")},
        {"missing skeleton id", with("\"skeleton_id\":\"pose_schema:traditional_v1\"", "\"skeleton_id\":\"\"")},
        {"duplicate node id", with("{\"id\":2,\"name\":\"eye_right\"}", "{\"id\":1,\"name\":\"eye_right\"}")},
        {"edge out of range", with("[1,2]]", "[1,3]]")},
        {"self edge", with("[1,2]]", "[2,2]]")},
        {"K mismatch", with("\"kpt_shape\":[3,3]", "\"kpt_shape\":[4,3]")},
        {"coordinate-only shape", with("\"kpt_shape\":[3,3]", "\"kpt_shape\":[3,2]")},
        {"invalid json", "{"},
    };
    for (const auto& c : cases) {
        PoseSkeletonSidecar s; std::string err;
        require(!parse_pose_skeleton_sidecar(c.json, c.name, &s, &err), std::string("rejects ") + c.name);
        require(!err.empty(), std::string("error message for ") + c.name);
    }
    PoseSkeletonSidecar s; std::string err;
    require(!load_pose_skeleton_sidecar("/nonexistent/sidecar.json", &s, &err), "missing file rejected");
}

void test_real_deployment_if_present()
{
    const std::string path = "/home/jeremy/orange_data/pose/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915/"
                             "palette_deployment_20260923_v2/pose_model_skeleton.json";
    std::ifstream probe(path);
    if (!probe) {
        std::cout << "  (real deployment sidecar not present; skipped)" << std::endl;
        return;
    }
    PoseSkeletonSidecar s; std::string err;
    require(load_pose_skeleton_sidecar(path, &s, &err), "real sidecar loads: " + err);
    require(s.file_sha256 == "c238af6f3b35bb19c1b9dfeb5b1d5d9ffe2086f5f1fd1c4ca73e2766f51c3f11", "real sidecar sha256 matches the README");
    require(s.labels == std::vector<std::string>{"swim_bladder", "eye_left", "eye_right"}, "real labels in training order");
    require(s.edges.size() == 3 && s.kpt_k == 3 && s.kpt_d == 3, "real edges and shape");
    std::cout << "  real sidecar ok: " << s.skeleton_id << " ipc hash " << s.file_sha256_prefix64 << std::endl;
}
}  // namespace

int main()
{
    test_good();
    test_rejections();
    test_real_deployment_if_present();
    std::cout << "pose_skeleton_sidecar_tests: all tests passed" << std::endl;
    return 0;
}
