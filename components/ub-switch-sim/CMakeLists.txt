cmake_minimum_required(VERSION 3.20)
project(ubsim_ub_switch_sim VERSION 0.1.0 LANGUAGES C CXX)

set(SIMBRICKS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sources/simbricks"
    CACHE PATH "Pinned SimBricks source root")
if(NOT EXISTS "${SIMBRICKS_ROOT}/lib/simbricks/base/if.c")
    message(FATAL_ERROR "SimBricks source missing; run scripts/fetch-sources.sh")
endif()

add_library(ub_switch_simbricks_base STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../udma-device-sim/simbricks_base_portable.c")
target_include_directories(ub_switch_simbricks_base SYSTEM PUBLIC
    "${SIMBRICKS_ROOT}/lib")
target_compile_options(ub_switch_simbricks_base PRIVATE -Wall -Wextra)

add_executable(ub-switch-sim src/main.cc)
target_include_directories(ub-switch-sim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(ub-switch-sim SYSTEM PRIVATE "${SIMBRICKS_ROOT}/lib")
target_link_libraries(ub-switch-sim PRIVATE ub_switch_simbricks_base)
target_compile_features(ub-switch-sim PRIVATE cxx_std_17)
target_compile_options(ub-switch-sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
    add_executable(ub-net-contract-peer tests/contract_peer.cc)
    target_include_directories(ub-net-contract-peer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(ub-net-contract-peer SYSTEM PRIVATE
        "${SIMBRICKS_ROOT}/lib")
    target_link_libraries(ub-net-contract-peer PRIVATE ub_switch_simbricks_base)
    target_compile_features(ub-net-contract-peer PRIVATE cxx_std_17)
    target_compile_options(ub-net-contract-peer PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ub_switch_process_contract
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_contract.sh"
                "$<TARGET_FILE:ub-switch-sim>"
                "$<TARGET_FILE:ub-net-contract-peer>"
                "${CMAKE_CURRENT_BINARY_DIR}/contract" off)
    add_test(NAME ub_switch_synchronized_contract
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_contract.sh"
                "$<TARGET_FILE:ub-switch-sim>"
                "$<TARGET_FILE:ub-net-contract-peer>"
                "${CMAKE_CURRENT_BINARY_DIR}/synchronized-contract" required)
    set_tests_properties(ub_switch_process_contract PROPERTIES TIMEOUT 20)
    set_tests_properties(ub_switch_synchronized_contract PROPERTIES TIMEOUT 20)
endif()

install(TARGETS ub-switch-sim RUNTIME DESTINATION bin)
