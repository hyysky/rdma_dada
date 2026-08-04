cmake_minimum_required(VERSION 3.18)

project(rdma_dada VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RDMA_DADA_PIPELINE_DEFAULT ON)
else()
    set(RDMA_DADA_PIPELINE_DEFAULT OFF)
endif()

option(BUILD_RDMA_PIPELINE
       "Build the Linux PSRDADA/RDMA receiver pipeline"
       ${RDMA_DADA_PIPELINE_DEFAULT})
option(BUILD_SSD_BENCHMARK
       "Build the portable sequential-write benchmark"
       ON)
option(USE_CUDA
       "Enable CUDA support in the RDMA pipeline"
       OFF)

if(USE_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 89 CACHE STRING
            "CUDA architectures for GPU algorithm modules")
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit 12.0 REQUIRED)
endif()

add_library(rdma_pipeline_config STATIC
    src/config/json_value.cpp
    src/config/pipeline_config.cpp
    src/pipeline/dada_header_builder.cpp)
target_include_directories(rdma_pipeline_config
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(rdma_pipeline_core STATIC
    src/pipeline/metadata.cpp)
target_include_directories(rdma_pipeline_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_pipeline_core PUBLIC rdma_pipeline_config)

add_library(rdma_module_beamform STATIC
    modules/beamform/beamform_module.cpp)
target_include_directories(rdma_module_beamform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_module_beamform PUBLIC rdma_pipeline_core)
if(USE_CUDA)
    target_sources(rdma_module_beamform PRIVATE
        modules/beamform/beamform_cuda_backend.cu)
    target_compile_definitions(rdma_module_beamform PRIVATE
        RDMA_DADA_HAVE_CUDA=1)
    set_target_properties(rdma_module_beamform PROPERTIES
        CUDA_STANDARD 11
        CUDA_STANDARD_REQUIRED ON)
    target_link_libraries(rdma_module_beamform PRIVATE
        CUDA::cudart
        CUDA::cublas)
endif()

add_executable(pipeline_config_inspect tools/pipeline_config_inspect.cpp)
target_link_libraries(pipeline_config_inspect PRIVATE rdma_pipeline_config)

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    add_executable(pipeline_config_test tests/pipeline_config_test.cpp)
    target_link_libraries(pipeline_config_test PRIVATE rdma_pipeline_config)
    add_executable(pipeline_core_test tests/pipeline_core_test.cpp)
    target_link_libraries(pipeline_core_test PRIVATE rdma_pipeline_core)
    add_executable(beamform_module_test tests/beamform_module_test.cpp)
    target_link_libraries(beamform_module_test PRIVATE rdma_module_beamform)
    if(USE_CUDA)
        add_executable(beamform_cuda_test tests/beamform_cuda_test.cpp)
        target_link_libraries(beamform_cuda_test PRIVATE
            rdma_module_beamform
            CUDA::cudart)
    endif()
    add_test(
        NAME pipeline_config_test
        COMMAND pipeline_config_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.json)
    add_test(
        NAME pipeline_config_inspect_test
        COMMAND pipeline_config_inspect
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.json)
    add_test(
        NAME pipeline_config_rejects_legacy_test
        COMMAND pipeline_config_inspect
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.conf)
    set_tests_properties(pipeline_config_rejects_legacy_test
        PROPERTIES WILL_FAIL TRUE)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME config_conversion_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_conversion_test.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/convert_config_to_json.py
                    $<TARGET_FILE:pipeline_config_inspect>
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.conf
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.json)
    endif()
    add_test(NAME pipeline_core_test COMMAND pipeline_core_test)
    add_test(
        NAME beamform_module_test
        COMMAND beamform_module_test
                ${CMAKE_CURRENT_BINARY_DIR}/beamform_test_weights.npy)
    if(USE_CUDA)
        add_test(
            NAME beamform_cuda_fp32_test
            COMMAND beamform_cuda_test
                    ${CMAKE_CURRENT_BINARY_DIR}/beamform_cuda_fp32_weights.npy
                    FP32)
        add_test(
            NAME beamform_cuda_tf32_test
            COMMAND beamform_cuda_test
                    ${CMAKE_CURRENT_BINARY_DIR}/beamform_cuda_tf32_weights.npy
                    TF32)
        set_tests_properties(
            beamform_cuda_fp32_test
            beamform_cuda_tf32_test
            PROPERTIES
            LABELS "cuda"
            SKIP_RETURN_CODE 77)
    endif()
endif()

if(BUILD_SSD_BENCHMARK)
    add_executable(ssd_write_benchmark tools/ssd_write_benchmark.cpp)
    target_compile_definitions(ssd_write_benchmark PRIVATE _GNU_SOURCE)
endif()

if(BUILD_RDMA_PIPELINE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(WARNING
                "The RDMA pipeline is intended for Linux; configuration on "
                "${CMAKE_SYSTEM_NAME} may fail because ibverbs/PSRDADA are unavailable")
    endif()

    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)

    pkg_check_modules(PSRDADA QUIET IMPORTED_TARGET psrdada)
    if(PSRDADA_FOUND)
        set(PSRDADA_TARGET PkgConfig::PSRDADA)
    else()
        find_path(PSRDADA_INCLUDE_DIR
                  NAMES dada_hdu.h
                  PATHS /usr/local/include /usr/include)
        find_library(PSRDADA_LIBRARY
                     NAMES psrdada
                     PATHS /usr/local/lib /usr/lib /usr/local/lib64 /usr/lib64)
        if(NOT PSRDADA_INCLUDE_DIR OR NOT PSRDADA_LIBRARY)
            message(FATAL_ERROR
                    "PSRDADA was not found. Set PKG_CONFIG_PATH or provide "
                    "PSRDADA_INCLUDE_DIR and PSRDADA_LIBRARY")
        endif()
        add_library(psrdada_external UNKNOWN IMPORTED)
        set_target_properties(psrdada_external PROPERTIES
            IMPORTED_LOCATION "${PSRDADA_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PSRDADA_INCLUDE_DIR}")
        set(PSRDADA_TARGET psrdada_external)
    endif()

    pkg_check_modules(IBVERBS QUIET IMPORTED_TARGET libibverbs)
    if(IBVERBS_FOUND)
        set(IBVERBS_TARGET PkgConfig::IBVERBS)
    else()
        find_path(IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)
        find_library(IBVERBS_LIBRARY NAMES ibverbs)
        if(NOT IBVERBS_INCLUDE_DIR OR NOT IBVERBS_LIBRARY)
            message(FATAL_ERROR
                    "libibverbs was not found. Install rdma-core/libibverbs-dev "
                    "or provide IBVERBS_INCLUDE_DIR and IBVERBS_LIBRARY")
        endif()
        add_library(ibverbs_external UNKNOWN IMPORTED)
        set_target_properties(ibverbs_external PROPERTIES
            IMPORTED_LOCATION "${IBVERBS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IBVERBS_INCLUDE_DIR}")
        set(IBVERBS_TARGET ibverbs_external)
    endif()

    set(RDMA_IO_SOURCES
        src/io/rdma/verbs_context.cpp
        src/io/rdma/packet_builder.cpp
        src/io/rdma/receiver.cpp)
    set(PSRDADA_IO_SOURCES
        src/io/psrdada/ring_writer.cpp
        src/io/psrdada/header_codec.cpp)

    add_library(rdma_dada_rdma_io STATIC ${RDMA_IO_SOURCES})
    target_include_directories(rdma_dada_rdma_io
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(rdma_dada_rdma_io PUBLIC _GNU_SOURCE)
    target_link_libraries(rdma_dada_rdma_io
        PUBLIC
            ${IBVERBS_TARGET}
            Threads::Threads)

    add_library(rdma_dada_psrdada_io STATIC ${PSRDADA_IO_SOURCES})
    target_include_directories(rdma_dada_psrdada_io
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(rdma_dada_psrdada_io PUBLIC _GNU_SOURCE)
    # ring_writer still exposes legacy MR registration helpers, so this IO
    # adapter temporarily depends on libibverbs as well as PSRDADA.
    target_link_libraries(rdma_dada_psrdada_io
        PUBLIC
            ${PSRDADA_TARGET}
            ${IBVERBS_TARGET}
            Threads::Threads)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(rdma_dada_psrdada_io PUBLIC rt)
    endif()

    if(USE_CUDA)
        target_link_libraries(rdma_dada_rdma_io
            PUBLIC
                CUDA::cuda_driver
                CUDA::cudart)
    else()
        target_compile_definitions(rdma_dada_rdma_io PUBLIC NO_CUDA)
    endif()

    # Compatibility umbrella for applications that need the full ingest stack.
    add_library(rdma_dada_core INTERFACE)
    target_link_libraries(rdma_dada_core
        INTERFACE
            rdma_pipeline_config
            rdma_pipeline_core
            rdma_dada_rdma_io
            rdma_dada_psrdada_io)

    add_executable(rdma2dada apps/rdma2dada/main.cpp)
    target_link_libraries(rdma2dada PRIVATE rdma_dada_core)

    if(BUILD_TESTING)
        add_executable(dada_header_roundtrip_test
            tests/dada_header_roundtrip_test.cpp)
        target_link_libraries(dada_header_roundtrip_test
            PRIVATE
                rdma_pipeline_config
                rdma_dada_psrdada_io)
        add_test(
            NAME dada_header_roundtrip_test
            COMMAND dada_header_roundtrip_test
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.json)
    endif()
endif()

message(STATUS "rdma_dada build configuration:")
message(STATUS "  BUILD_RDMA_PIPELINE = ${BUILD_RDMA_PIPELINE}")
message(STATUS "  BUILD_SSD_BENCHMARK = ${BUILD_SSD_BENCHMARK}")
message(STATUS "  USE_CUDA = ${USE_CUDA}")
if(USE_CUDA)
    message(STATUS "  CMAKE_CUDA_ARCHITECTURES = ${CMAKE_CUDA_ARCHITECTURES}")
endif()
