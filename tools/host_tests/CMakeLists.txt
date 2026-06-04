cmake_minimum_required(VERSION 3.16)
project(orange_worker_entry_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ORANGE_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_executable(worker_entry_ownership_core_host_tests
    "${ORANGE_REPO_ROOT}/tools/worker_entry_ownership_core_host_tests.cpp"
)
target_include_directories(worker_entry_ownership_core_host_tests PRIVATE
    "${ORANGE_REPO_ROOT}/src"
)

enable_testing()
add_test(
    NAME worker_entry_ownership_core_host_tests
    COMMAND worker_entry_ownership_core_host_tests
)
