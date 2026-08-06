#include "rdma_dada/simulation/vdif_sender_sim.h"

#include "rdma_dada/config/json_value.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace rdma_dada {
namespace simulation {
namespace {

const std::uint64_t kPicosecondsPerSecond = UINT64_C(1000000000000);
const std::uint64_t kIpv4UdpOverhead = 28;
const std::uint64_t kMaximumUdpPayload = 65507;

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     const std::string& name, std::uint64_t* result,
                     std::string* error) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return Fail(name + " exceeds uint64 range", error);
    *result = left * right;
    return true;
}

bool ExactKeys(const json::Value::Object& object,
               const char* const* keys, std::size_t count,
               const std::string& path, std::string* error) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!object.count(keys[i])) return Fail(path + " is missing: " + keys[i], error);
    }
    for (json::Value::Object::const_iterator it = object.begin();
         it != object.end(); ++it) {
        bool known = false;
        for (std::size_t i = 0; i < count; ++i) known = known || it->first == keys[i];
        if (!known) return Fail(path + " has unknown field: " + it->first, error);
    }
    return true;
}

bool ObjectField(const json::Value::Object& parent, const char* key,
                 const json::Value::Object** result, std::string* error) {
    const json::Value& value = parent.find(key)->second;
    if (value.type() != json::Value::kObject)
        return Fail(std::string(key) + " must be an object", error);
    *result = &value.object();
    return true;
}

bool StringField(const json::Value::Object& object, const char* key,
                 std::string* result, std::string* error) {
    const json::Value& value = object.find(key)->second;
    if (value.type() != json::Value::kString)
        return Fail(std::string(key) + " must be a string", error);
    *result = value.text();
    return true;
}

bool ParseUnsignedText(const std::string& name, const std::string& text,
                       std::uint64_t* result, std::string* error) {
    if (text.empty()) return Fail(name + " must not be empty", error);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9')
            return Fail(name + " must contain decimal digits only", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0')
        return Fail(name + " exceeds uint64 range", error);
    *result = static_cast<std::uint64_t>(parsed);
    return true;
}

bool UintField(const json::Value::Object& object, const char* key,
               std::uint64_t* result, std::string* error) {
    const json::Value& value = object.find(key)->second;
    if (value.type() != json::Value::kNumber ||
        value.text().find_first_of(".eE-") != std::string::npos)
        return Fail(std::string(key) + " must use non-negative integer JSON syntax", error);
    return ParseUnsignedText(key, value.text(), result, error);
}

bool FaultList(const json::Value::Object& object, const char* key,
               std::uint64_t group_count, std::vector<std::uint64_t>* result,
               std::string* error) {
    const json::Value& value = object.find(key)->second;
    if (value.type() != json::Value::kArray)
        return Fail(std::string(key) + " must be an array", error);
    std::vector<std::uint64_t> parsed;
    for (std::size_t i = 0; i < value.array().size(); ++i) {
        const json::Value& entry = value.array()[i];
        if (entry.type() != json::Value::kNumber ||
            entry.text().find_first_of(".eE-") != std::string::npos)
            return Fail(std::string(key) + " entries must be integers", error);
        std::uint64_t index = 0;
        if (!ParseUnsignedText(key, entry.text(), &index, error)) return false;
        if (index >= group_count)
            return Fail(std::string(key) + " entry is outside group_count", error);
        if (!parsed.empty() && index <= parsed.back())
            return Fail(std::string(key) + " entries must be strictly increasing", error);
        parsed.push_back(index);
    }
    *result = parsed;
    return true;
}

bool Contains(const std::vector<std::uint64_t>& values, std::uint64_t value) {
    return std::binary_search(values.begin(), values.end(), value);
}

bool ValidateConfig(const VdifSenderSimConfig& config, std::string* error) {
    if (config.destination_ip.empty()) return Fail("destination IP must not be empty", error);
    if (config.destination_port == 0) return Fail("destination port must be positive", error);
    if (config.path_mtu < 68) return Fail("IPv4 path MTU must be at least 68 bytes", error);
    if (config.geometry.nchan == 0 ||
        (config.geometry.npol != 1 && config.geometry.npol != 2) ||
        config.geometry.nsamp_per_packet == 0 ||
        (config.geometry.component_bits != 8 && config.geometry.component_bits != 16))
        return Fail("packet TFP/component geometry is invalid", error);
    if (config.reference_epoch > 63 || config.start_seconds > 0x3fffffffU)
        return Fail("VDIF start time exceeds header field width", error);
    if (config.sample_interval_ps == 0 || config.group_count == 0)
        return Fail("sample interval and group count must be positive", error);
    if (config.mode != "BURST" && config.mode != "REALTIME")
        return Fail("mode must be BURST or REALTIME", error);
    if (config.mode == "REALTIME" && config.start_utc.empty())
        return Fail("REALTIME mode requires start_utc", error);

    std::uint64_t payload = config.geometry.nsamp_per_packet;
    if (!CheckedMultiply(payload, config.geometry.nchan, "TF elements", &payload, error) ||
        !CheckedMultiply(payload, config.geometry.npol, "TFP elements", &payload, error) ||
        !CheckedMultiply(payload, 2U * (config.geometry.component_bits / 8U),
                         "payload bytes", &payload, error)) return false;
    if (payload != config.geometry.payload_bytes)
        return Fail("payload bytes do not match TFP complex geometry", error);
    if (payload > std::numeric_limits<std::uint64_t>::max() - 32U)
        return Fail("record bytes exceed uint64 range", error);
    const std::uint64_t record_bytes = 32U + payload;
    if (record_bytes % 8U != 0 || record_bytes / 8U > 0xffffffU)
        return Fail("record length must fit Project VDIF frame units", error);
    if (record_bytes > kMaximumUdpPayload)
        return Fail("record exceeds maximum IPv4 UDP payload", error);
    if (config.path_mtu <= kIpv4UdpOverhead ||
        record_bytes > config.path_mtu - kIpv4UdpOverhead)
        return Fail("record exceeds configured IPv4 path MTU", error);

    std::uint64_t packet_duration = 0;
    if (!CheckedMultiply(config.geometry.nsamp_per_packet,
                         config.sample_interval_ps,
                         "packet duration", &packet_duration, error)) return false;
    std::uint64_t last_elapsed = 0;
    if (!CheckedMultiply(config.group_count - 1U, packet_duration,
                         "last group time", &last_elapsed, error)) return false;
    const std::uint64_t second_offset = last_elapsed / kPicosecondsPerSecond;
    if (second_offset > 0x3fffffffU - config.start_seconds)
        return Fail("last group seconds exceed VDIF 30-bit field", error);
    const std::uint64_t maximum_frame =
        (kPicosecondsPerSecond - 1U) / packet_duration;
    if (maximum_frame > 0xffffffU)
        return Fail("frame ordinal may exceed VDIF 24-bit field", error);

    std::set<std::uint64_t> faults;
    const std::vector<std::uint64_t>* lists[] = {
        &config.drop_groups, &config.duplicate_groups,
        &config.invalid_header_groups
    };
    for (std::size_t list = 0; list < 3; ++list) {
        for (std::size_t i = 0; i < lists[list]->size(); ++i) {
            const std::uint64_t index = (*lists[list])[i];
            if (i != 0 && index <= (*lists[list])[i - 1U])
                return Fail("fault group lists must be strictly increasing", error);
            if (index >= config.group_count)
                return Fail("fault group index is outside group_count", error);
            if (!faults.insert(index).second)
                return Fail("fault group lists must not overlap", error);
        }
    }
    return true;
}

}  // namespace

bool LoadVdifSenderSimConfig(const std::string& path,
                             VdifSenderSimConfig* config,
                             std::string* error) {
    if (!config) return Fail("config output pointer is null", error);
    std::ifstream input(path.c_str());
    if (!input) return Fail("cannot open config file: " + path, error);
    std::ostringstream contents; contents << input.rdbuf();
    json::Value root;
    if (!json::Parse(contents.str(), &root, error)) return false;
    if (root.type() != json::Value::kObject) return Fail("root must be an object", error);
    const json::Value::Object& object = root.object();
    static const char* const root_keys[] = {
        "schema_version", "destination", "station", "packet", "time", "faults"
    };
    if (!ExactKeys(object, root_keys, 6, "root", error)) return false;
    std::uint64_t schema = 0;
    if (!UintField(object, "schema_version", &schema, error) || schema != 1)
        return Fail("schema_version must be 1", error);

    const json::Value::Object *destination, *station, *packet, *time, *faults;
    if (!ObjectField(object, "destination", &destination, error) ||
        !ObjectField(object, "station", &station, error) ||
        !ObjectField(object, "packet", &packet, error) ||
        !ObjectField(object, "time", &time, error) ||
        !ObjectField(object, "faults", &faults, error)) return false;
    static const char* const destination_keys[] = {"ip", "port", "path_mtu"};
    static const char* const station_keys[] = {"station_id"};
    static const char* const packet_keys[] = {
        "first_channel_id", "nchan", "npol", "nsamp_per_packet",
        "component_bits", "sample_interval_ps"
    };
    static const char* const time_keys[] = {
        "reference_epoch", "start_seconds", "group_count", "mode", "start_utc"
    };
    static const char* const fault_keys[] = {
        "drop_groups", "duplicate_groups", "invalid_header_groups"
    };
    if (!ExactKeys(*destination, destination_keys, 3, "destination", error) ||
        !ExactKeys(*station, station_keys, 1, "station", error) ||
        !ExactKeys(*packet, packet_keys, 6, "packet", error) ||
        !ExactKeys(*time, time_keys, 5, "time", error) ||
        !ExactKeys(*faults, fault_keys, 3, "faults", error)) return false;

    VdifSenderSimConfig parsed = {};
    std::uint64_t port, mtu, station_id, first_channel, nchan, npol;
    std::uint64_t nsamp, component_bits, epoch, start_seconds;
    std::string interval;
    if (!StringField(*destination, "ip", &parsed.destination_ip, error) ||
        !UintField(*destination, "port", &port, error) ||
        !UintField(*destination, "path_mtu", &mtu, error) ||
        !UintField(*station, "station_id", &station_id, error) ||
        !UintField(*packet, "first_channel_id", &first_channel, error) ||
        !UintField(*packet, "nchan", &nchan, error) ||
        !UintField(*packet, "npol", &npol, error) ||
        !UintField(*packet, "nsamp_per_packet", &nsamp, error) ||
        !UintField(*packet, "component_bits", &component_bits, error) ||
        !StringField(*packet, "sample_interval_ps", &interval, error) ||
        !UintField(*time, "reference_epoch", &epoch, error) ||
        !UintField(*time, "start_seconds", &start_seconds, error) ||
        !UintField(*time, "group_count", &parsed.group_count, error) ||
        !StringField(*time, "mode", &parsed.mode, error) ||
        !StringField(*time, "start_utc", &parsed.start_utc, error)) return false;
    if (port > 65535U || mtu > std::numeric_limits<std::uint32_t>::max() ||
        station_id > 65535U || first_channel > 65535U || nchan > 255U ||
        npol > 255U || nsamp > std::numeric_limits<std::uint32_t>::max() ||
        component_bits > 255U || epoch > 255U ||
        start_seconds > std::numeric_limits<std::uint32_t>::max())
        return Fail("sender config integer exceeds destination field width", error);
    parsed.destination_port = static_cast<std::uint16_t>(port);
    parsed.path_mtu = static_cast<std::uint32_t>(mtu);
    parsed.station_id = static_cast<std::uint16_t>(station_id);
    parsed.geometry.first_channel_id = static_cast<std::uint16_t>(first_channel);
    parsed.geometry.nchan = static_cast<std::uint8_t>(nchan);
    parsed.geometry.npol = static_cast<std::uint8_t>(npol);
    parsed.geometry.nsamp_per_packet = static_cast<std::uint32_t>(nsamp);
    parsed.geometry.component_bits = static_cast<std::uint8_t>(component_bits);
    parsed.reference_epoch = static_cast<std::uint8_t>(epoch);
    parsed.start_seconds = static_cast<std::uint32_t>(start_seconds);
    if (!ParseUnsignedText("sample_interval_ps", interval,
                           &parsed.sample_interval_ps, error)) return false;
    std::uint64_t payload = nsamp;
    if (!CheckedMultiply(payload, nchan, "payload T x F", &payload, error) ||
        !CheckedMultiply(payload, npol, "payload T x F x P", &payload, error) ||
        !CheckedMultiply(payload, 2U * (component_bits / 8U),
                         "payload bytes", &payload, error)) return false;
    parsed.geometry.payload_bytes = payload;
    if (!FaultList(*faults, "drop_groups", parsed.group_count,
                   &parsed.drop_groups, error) ||
        !FaultList(*faults, "duplicate_groups", parsed.group_count,
                   &parsed.duplicate_groups, error) ||
        !FaultList(*faults, "invalid_header_groups", parsed.group_count,
                   &parsed.invalid_header_groups, error) ||
        !ValidateConfig(parsed, error)) return false;
    *config = parsed;
    return true;
}

bool BuildVdifSenderRecord(const VdifSenderSimConfig& config,
                           std::uint64_t group_index,
                           std::vector<std::uint8_t>* record,
                           std::string* error) {
    if (!record) return Fail("record output pointer is null", error);
    if (!ValidateConfig(config, error)) return false;
    if (group_index >= config.group_count)
        return Fail("group index is outside group_count", error);
    std::uint64_t packet_duration = 0;
    std::uint64_t elapsed = 0;
    if (!CheckedMultiply(config.geometry.nsamp_per_packet,
                         config.sample_interval_ps,
                         "packet duration", &packet_duration, error) ||
        !CheckedMultiply(group_index, packet_duration,
                         "group time", &elapsed, error)) return false;
    const std::uint64_t second_offset = elapsed / kPicosecondsPerSecond;
    const std::uint64_t within_second = elapsed % kPicosecondsPerSecond;
    const std::uint64_t frame = within_second / packet_duration;

    modules::vdif_unpack::ProjectVdifHeader header = {};
    header.seconds_from_reference_epoch = static_cast<std::uint32_t>(
        config.start_seconds + second_offset);
    header.reference_epoch = config.reference_epoch;
    header.frame_number_within_second = static_cast<std::uint32_t>(frame);
    header.station_id = config.station_id;
    header.first_channel_id = config.geometry.first_channel_id;
    header.nchan = config.geometry.nchan;
    header.npol = config.geometry.npol;
    header.nsamp_per_packet = config.geometry.nsamp_per_packet;
    header.component_bits = config.geometry.component_bits;
    header.frame_length_units_8_bytes = static_cast<std::uint32_t>(
        (32U + config.geometry.payload_bytes) / 8U);

    std::vector<std::uint8_t> result(static_cast<std::size_t>(
        32U + config.geometry.payload_bytes), 0);
    if (!modules::vdif_unpack::EncodeProjectVdifV1(
            header, result.data(), result.size(), error)) return false;
    const std::uint64_t mask = config.geometry.component_bits == 8 ? 0xffU : 0xffffU;
    std::size_t offset = 32;
    for (std::uint32_t t = 0; t < config.geometry.nsamp_per_packet; ++t) {
        for (std::uint32_t f = 0; f < config.geometry.nchan; ++f) {
            for (std::uint32_t p = 0; p < config.geometry.npol; ++p) {
                for (std::uint32_t component = 0; component < 2; ++component) {
                    const std::uint64_t value =
                        (config.station_id + (group_index % (mask + 1U)) * 7U +
                         (t % (mask + 1U)) * 3U + (f % (mask + 1U)) * 5U +
                         (p % (mask + 1U)) * 11U + component) & mask;
                    result[offset++] = static_cast<std::uint8_t>(value & 0xffU);
                    if (config.geometry.component_bits == 16)
                        result[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
                }
            }
        }
    }
    if (Contains(config.invalid_header_groups, group_index)) result[28] = 1;
    *record = result;
    return true;
}

}  // namespace simulation
}  // namespace rdma_dada
