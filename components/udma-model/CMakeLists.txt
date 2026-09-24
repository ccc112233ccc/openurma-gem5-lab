cmake_minimum_required(VERSION 3.20)
project(openurma_udma_model VERSION 0.1.0 LANGUAGES CXX)

set(SIMBRICKS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sources/simbricks"
    CACHE PATH "Pinned SimBricks source root")
if(NOT EXISTS "${SIMBRICKS_ROOT}/lib/simbricks/base/proto.h")
    message(FATAL_ERROR "SimBricks source missing at ${SIMBRICKS_ROOT}; run ./lab setup --sources-only")
endif()

add_library(openurma_udma_model STATIC src/udma_model.cc)
target_include_directories(openurma_udma_model PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(openurma_udma_model PUBLIC cxx_std_17)
target_compile_options(openurma_udma_model PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
    add_executable(udma_model_test tests/model_test.cc)
    target_include_directories(udma_model_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(udma_model_test SYSTEM PRIVATE
        "${SIMBRICKS_ROOT}/lib")
    target_link_libraries(udma_model_test PRIVATE openurma_udma_model)
    target_compile_options(udma_model_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_model_separation COMMAND udma_model_test)
endif()
