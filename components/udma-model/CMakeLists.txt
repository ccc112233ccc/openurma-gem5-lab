cmake_minimum_required(VERSION 3.20)
project(ubsim_udma_model VERSION 0.1.0 LANGUAGES CXX)

set(SIMBRICKS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sources/simbricks"
    CACHE PATH "Pinned SimBricks source root")
if(NOT EXISTS "${SIMBRICKS_ROOT}/lib/simbricks/base/proto.h")
    message(FATAL_ERROR "SimBricks source missing at ${SIMBRICKS_ROOT}; run ./lab setup --sources-only")
endif()

add_library(ubsim_udma_model STATIC src/udma_model.cc)
target_include_directories(ubsim_udma_model PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ubsim_udma_model PUBLIC cxx_std_17)
target_compile_options(ubsim_udma_model PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
    add_executable(udma_ceq_credit_test tests/ceq_credit_test.cc)
    target_link_libraries(udma_ceq_credit_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_ceq_credit_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_ceq_credit COMMAND udma_ceq_credit_test)
    add_executable(udma_sq_pipeline_test tests/sq_pipeline_test.cc)
    target_link_libraries(udma_sq_pipeline_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_sq_pipeline_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_sq_pipeline COMMAND udma_sq_pipeline_test)
    add_executable(udma_model_test tests/model_test.cc)
    target_include_directories(udma_model_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(udma_model_test SYSTEM PRIVATE
        "${SIMBRICKS_ROOT}/lib")
    target_link_libraries(udma_model_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_model_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_model_separation COMMAND udma_model_test)
    add_executable(udma_token_dma_test tests/token_dma_test.cc)
    target_link_libraries(udma_token_dma_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_token_dma_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_token_dma_drain COMMAND udma_token_dma_test)
    add_executable(udma_jfc_query_test tests/jfc_query_test.cc)
    target_link_libraries(udma_jfc_query_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_jfc_query_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_jfc_query_context COMMAND udma_jfc_query_test)
    add_executable(udma_lifecycle_resource_test tests/lifecycle_resource_test.cc)
    target_link_libraries(udma_lifecycle_resource_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_lifecycle_resource_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_resource_destroy_drain COMMAND udma_lifecycle_resource_test)
    add_executable(udma_lifecycle_test tests/lifecycle_test.cc)
    target_link_libraries(udma_lifecycle_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_lifecycle_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_lifecycle_drain COMMAND udma_lifecycle_test)
    add_executable(udma_remote_service_test tests/remote_service_test.cc)
    target_link_libraries(udma_remote_service_test PRIVATE ubsim_udma_model)
    target_compile_options(udma_remote_service_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME udma_remote_service_drain COMMAND udma_remote_service_test)
endif()
