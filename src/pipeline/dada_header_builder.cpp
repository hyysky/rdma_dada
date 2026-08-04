#include "rdma_dada/pipeline/dada_header_builder.h"

#include <cstdio>
#include <cstring>

namespace rdma_dada {
namespace {

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool UtcToMjd(const std::string& utc, double* mjd, std::string* error) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    if (std::sscanf(utc.c_str(), "%d-%d-%d-%d:%d:%d%n", &year, &month, &day,
                    &hour, &minute, &second, &consumed) != 6 ||
        consumed != static_cast<int>(utc.size())) {
        if (error) *error = "UTC_START must use YYYY-MM-DD-HH:MM:SS";
        return false;
    }
    static const int days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (year < 1 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) {
        if (error) *error = "UTC_START contains an out-of-range date or time";
        return false;
    }
    int max_day = days_per_month[month - 1];
    if (month == 2 && IsLeapYear(year)) ++max_day;
    if (day < 1 || day > max_day) {
        if (error) *error = "UTC_START contains an invalid calendar date";
        return false;
    }

    const int a = (14 - month) / 12;
    const int y = year + 4800 - a;
    const int m = month + 12 * a - 3;
    const int jdn = day + (153 * m + 2) / 5 + 365 * y + y / 4 -
                    y / 100 + y / 400 - 32045;
    const double day_fraction =
        (hour + minute / 60.0 + second / 3600.0) / 24.0;
    *mjd = static_cast<double>(jdn - 2400001) + day_fraction;
    return true;
}

bool CopyField(const std::string& source, char* destination,
               std::size_t destination_size, const char* name,
               std::string* error) {
    if (source.empty()) {
        if (error) *error = std::string(name) + " must not be empty";
        return false;
    }
    if (source.size() >= destination_size) {
        if (error) *error = std::string(name) + " is too long";
        return false;
    }
    std::memcpy(destination, source.c_str(), source.size() + 1);
    return true;
}

}  // namespace

bool BuildPipelineDadaHeader(const PipelineConfig& config,
                             const PipelineLayout& layout,
                             DataStage stage,
                             dada_header_t* header,
                             std::string* error) {
    if (!header) {
        if (error) *error = "header output pointer is null";
        return false;
    }

    dada_header_t result = dada_header_t();
    result.pipeline_version = DADA_PIPELINE_CONTRACT_VERSION;
    result.nant = config.nant;
    result.nchan = config.nchan;
    result.npol = config.npol;
    result.nbit = config.packet_nbit;
    result.pkt_header = config.packet_header_bytes;
    result.pkt_data = config.packet_payload_bytes;
    result.pkt_nsamp = config.packet_samples;
    result.pkt_tsamp = config.sample_interval_us;
    result.bytes_per_second = layout.payload_bytes_per_second;
    result.raw_bytes_per_second = layout.raw_bytes_per_second;

    if (!CopyField(config.utc_start, result.utc_start, sizeof(result.utc_start),
                   "UTC_START", error) ||
        !CopyField(config.payload_order, result.order, sizeof(result.order),
                   "ORDER", error) ||
        !UtcToMjd(config.utc_start, &result.mjd, error)) {
        return false;
    }

    const char* stage_name = NULL;
    if (stage == DataStage::kRaw) {
        stage_name = "RAW";
        result.record_header_bytes = config.packet_header_bytes;
        result.record_bytes = layout.raw_record_bytes;
        result.resolution = layout.raw_resolution;
        result.filebytes = layout.raw_file_bytes;
    } else if (stage == DataStage::kCompute) {
        stage_name = "COMPUTE";
        result.record_header_bytes = 0;
        result.record_bytes = layout.compute_record_bytes;
        result.resolution = layout.compute_resolution;
        result.filebytes = layout.compute_file_bytes;
    } else {
        if (error) *error = "unsupported DATA_STAGE";
        return false;
    }
    if (!CopyField(stage_name, result.data_stage, sizeof(result.data_stage),
                   "DATA_STAGE", error)) {
        return false;
    }

    *header = result;
    return true;
}

}  // namespace rdma_dada
