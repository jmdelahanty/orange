#include "ptp_readback_evidence.h"
#include "recording_metadata_csv.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        std::ostringstream out;
        orange::recording_metadata::write_header(out);
        orange::recording_metadata::write_row(out, 201, 1787695084226944090ULL,
                                              1787695047236290646ULL);
        require(out.str() == "frame_id,timestamp,timestamp_sys,recording_frame_id\n"
                "201,1787695084226944090,1787695047236290646,201\n",
                "native metadata must preserve parent IDs and exact integer timestamps");

        orange::PtpReadbackEvidence e;
        e.recorded_frame(67794);
        e.observe("TwoStep", "Slave", 90548, "old", 100);
        e.reset();
        e.observe("TwoStep", "Slave", 90918, "new", 200);
        require(e.observations()[0]["first_recording_frame_id"] == 0,
                "pre-first-frame readback must not carry previous recording ID");
        require(!e.finish_if_stopped(false, false), "pre-start is not a finished recording");
        e.recorded_frame(1);
        e.observe("TwoStep", "Slave", 90919, "new", 300);
        require(e.observations()[0]["last_recording_frame_id"] == 1,
                "source stream counter must remain distinct from parent ID");
        e.observe("TwoStep", std::nullopt, 91019, "new", 1000);
        require(e.observations().size() == 2 && e.observations()[1]["ptp_status"].is_null(),
                "failed status read cannot reuse previous successful Slave value");
        e.observe(std::nullopt, std::nullopt, 91119, "new", 2000);
        require(e.coverage()["attempts"] == 4 && e.coverage()["status_read_failures"] == 2,
                "failed read attempts must remain in coverage");
        require(e.coverage()["max_sample_gap_ns"] == 1000,
                "coverage must quantify sample gaps without claiming continuous lock");
        e.recorded_frame(200);
        require(!e.finish_if_stopped(false, true), "rolling pause preserves parent scope");
        e.recorded_frame(201);
        require(e.last_recording_frame_id() == 201, "clip rollover preserves parent IDs");
        require(e.finish_if_stopped(false, false), "recording stop finalizes evidence");
        auto frozen = e.observations();
        e.observe("Off", "Disabled", 99999, "later", 3000);
        e.recorded_frame(12345);
        require(e.observations() == frozen && e.last_recording_frame_id() == 201,
                "continued streaming cannot change closed recording evidence");
        e.reset();
        require(!e.closed() && e.last_recording_frame_id() == 0 && e.observations().empty(),
                "new parent recording resets all observation state");
        for (size_t i = 0; i < orange::PtpReadbackEvidence::kMaxStateRuns + 2; ++i)
            e.observe("TwoStep", i % 2 ? "Slave" : "Faulty", i, "sample", i + 1);
        require(e.observations().size() == orange::PtpReadbackEvidence::kMaxStateRuns &&
                e.coverage()["omitted_observations"] == 2,
                "state oscillation must remain bounded and explicitly incomplete");
        std::cout << "recording_timing_evidence_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
