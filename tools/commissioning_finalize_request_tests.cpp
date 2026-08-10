#include "gui/spatial_layout/commissioning_finalize_request.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace spatial = orange::gui::spatial_layout;

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void TestCitrusOwnsSourceSessionDerivation()
{
    const auto params = spatial::build_commissioning_finalize_params(
        "release-transaction",
        "/rigs/omnifin0/shadow/shadow.json",
        "sha256:canvas",
        true);
    Require(params.value("transaction_id", "") == "release-transaction",
            "transaction identity should be preserved");
    Require(params.value("canvas_path", "") ==
                "/rigs/omnifin0/shadow/shadow.json",
            "canvas path should be preserved");
    Require(params.value("expected_canvas_checksum", "") ==
                "sha256:canvas",
            "canvas compare-and-swap checksum should be preserved");
    Require(params.value("accept_commissioning_armed", false),
            "explicit acceptance should be preserved");
    Require(params.contains("orange_session_dirs") &&
                params["orange_session_dirs"].is_array() &&
                params["orange_session_dirs"].empty(),
            "Orange must leave source-session derivation to Citrus");
}

void TestUnarmedRequestRemainsUnarmed()
{
    const auto params = spatial::build_commissioning_finalize_params(
        "release-transaction", "/canvas.json", "sha256:canvas", false);
    Require(!params.value("accept_commissioning_armed", true),
            "the builder must never arm a request implicitly");
}

}  // namespace

int main()
{
    try {
        TestCitrusOwnsSourceSessionDerivation();
        TestUnarmedRequestRemainsUnarmed();
        std::cout << "commissioning finalize request tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "commissioning finalize request tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
