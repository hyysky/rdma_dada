#include "rdma_dada/simulation/vdif_sender_sim.h"
#include "rdma_dada/simulation/udp_vdif_sender.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

const std::uint64_t kPicosecondsPerNanosecond = 1000;

bool Contains(const std::vector<std::uint64_t>& values, std::uint64_t value) {
    return std::binary_search(values.begin(), values.end(), value);
}

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool ParseUtc(const std::string& text,
              std::chrono::system_clock::time_point* result,
              std::string* error) {
    int year, month, day, hour, minute, second, consumed = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d-%d:%d:%d%n",
                    &year, &month, &day, &hour, &minute, &second,
                    &consumed) != 6 || consumed != static_cast<int>(text.size())) {
        if (error) *error = "start_utc must use YYYY-MM-DD-HH:MM:SS";
        return false;
    }
    static const int month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (year < 1970 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        if (error) *error = "start_utc contains an out-of-range value";
        return false;
    }
    int maximum_day = month_days[month - 1];
    if (month == 2 && IsLeapYear(year)) ++maximum_day;
    if (day < 1 || day > maximum_day) {
        if (error) *error = "start_utc contains an invalid calendar date";
        return false;
    }

    int adjusted_year = year;
    const unsigned adjusted_month = static_cast<unsigned>(month);
    adjusted_year -= adjusted_month <= 2;
    const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned day_of_year =
        (153U * (adjusted_month + (adjusted_month > 2 ? -3 : 9)) + 2U) / 5U +
        static_cast<unsigned>(day) - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
        year_of_era / 100U + day_of_year;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 +
        static_cast<std::int64_t>(day_of_era) - 719468;
    const std::int64_t unix_seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    *result = std::chrono::system_clock::time_point(
        std::chrono::seconds(unix_seconds));
    return true;
}

class SocketHandle {
public:
    explicit SocketHandle(int value) : value_(value) {}
    ~SocketHandle() { if (value_ >= 0) close(value_); }
    int get() const { return value_; }
private:
    int value_;
};

bool EnableDoNotFragment(int socket_fd, std::string* error) {
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
    const int policy = IP_PMTUDISC_DO;
    if (setsockopt(socket_fd, IPPROTO_IP, IP_MTU_DISCOVER,
                   &policy, sizeof(policy)) != 0) {
        if (error) *error = std::string("setsockopt(IP_MTU_DISCOVER): ") +
                            std::strerror(errno);
        return false;
    }
#elif defined(IP_DONTFRAG)
    const int enabled = 1;
    if (setsockopt(socket_fd, IPPROTO_IP, IP_DONTFRAG,
                   &enabled, sizeof(enabled)) != 0) {
        if (error) *error = std::string("setsockopt(IP_DONTFRAG): ") +
                            std::strerror(errno);
        return false;
    }
#else
    (void) socket_fd;
    (void) error;
#endif
    return true;
}

bool SendRecord(int socket_fd, const std::vector<std::uint8_t>& record,
                std::string* error) {
    ssize_t sent;
    do {
        sent = send(socket_fd, record.data(), record.size(), 0);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        if (error) *error = std::string("UDP send failed: ") + std::strerror(errno);
        return false;
    }
    if (static_cast<std::size_t>(sent) != record.size()) {
        if (error) *error = "UDP send did not transmit the complete record";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: fpga_sender_sim CONFIG.json\n";
        return 2;
    }
    rdma_dada::simulation::VdifSenderSimConfig config = {};
    std::string error;
    if (!rdma_dada::simulation::LoadVdifSenderSimConfig(argv[1], &config, &error)) {
        std::cerr << "CONFIG_ERROR: " << error << '\n';
        return 1;
    }

    if (config.schema_version == 2U || config.schema_version == 3U) {
        rdma_dada::simulation::VdifSenderStats stats = {};
        if (!rdma_dada::simulation::RunUdpVdifSender(config, &stats, &error)) {
            std::cerr << "SEND_ERROR: " << error << '\n';
            return 1;
        }
        std::cout << rdma_dada::simulation::FormatVdifSenderStatsJson(
            config, stats);
        return 0;
    }

    SocketHandle socket_fd(socket(AF_INET, SOCK_DGRAM, 0));
    if (socket_fd.get() < 0) {
        std::cerr << "SOCKET_ERROR: socket: " << std::strerror(errno) << '\n';
        return 1;
    }
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(config.destination_port);
    if (inet_pton(AF_INET, config.destination_ip.c_str(),
                  &destination.sin_addr) != 1) {
        std::cerr << "CONFIG_ERROR: destination.ip must be a numeric IPv4 address\n";
        return 1;
    }
    if (!EnableDoNotFragment(socket_fd.get(), &error)) {
        std::cerr << "SOCKET_ERROR: " << error << '\n';
        return 1;
    }
    if (connect(socket_fd.get(), reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination)) != 0) {
        std::cerr << "SOCKET_ERROR: connect: " << std::strerror(errno) << '\n';
        return 1;
    }

    std::chrono::system_clock::time_point realtime_start;
    if (config.mode == "REALTIME") {
        if (!ParseUtc(config.start_utc, &realtime_start, &error)) {
            std::cerr << "CONFIG_ERROR: " << error << '\n';
            return 1;
        }
        if (realtime_start <= std::chrono::system_clock::now()) {
            std::cerr << "CONFIG_ERROR: REALTIME start_utc must be in the future\n";
            return 1;
        }
    }

    const std::uint64_t packet_duration_ps =
        static_cast<std::uint64_t>(config.geometry.nsamp_per_packet) *
        config.sample_interval_ps;
    std::uint64_t sent_records = 0;
    std::uint64_t dropped_groups = 0;
    std::uint64_t duplicated_groups = 0;
    for (std::uint64_t group = 0; group < config.group_count; ++group) {
        if (config.mode == "REALTIME") {
            const std::uint64_t elapsed_ps = group * packet_duration_ps;
            std::this_thread::sleep_until(
                realtime_start + std::chrono::nanoseconds(
                    elapsed_ps / kPicosecondsPerNanosecond));
        }
        if (Contains(config.drop_groups, group)) {
            ++dropped_groups;
            continue;
        }
        std::vector<std::uint8_t> record;
        if (!rdma_dada::simulation::BuildVdifSenderRecord(
                config, group, &record, &error)) {
            std::cerr << "RECORD_ERROR: group " << group << ": " << error << '\n';
            return 1;
        }
        if (!SendRecord(socket_fd.get(), record, &error)) {
            std::cerr << "SEND_ERROR: group " << group << ": " << error << '\n';
            return 1;
        }
        ++sent_records;
        if (Contains(config.duplicate_groups, group)) {
            if (!SendRecord(socket_fd.get(), record, &error)) {
                std::cerr << "SEND_ERROR: duplicate group " << group << ": "
                          << error << '\n';
                return 1;
            }
            ++sent_records;
            ++duplicated_groups;
        }
    }

    std::cout << "station_id=" << config.station_id
              << " groups=" << config.group_count
              << " sent_records=" << sent_records
              << " dropped_groups=" << dropped_groups
              << " duplicated_groups=" << duplicated_groups << '\n';
    return 0;
}
