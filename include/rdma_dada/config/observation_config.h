#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {

enum class ObservationModuleKind {
    kBeamform,
    kPower,
    kStokes,
    kIntegrate
};

enum class CudaPipelineMode {
    kSynchronousDirect,
    kStagedPipeline
};

const char* CudaPipelineModeName(CudaPipelineMode mode);

struct UtcDateTime {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

struct ObservationModuleConfig {
    ObservationModuleKind kind;
    std::string weights_file;
    std::string weights_order;
    std::string weights_id;
    std::string weights_scale;
    std::string compute_mode;
    std::uint64_t integration_length;
    std::string integration_operation;
};

struct ObservationConfig {
    std::uint32_t schema_version;
    std::string source_path;
    std::string observation_id;
    std::string utc_start;
    std::string duration_seconds;
    std::uint64_t duration_ps;
    std::vector<std::uint16_t> station_ids;
    std::uint16_t first_channel_id;
    std::uint32_t nchan;
    std::uint32_t npol;
    std::uint64_t sample_interval_ps;

    std::string telescope;
    std::uint64_t bandwidth_hz;
    std::uint64_t center_frequency_hz;

    std::string wire_profile_path;
    std::uint64_t samples_per_packet;

    std::uint64_t groups_per_block;
    std::uint64_t raw_ring_blocks;
    std::uint64_t compute_ring_blocks;
    std::uint64_t window_blocks;

    std::uint32_t raw_key;
    std::uint32_t compute_key;
    std::uint32_t output_key;

    bool disk_enabled;
    std::uint64_t blocks_per_file;
    bool direct_io;

    std::string receiver_device;
    std::string destination_mac;
    std::string destination_ip;
    std::uint16_t destination_port;

    std::string backend;
    int cuda_device;
    bool run_once;
    CudaPipelineMode cuda_pipeline_mode;
    std::uint32_t cuda_inflight_blocks;
    std::string conversion_scale;
    std::string output_sample_format;
    std::vector<ObservationModuleConfig> modules;
};

bool ParseExactSecondsToPicoseconds(const std::string& text,
                                    std::uint64_t* picoseconds,
                                    std::string* error);

bool ParseUtcDateTime(const std::string& text,
                      UtcDateTime* value,
                      std::string* error);

bool ParseObservationConfigText(const std::string& contents,
                                const std::string& source_path,
                                ObservationConfig* config,
                                std::string* error);

bool LoadObservationConfig(const std::string& path,
                           ObservationConfig* config,
                           std::string* error);

}  // namespace rdma_dada
