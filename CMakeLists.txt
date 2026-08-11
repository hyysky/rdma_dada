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
option(BUILD_ALGORITHM_BENCHMARKS
       "Build portable algorithm microbenchmarks"
       OFF)
option(USE_CUDA
       "Enable CUDA support in the RDMA pipeline"
       OFF)

if(USE_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 86 CACHE STRING
            "CUDA architectures for GPU algorithm modules")
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit 12.0 REQUIRED)
endif()

add_library(rdma_pipeline_config STATIC
    src/config/beamform_weight_metadata.cpp
    src/config/json_value.cpp
    src/config/observation_config.cpp
    src/config/packet_format_config.cpp
    src/config/pipeline_config.cpp
    src/config/resolved_observation_plan.cpp
    src/config/resolved_plan_json.cpp
    src/config/sha256.cpp
    src/pipeline/dada_header_builder.cpp)
target_include_directories(rdma_pipeline_config
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(rdma_receive_policy STATIC
    src/io/rdma/receive_policy.cpp)
target_include_directories(rdma_receive_policy
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(rdma_pipeline_core STATIC
    src/pipeline/ascii_metadata.cpp
    src/pipeline/group_block_writer.cpp
    src/pipeline/metadata.cpp
    src/pipeline/worker_config.cpp)
target_include_directories(rdma_pipeline_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_pipeline_core PUBLIC rdma_pipeline_config)

add_library(rdma_module_vdif_unpack STATIC
    modules/vdif_unpack/atfp_block_writer.cpp
    modules/vdif_unpack/project_vdif_v1.cpp
    modules/vdif_unpack/vdif_atfp_engine.cpp
    modules/vdif_unpack/vdif_unpack_config.cpp
    modules/vdif_unpack/vdif_unpack_engine.cpp
    modules/vdif_unpack/vdif_unpack_header.cpp
    modules/vdif_unpack/vdif_timeline.cpp)
target_include_directories(rdma_module_vdif_unpack
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_module_vdif_unpack PUBLIC rdma_pipeline_core)

add_library(rdma_observation_compiler STATIC
    src/config/observation_artifacts.cpp)
target_include_directories(rdma_observation_compiler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_observation_compiler PUBLIC
    rdma_module_vdif_unpack
    rdma_pipeline_config)

add_library(rdma_vdif_sender_sim STATIC
    src/simulation/vdif_sender_sim.cpp
    src/simulation/vdif_sender_rate.cpp
    src/simulation/vdif_sender_batch.cpp
    src/simulation/udp_vdif_sender.cpp)
target_include_directories(rdma_vdif_sender_sim
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_vdif_sender_sim PUBLIC
    rdma_module_vdif_unpack
    rdma_pipeline_config)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(rdma_vdif_sender_sim PRIVATE _GNU_SOURCE)
endif()

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

add_library(rdma_module_complex_convert STATIC
    modules/complex_convert/complex_convert_module.cpp)
target_include_directories(rdma_module_complex_convert
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_module_complex_convert PUBLIC rdma_pipeline_core)
if(USE_CUDA)
    target_sources(rdma_module_complex_convert PRIVATE
        modules/complex_convert/complex_convert_cuda_backend.cu)
    target_compile_definitions(rdma_module_complex_convert PRIVATE
        RDMA_DADA_HAVE_CUDA=1)
    set_target_properties(rdma_module_complex_convert PROPERTIES
        CUDA_STANDARD 11
        CUDA_STANDARD_REQUIRED ON)
    target_link_libraries(rdma_module_complex_convert PRIVATE CUDA::cudart)
endif()

add_library(rdma_module_power STATIC
    modules/power/power_module.cpp)
target_include_directories(rdma_module_power
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_module_power PUBLIC rdma_pipeline_core)
if(USE_CUDA)
    target_sources(rdma_module_power PRIVATE
        modules/power/power_cuda_backend.cu)
    target_compile_definitions(rdma_module_power PRIVATE
        RDMA_DADA_HAVE_CUDA=1)
    set_target_properties(rdma_module_power PROPERTIES
        CUDA_STANDARD 11
        CUDA_STANDARD_REQUIRED ON)
    target_link_libraries(rdma_module_power PRIVATE CUDA::cudart)
endif()

add_library(rdma_module_stokes STATIC
    modules/stokes/stokes_module.cpp)
target_include_directories(rdma_module_stokes
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_module_stokes PUBLIC rdma_pipeline_core)
if(USE_CUDA)
    target_sources(rdma_module_stokes PRIVATE
        modules/stokes/stokes_cuda_backend.cu)
    target_compile_definitions(rdma_module_stokes PRIVATE
        RDMA_DADA_HAVE_CUDA=1)
    set_target_properties(rdma_module_stokes PROPERTIES
        CUDA_STANDARD 11
        CUDA_STANDARD_REQUIRED ON)
    target_link_libraries(rdma_module_stokes PRIVATE CUDA::cudart)
endif()

add_library(rdma_module_time_integrate STATIC
    modules/time_integrate/time_integrate_module.cpp)
target_include_directories(rdma_module_time_integrate
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_module_time_integrate PUBLIC rdma_pipeline_core)
if(USE_CUDA)
    target_sources(rdma_module_time_integrate PRIVATE
        modules/time_integrate/time_integrate_cuda_backend.cu)
    target_compile_definitions(rdma_module_time_integrate PRIVATE
        RDMA_DADA_HAVE_CUDA=1)
    set_target_properties(rdma_module_time_integrate PROPERTIES
        CUDA_STANDARD 11
        CUDA_STANDARD_REQUIRED ON)
    target_link_libraries(rdma_module_time_integrate PRIVATE CUDA::cudart)
endif()

add_library(rdma_module_transfer STATIC
    modules/host_to_device/host_to_device_module.cpp
    modules/device_to_host/device_to_host_module.cpp)
target_include_directories(rdma_module_transfer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_module_transfer PUBLIC rdma_pipeline_core)
if(USE_CUDA)
    target_sources(rdma_module_transfer PRIVATE
        modules/transfer/transfer_cuda_backend.cu)
    target_compile_definitions(rdma_module_transfer PRIVATE
        RDMA_DADA_HAVE_CUDA=1)
    set_target_properties(rdma_module_transfer PROPERTIES
        CUDA_STANDARD 11
        CUDA_STANDARD_REQUIRED ON)
    target_link_libraries(rdma_module_transfer PRIVATE CUDA::cudart)
endif()

add_library(rdma_pipeline_worker_core STATIC
    src/pipeline/module_chain.cpp)
target_include_directories(rdma_pipeline_worker_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdma_pipeline_worker_core
    PUBLIC
        rdma_pipeline_core
        rdma_module_beamform
        rdma_module_power
        rdma_module_stokes
        rdma_module_time_integrate)
target_link_libraries(rdma_observation_compiler PUBLIC
    rdma_module_complex_convert
    rdma_pipeline_worker_core)

add_executable(pipeline_config_inspect tools/pipeline_config_inspect.cpp)
target_link_libraries(pipeline_config_inspect PRIVATE rdma_pipeline_config)

add_executable(packet_format_inspect tools/packet_format_inspect.cpp)
target_link_libraries(packet_format_inspect PRIVATE rdma_pipeline_config)

add_executable(observation_config_compile
    tools/observation_config_compile.cpp)
target_link_libraries(observation_config_compile PRIVATE
    rdma_observation_compiler)

add_executable(pipeline_worker_config_inspect
    tools/pipeline_worker_config_inspect.cpp)
target_link_libraries(pipeline_worker_config_inspect PRIVATE rdma_pipeline_core)

add_executable(fpga_sender_sim apps/fpga_sender_sim/main.cpp)
target_link_libraries(fpga_sender_sim PRIVATE rdma_vdif_sender_sim)

if(BUILD_ALGORITHM_BENCHMARKS)
    add_executable(time_integrate_benchmark
        tools/time_integrate_benchmark.cpp)
    target_link_libraries(time_integrate_benchmark PRIVATE
        rdma_module_time_integrate)
    if(USE_CUDA)
        add_executable(time_integrate_cuda_benchmark
            tools/time_integrate_cuda_benchmark.cpp)
        target_link_libraries(time_integrate_cuda_benchmark PRIVATE
            rdma_module_time_integrate
            CUDA::cudart)
    endif()
endif()

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    add_executable(rdma_receive_policy_test tests/rdma_receive_policy_test.cpp)
    target_link_libraries(rdma_receive_policy_test PRIVATE rdma_receive_policy)
    add_executable(project_vdif_v1_test tests/project_vdif_v1_test.cpp)
    target_link_libraries(project_vdif_v1_test PRIVATE rdma_module_vdif_unpack)
    add_executable(vdif_unpack_config_test tests/vdif_unpack_config_test.cpp)
    target_link_libraries(vdif_unpack_config_test PRIVATE rdma_module_vdif_unpack)
    add_executable(vdif_unpack_header_test tests/vdif_unpack_header_test.cpp)
    target_link_libraries(vdif_unpack_header_test PRIVATE rdma_module_vdif_unpack)
    add_executable(vdif_timeline_test tests/vdif_timeline_test.cpp)
    target_link_libraries(vdif_timeline_test PRIVATE rdma_module_vdif_unpack)
    add_executable(vdif_unpack_engine_test tests/vdif_unpack_engine_test.cpp)
    target_link_libraries(vdif_unpack_engine_test PRIVATE rdma_module_vdif_unpack)
    add_executable(vdif_atfp_engine_test tests/vdif_atfp_engine_test.cpp)
    target_link_libraries(vdif_atfp_engine_test PRIVATE rdma_module_vdif_unpack)
    add_executable(atfp_block_writer_test tests/atfp_block_writer_test.cpp)
    target_link_libraries(atfp_block_writer_test PRIVATE rdma_module_vdif_unpack)
    add_executable(group_block_writer_test tests/group_block_writer_test.cpp)
    target_link_libraries(group_block_writer_test PRIVATE rdma_pipeline_core)
    add_executable(vdif_sender_sim_test tests/vdif_sender_sim_test.cpp)
    target_link_libraries(vdif_sender_sim_test PRIVATE
        rdma_vdif_sender_sim
        rdma_module_vdif_unpack)
    add_executable(vdif_sender_rate_test tests/vdif_sender_rate_test.cpp)
    target_link_libraries(vdif_sender_rate_test PRIVATE rdma_vdif_sender_sim)
    add_executable(vdif_sender_batch_test tests/vdif_sender_batch_test.cpp)
    target_link_libraries(vdif_sender_batch_test PRIVATE
        rdma_vdif_sender_sim
        rdma_module_vdif_unpack)
    add_executable(udp_vdif_sender_test tests/udp_vdif_sender_test.cpp)
    target_link_libraries(udp_vdif_sender_test PRIVATE
        rdma_vdif_sender_sim
        rdma_pipeline_config)
    add_executable(pipeline_config_test tests/pipeline_config_test.cpp)
    target_link_libraries(pipeline_config_test PRIVATE rdma_pipeline_config)
    add_executable(observation_config_test tests/observation_config_test.cpp)
    target_link_libraries(observation_config_test PRIVATE rdma_pipeline_config)
    add_executable(resolved_observation_plan_test
        tests/resolved_observation_plan_test.cpp)
    target_link_libraries(resolved_observation_plan_test PRIVATE
        rdma_pipeline_config)
    add_executable(config_identity_test tests/config_identity_test.cpp)
    target_link_libraries(config_identity_test PRIVATE rdma_pipeline_config)
    add_executable(beamform_weight_metadata_test
        tests/beamform_weight_metadata_test.cpp)
    target_link_libraries(beamform_weight_metadata_test PRIVATE
        rdma_pipeline_config)
    add_executable(observation_artifacts_test
        tests/observation_artifacts_test.cpp)
    target_link_libraries(observation_artifacts_test PRIVATE
        rdma_observation_compiler)
    add_executable(packet_format_config_test
        tests/packet_format_config_test.cpp)
    target_link_libraries(packet_format_config_test PRIVATE
        rdma_pipeline_config)
    add_executable(pipeline_core_test tests/pipeline_core_test.cpp)
    target_link_libraries(pipeline_core_test PRIVATE rdma_pipeline_core)
    add_executable(beamform_module_test tests/beamform_module_test.cpp)
    target_link_libraries(beamform_module_test PRIVATE rdma_module_beamform)
    add_executable(complex_convert_module_test
        tests/complex_convert_module_test.cpp)
    target_link_libraries(complex_convert_module_test PRIVATE
        rdma_module_complex_convert)
    add_executable(power_module_test tests/power_module_test.cpp)
    target_link_libraries(power_module_test PRIVATE rdma_module_power)
    add_executable(stokes_module_test tests/stokes_module_test.cpp)
    target_link_libraries(stokes_module_test PRIVATE rdma_module_stokes)
    add_executable(time_integrate_module_test
        tests/time_integrate_module_test.cpp)
    target_link_libraries(time_integrate_module_test PRIVATE
        rdma_module_time_integrate)
    add_executable(pipeline_worker_core_test
        tests/pipeline_worker_core_test.cpp)
    target_link_libraries(pipeline_worker_core_test
        PRIVATE rdma_pipeline_worker_core)
    add_executable(worker_resolved_plan_test
        tests/worker_resolved_plan_test.cpp)
    target_link_libraries(worker_resolved_plan_test PRIVATE
        rdma_pipeline_core)
    if(USE_CUDA)
        add_executable(beamform_cuda_test tests/beamform_cuda_test.cpp)
        target_link_libraries(beamform_cuda_test PRIVATE
            rdma_module_beamform
            CUDA::cudart)
        add_executable(complex_convert_cuda_test
            tests/complex_convert_cuda_test.cpp)
        target_link_libraries(complex_convert_cuda_test PRIVATE
            rdma_module_complex_convert
            CUDA::cudart)
        add_executable(complex_convert_transpose_cuda_benchmark
            tools/complex_convert_transpose_cuda_benchmark.cpp)
        target_link_libraries(complex_convert_transpose_cuda_benchmark PRIVATE
            rdma_module_complex_convert
            CUDA::cudart)
        add_executable(power_cuda_test tests/power_cuda_test.cpp)
        target_link_libraries(power_cuda_test PRIVATE
            rdma_module_power
            CUDA::cudart)
        add_executable(stokes_cuda_test tests/stokes_cuda_test.cpp)
        target_link_libraries(stokes_cuda_test PRIVATE
            rdma_module_stokes
            CUDA::cudart)
        add_executable(time_integrate_cuda_test
            tests/time_integrate_cuda_test.cpp)
        target_link_libraries(time_integrate_cuda_test PRIVATE
            rdma_module_time_integrate
            CUDA::cudart)
        add_executable(transfer_cuda_roundtrip_test
            tests/transfer_cuda_roundtrip_test.cpp)
        target_link_libraries(transfer_cuda_roundtrip_test PRIVATE
            rdma_module_transfer
            CUDA::cudart)
        add_executable(pipeline_worker_cuda_chain_test
            tests/pipeline_worker_cuda_chain_test.cpp)
        target_link_libraries(pipeline_worker_cuda_chain_test PRIVATE
            rdma_pipeline_worker_core
            rdma_module_complex_convert
            rdma_module_transfer
            CUDA::cudart)
        add_executable(pipeline_worker_cuda_products_test
            tests/pipeline_worker_cuda_products_test.cpp)
        target_link_libraries(pipeline_worker_cuda_products_test PRIVATE
            rdma_pipeline_worker_core
            rdma_module_complex_convert
            rdma_module_transfer
            CUDA::cudart)
    endif()
    add_test(
        NAME rdma_receive_policy_test
        COMMAND rdma_receive_policy_test)
    add_test(
        NAME project_vdif_v1_test
        COMMAND project_vdif_v1_test)
    add_test(
        NAME vdif_unpack_config_test
        COMMAND vdif_unpack_config_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/vdif_unpack.example.json
                ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json
                ${CMAKE_CURRENT_SOURCE_DIR}/config/packet_formats/frontend.example-v1.json)
    add_test(
        NAME vdif_unpack_header_test
        COMMAND vdif_unpack_header_test)
    add_test(
        NAME vdif_timeline_test
        COMMAND vdif_timeline_test)
    add_test(
        NAME vdif_unpack_engine_test
        COMMAND vdif_unpack_engine_test)
    add_test(
        NAME vdif_atfp_engine_test
        COMMAND vdif_atfp_engine_test)
    add_test(
        NAME atfp_block_writer_test
        COMMAND atfp_block_writer_test)
    add_test(
        NAME group_block_writer_test
        COMMAND group_block_writer_test)
    add_test(
        NAME vdif_sender_sim_test
        COMMAND vdif_sender_sim_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/fpga_sender_sim.example.json
                ${CMAKE_CURRENT_SOURCE_DIR}/config/fpga_sender_sim.paced.example.json)
    add_test(NAME vdif_sender_rate_test COMMAND vdif_sender_rate_test)
    add_test(NAME vdif_sender_batch_test COMMAND vdif_sender_batch_test)
    add_test(NAME udp_vdif_sender_test COMMAND udp_vdif_sender_test)
    add_test(
        NAME pipeline_config_test
        COMMAND pipeline_config_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.json)
    add_test(
        NAME observation_config_test
        COMMAND observation_config_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json)
    add_test(
        NAME resolved_observation_plan_test
        COMMAND resolved_observation_plan_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json
                ${CMAKE_CURRENT_SOURCE_DIR}/config/packet_formats/frontend.example-v1.json)
    add_test(
        NAME config_identity_test
        COMMAND config_identity_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json
                ${CMAKE_CURRENT_SOURCE_DIR}/config/packet_formats/frontend.example-v1.json)
    add_test(
        NAME observation_artifacts_test
        COMMAND observation_artifacts_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json)
    add_test(
        NAME packet_format_config_test
        COMMAND packet_format_config_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/packet_formats/frontend.example-v1.json)
    add_test(
        NAME pipeline_config_inspect_test
        COMMAND pipeline_config_inspect
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.json)
    add_test(
        NAME pipeline_worker_config_inspect_test
        COMMAND pipeline_worker_config_inspect
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline_worker.example.json)
    add_test(
        NAME pipeline_config_rejects_legacy_test
        COMMAND pipeline_config_inspect
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.conf)
    set_tests_properties(pipeline_config_rejects_legacy_test
        PROPERTIES WILL_FAIL TRUE)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME observation_config_compile_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/observation_config_compile_test.py
                    $<TARGET_FILE:observation_config_compile>
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json)
        add_test(
            NAME fpga_sender_sim_loopback_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fpga_sender_sim_loopback_test.py
                    $<TARGET_FILE:fpga_sender_sim>
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/fpga_sender_sim.example.json
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/fpga_sender_sim.paced.example.json)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(
                NAME fpga_sender_sim_linux_batch_test
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fpga_sender_sim_linux_batch_test.py
                        $<TARGET_FILE:fpga_sender_sim>
                        ${CMAKE_CURRENT_SOURCE_DIR}/config/fpga_sender_sim.paced.example.json)
        endif()
        add_test(
            NAME packet_format_inspect_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/packet_format_inspect_test.py
                    ${CMAKE_CURRENT_BINARY_DIR}/packet_format_inspect
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/packet_formats/frontend.example-v1.json)
        add_test(
            NAME config_conversion_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_conversion_test.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/convert_config_to_json.py
                    $<TARGET_FILE:pipeline_config_inspect>
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.conf
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline.example.json)
        add_test(
            NAME beamform_test_weights_generator_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/beamform_test_weights_generator_test.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_beamform_test_weights.py)
        add_test(
            NAME task8c_rate_point_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/task8c_rate_point_test.py)
        add_test(
            NAME atfp_throughput_campaign_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/atfp_throughput_campaign_test.py)
    endif()
    add_test(NAME pipeline_core_test COMMAND pipeline_core_test)
    add_test(NAME beamform_weight_metadata_test
        COMMAND beamform_weight_metadata_test)
    add_test(
        NAME beamform_module_test
        COMMAND beamform_module_test
                ${CMAKE_CURRENT_BINARY_DIR}/beamform_test_weights.npy)
    add_test(
        NAME complex_convert_module_test
        COMMAND complex_convert_module_test)
    add_test(NAME power_module_test COMMAND power_module_test)
    add_test(NAME stokes_module_test COMMAND stokes_module_test)
    add_test(NAME time_integrate_module_test COMMAND time_integrate_module_test)
    add_test(
        NAME pipeline_worker_core_test
        COMMAND pipeline_worker_core_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/pipeline_worker.example.json
                ${CMAKE_CURRENT_BINARY_DIR}/pipeline_worker_test_weights.npy)
    add_test(
        NAME worker_resolved_plan_test
        COMMAND worker_resolved_plan_test
                ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json
                ${CMAKE_CURRENT_SOURCE_DIR}/config/packet_formats/frontend.example-v1.json)
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
        add_test(
            NAME complex_convert_cuda_test
            COMMAND complex_convert_cuda_test)
        add_test(NAME power_cuda_test COMMAND power_cuda_test)
        add_test(NAME stokes_cuda_test COMMAND stokes_cuda_test)
        add_test(NAME time_integrate_cuda_test COMMAND time_integrate_cuda_test)
        add_test(
            NAME transfer_cuda_roundtrip_test
            COMMAND transfer_cuda_roundtrip_test)
        add_test(
            NAME pipeline_worker_cuda_chain_test
            COMMAND pipeline_worker_cuda_chain_test
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/beamform_cuda_weights_f2_p1_a2_b2_i8.npy)
        add_test(
            NAME pipeline_worker_cuda_products_test
            COMMAND pipeline_worker_cuda_products_test
                    ${CMAKE_CURRENT_BINARY_DIR}/pipeline_worker_cuda_products_weights.npy)
        add_test(
            NAME complex_convert_cuda_help_test
            COMMAND complex_convert_cuda_test -h)
        add_test(NAME beamform_cuda_help_test COMMAND beamform_cuda_test -h)
        add_test(NAME power_cuda_help_test COMMAND power_cuda_test -h)
        add_test(NAME stokes_cuda_help_test COMMAND stokes_cuda_test -h)
        add_test(
            NAME time_integrate_cuda_help_test
            COMMAND time_integrate_cuda_test -h)
        add_test(
            NAME transfer_cuda_roundtrip_help_test
            COMMAND transfer_cuda_roundtrip_test -h)
        add_test(
            NAME pipeline_worker_cuda_chain_help_test
            COMMAND pipeline_worker_cuda_chain_test -h)
        add_test(
            NAME pipeline_worker_cuda_products_help_test
            COMMAND pipeline_worker_cuda_products_test -h)
        set_tests_properties(
            beamform_cuda_fp32_test
            beamform_cuda_tf32_test
            complex_convert_cuda_test
            power_cuda_test
            stokes_cuda_test
            time_integrate_cuda_test
            transfer_cuda_roundtrip_test
            pipeline_worker_cuda_chain_test
            pipeline_worker_cuda_products_test
            complex_convert_cuda_help_test
            beamform_cuda_help_test
            power_cuda_help_test
            stokes_cuda_help_test
            time_integrate_cuda_help_test
            transfer_cuda_roundtrip_help_test
            pipeline_worker_cuda_chain_help_test
            pipeline_worker_cuda_products_help_test
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
    if(PSRDADA_PREFIX)
        set(_PSRDADA_RUNTIME_BIN_DEFAULT "${PSRDADA_PREFIX}/bin")
    elseif(PSRDADA_LIBRARY)
        get_filename_component(_PSRDADA_LIBRARY_DIR "${PSRDADA_LIBRARY}" DIRECTORY)
        get_filename_component(_PSRDADA_PREFIX "${_PSRDADA_LIBRARY_DIR}" DIRECTORY)
        set(_PSRDADA_RUNTIME_BIN_DEFAULT "${_PSRDADA_PREFIX}/bin")
    else()
        set(_PSRDADA_RUNTIME_BIN_DEFAULT "")
    endif()
    set(PSRDADA_RUNTIME_BIN_DIR "${_PSRDADA_RUNTIME_BIN_DEFAULT}" CACHE PATH
        "Directory containing dada_db, dada_diskdb and dada_dbdisk")

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
            rdma_receive_policy
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
            rdma_pipeline_core
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
    target_link_libraries(rdma2dada PRIVATE
        rdma_dada_core
        rdma_observation_compiler)

    add_executable(pipeline_worker apps/pipeline_worker/main.cpp)
    target_compile_definitions(pipeline_worker PRIVATE _GNU_SOURCE)
    target_link_libraries(pipeline_worker
        PRIVATE
            rdma_pipeline_worker_core
            rdma_observation_compiler
            rdma_module_complex_convert
            rdma_module_transfer
            ${PSRDADA_TARGET}
            Threads::Threads)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(pipeline_worker PRIVATE rt)
    endif()
    if(USE_CUDA)
        target_compile_definitions(pipeline_worker PRIVATE
            RDMA_DADA_HAVE_CUDA=1)
        target_link_libraries(pipeline_worker PRIVATE CUDA::cudart)
    endif()

    add_executable(vdif_unpack_worker apps/vdif_unpack_worker/main.cpp)
    target_compile_definitions(vdif_unpack_worker PRIVATE _GNU_SOURCE)
    target_link_libraries(vdif_unpack_worker
        PRIVATE
            rdma_module_vdif_unpack
            rdma_pipeline_config
            ${PSRDADA_TARGET}
            Threads::Threads)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(vdif_unpack_worker PRIVATE rt)
    endif()

    if(BUILD_TESTING)
        add_executable(dada_header_roundtrip_test
            tests/dada_header_roundtrip_test.cpp)
        target_link_libraries(dada_header_roundtrip_test
            PRIVATE
                rdma_observation_compiler
                rdma_dada_psrdada_io)
        add_test(
            NAME dada_header_roundtrip_test
            COMMAND dada_header_roundtrip_test
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/observation.example.json)
        add_test(
            NAME rdma_receiver_integration_test
            COMMAND bash
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rdma_receiver_integration.sh
                    $<TARGET_FILE:rdma2dada>
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(rdma_receiver_integration_test PROPERTIES
            LABELS "rdma;integration"
            SKIP_RETURN_CODE 77
            TIMEOUT 120)
        add_test(
            NAME vdif_unpack_worker_integration_test
            COMMAND bash
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vdif_unpack_worker_integration.sh
                    $<TARGET_FILE:vdif_unpack_worker>
                    $<TARGET_FILE:observation_config_compile>
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(vdif_unpack_worker_integration_test PROPERTIES
            LABELS "psrdada;integration"
            SKIP_RETURN_CODE 77
            TIMEOUT 90)
        if(USE_CUDA)
            add_test(
                NAME pipeline_worker_resolved_integration_test
                COMMAND bash
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline_worker_resolved_integration.sh
                        $<TARGET_FILE:pipeline_worker>
                        $<TARGET_FILE:observation_config_compile>
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_beamform_test_weights.py
                        ${CMAKE_CURRENT_SOURCE_DIR}
                        ${PSRDADA_RUNTIME_BIN_DIR})
            set_tests_properties(
                pipeline_worker_resolved_integration_test PROPERTIES
                LABELS "cuda;psrdada;integration"
                RUN_SERIAL TRUE
                SKIP_RETURN_CODE 77
                TIMEOUT 120)
        endif()
    endif()
endif()

message(STATUS "rdma_dada build configuration:")
message(STATUS "  BUILD_RDMA_PIPELINE = ${BUILD_RDMA_PIPELINE}")
message(STATUS "  BUILD_SSD_BENCHMARK = ${BUILD_SSD_BENCHMARK}")
message(STATUS "  USE_CUDA = ${USE_CUDA}")
if(USE_CUDA)
    message(STATUS "  CMAKE_CUDA_ARCHITECTURES = ${CMAKE_CUDA_ARCHITECTURES}")
endif()
