#include "rdma_dada/simulation/vdif_sender_batch.h"

#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"

#include <algorithm>
#include <limits>

namespace rdma_dada {
namespace simulation {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool Contains(const std::vector<std::uint64_t>& values, std::uint64_t value) {
    return std::binary_search(values.begin(), values.end(), value);
}

}  // namespace

VdifSenderBatch::VdifSenderBatch()
    : record_bytes_(0), size_(0) {}

bool VdifSenderBatch::Initialize(const VdifSenderSimConfig& config,
                                 std::string* error) {
    if (config.batch_packets == 0 || config.batch_packets > 64U)
        return Fail("batch_packets must be in 1..64", error);
    const std::vector<std::uint16_t> stations =
        config.schema_version == 3U
            ? config.station_ids
            : std::vector<std::uint16_t>(1U, config.station_id);
    if (stations.empty()) return Fail("sender has no configured Stations", error);
    std::vector<std::uint8_t> first;
    if (!BuildVdifSenderRecordForStation(
            config, 0, stations.front(), &first, error)) return false;
    if (first.size() < 32U)
        return Fail("VDIF record is smaller than its header", error);
    if (first.size() > std::numeric_limits<std::size_t>::max() /
                           config.batch_packets)
        return Fail("packet batch storage exceeds size_t range", error);

    config_ = config;
    record_bytes_ = first.size();
    size_ = 0;
    station_templates_.clear();
    for (std::size_t index = 0; index < stations.size(); ++index) {
        std::vector<std::uint8_t> record;
        if (!BuildVdifSenderRecordForStation(
                config, 0, stations[index], &record, error)) return false;
        station_templates_.push_back(record);
    }
    storage_.assign(record_bytes_ * config.batch_packets, 0);
    packets_.resize(config.batch_packets);
    for (std::uint32_t i = 0; i < config.batch_packets; ++i) {
        std::uint8_t* slot = &storage_[static_cast<std::size_t>(i) * record_bytes_];
        std::copy(first.begin() + 32, first.end(), slot + 32);
        packets_[i].data = slot;
        packets_[i].bytes = record_bytes_;
        packets_[i].group_index = 0;
        packets_[i].station_id = stations.front();
        packets_[i].station_index = 0;
    }
    return true;
}

bool VdifSenderBatch::Prepare(std::uint64_t first_packet,
                              std::uint32_t packet_count,
                              std::string* error) {
    if (record_bytes_ == 0) return Fail("packet batch is not initialized", error);
    if (packet_count == 0 || packet_count > packets_.size())
        return Fail("packet_count must be within batch capacity", error);
    const std::uint64_t station_count = station_templates_.size();
    if (config_.group_count >
        std::numeric_limits<std::uint64_t>::max() / station_count)
        return Fail("flattened packet count exceeds uint64 range", error);
    const std::uint64_t total_packets = config_.group_count * station_count;
    if (first_packet >= total_packets || packet_count > total_packets - first_packet)
        return Fail("packet batch range exceeds flattened transfer", error);

    std::uint64_t cached_group = std::numeric_limits<std::uint64_t>::max();
    modules::vdif_unpack::ProjectVdifHeader cached_header = {};
    for (std::uint32_t i = 0; i < packet_count; ++i) {
        const std::uint64_t flat_index = first_packet + i;
        const std::uint64_t group = flat_index / station_count;
        const std::uint64_t within_group = flat_index % station_count;
        const std::size_t station_index = static_cast<std::size_t>(
            (within_group + group % station_count) % station_count);
        const std::uint16_t station =
            config_.schema_version == 3U
                ? config_.station_ids[station_index]
                : config_.station_id;
        std::uint8_t* slot = const_cast<std::uint8_t*>(packets_[i].data);
        if (config_.payload_mode == "DETERMINISTIC") {
            std::vector<std::uint8_t> record;
            if (!BuildVdifSenderRecordForStation(
                    config_, group, station, &record, error)) return false;
            std::copy(record.begin(), record.end(), slot);
        } else {
            const std::vector<std::uint8_t>& repeat =
                station_templates_[station_index];
            std::copy(repeat.begin() + 32, repeat.end(), slot + 32);
            if (group != cached_group) {
                if (!BuildVdifSenderHeaderFromValidatedConfig(
                        config_, group,
                        config_.schema_version == 3U
                            ? config_.station_ids.front()
                            : config_.station_id,
                        &cached_header, error)) return false;
                cached_group = group;
            }
            modules::vdif_unpack::ProjectVdifHeader header = cached_header;
            header.station_id = station;
            if (!modules::vdif_unpack::EncodeProjectVdifV1(
                    header, slot, record_bytes_, error)) return false;
            if (Contains(config_.invalid_header_groups, group)) slot[28] = 1;
        }
        packets_[i].group_index = group;
        packets_[i].station_id = station;
        packets_[i].station_index = static_cast<std::uint32_t>(station_index);
    }
    size_ = packet_count;
    return true;
}

std::uint32_t VdifSenderBatch::capacity() const {
    return static_cast<std::uint32_t>(packets_.size());
}

std::uint32_t VdifSenderBatch::size() const { return size_; }

const VdifPacketView& VdifSenderBatch::packet(std::uint32_t index) const {
    return packets_[index];
}

}  // namespace simulation
}  // namespace rdma_dada
