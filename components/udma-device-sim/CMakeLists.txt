cmake_minimum_required(VERSION 3.20)
project(openurma_udma_device_sim VERSION 0.1.0 LANGUAGES C CXX)

set(SIMBRICKS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sources/simbricks"
    CACHE PATH "Pinned SimBricks source root")
if(NOT EXISTS "${SIMBRICKS_ROOT}/lib/simbricks/base/if.c")
    message(FATAL_ERROR "SimBricks source missing; run scripts/fetch-sources.sh")
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../udma-model"
                 "${CMAKE_CURRENT_BINARY_DIR}/udma-model")

add_library(simbricks_base STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/simbricks_base_portable.c")
target_include_directories(simbricks_base SYSTEM PUBLIC "${SIMBRICKS_ROOT}/lib")
target_compile_definitions(simbricks_base PRIVATE
    SIMBRICKS_BASE_SOURCE="${SIMBRICKS_ROOT}/lib/simbricks/base/if.c")
target_compile_options(simbricks_base PRIVATE -Wall -Wextra)

add_executable(udma-device-sim src/main.cc)
target_include_directories(udma-device-sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(udma-device-sim SYSTEM PRIVATE "${SIMBRICKS_ROOT}/lib")
target_link_libraries(udma-device-sim PRIVATE openurma_udma_model simbricks_base)
target_compile_features(udma-device-sim PRIVATE cxx_std_17)
target_compile_options(udma-device-sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
    add_executable(udma-transport-peer tests/transport_peer.cc)
    target_include_directories(udma-transport-peer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(udma-transport-peer SYSTEM PRIVATE
        "${SIMBRICKS_ROOT}/lib")
    target_link_libraries(udma-transport-peer PRIVATE
        openurma_udma_model simbricks_base)
    target_compile_features(udma-transport-peer PRIVATE cxx_std_17)
    target_compile_options(udma-transport-peer PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME udma_process_contract
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_contract.sh"
                "$<TARGET_FILE:udma-device-sim>"
                "$<TARGET_FILE:udma-transport-peer>"
                "${CMAKE_CURRENT_BINARY_DIR}/contract")
    set_tests_properties(udma_process_contract PROPERTIES TIMEOUT 20)
endif()

install(TARGETS udma-device-sim RUNTIME DESTINATION bin)
