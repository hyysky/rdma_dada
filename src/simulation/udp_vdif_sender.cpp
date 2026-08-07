#include "rdma_dada/simulation/udp_vdif_sender.h"

#include "rdma_dada/simulation/vdif_sender_batch.h"
#include "rdma_dada/simulation/vdif_sender_rate.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace rdma_dada {
namespace simulation {
namespace {

const std::uint64_t kFinalSpinNs = UINT64_C(50000);

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

class SocketHandle {
public:
    explicit SocketHandle(int value) : value_(value) {}
    ~SocketHandle() { if (value_ >= 0) close(value_); }
    int get() const { return value_; }
private:
    int value_;
};

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool ParseUtc(const std::string& text,
              std::chrono::system_clock::time_point* result,
              std::string* error) {
    int year, month, day, hour, minute, second, consumed = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d-%d:%d:%d%n",
                    &year, &month, &day, &hour, &minute, &second,
                    &consumed) != 6 || consumed != static_cast<int>(text.size()))
        return Fail("start_utc must use YYYY-MM-DD-HH:MM:SS", error);
    static const int month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (year < 1970 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59)
        return Fail("start_utc contains an out-of-range value", error);
    int maximum_day = month_days[month - 1];
    if (month == 2 && IsLeapYear(year)) ++maximum_day;
    if (day < 1 || day > maximum_day)
        return Fail("start_utc contains an invalid calendar date", error);
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
    const std::int64_t unix_seconds =
        days * 86400 + hour * 3600 + minute * 60 + second;
    *result = std::chrono::system_clock::time_point(
        std::chrono::seconds(unix_seconds));
    return true;
}

bool MonotonicNowNs(std::uint64_t* result, std::string* error) {
    timespec value = {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return Fail(std::string("clock_gettime(CLOCK_MONOTONIC): ") +
                    std::strerror(errno), error);
    if (value.tv_sec < 0)
        return Fail("monotonic clock returned a negative second", error);
    const std::uint64_t seconds = static_cast<std::uint64_t>(value.tv_sec);
    if (seconds > (std::numeric_limits<std::uint64_t>::max() -
                   static_cast<std::uint64_t>(value.tv_nsec)) /
                      UINT64_C(1000000000))
        return Fail("monotonic nanoseconds exceed uint64 range", error);
    *result = seconds * UINT64_C(1000000000) +
        static_cast<std::uint64_t>(value.tv_nsec);
    return true;
}

bool WaitUntilMonotonic(std::uint64_t deadline_ns,
                        bool* overrun,
                        std::string* error) {
    bool late_on_entry = false;
    bool first_check = true;
    for (;;) {
        std::uint64_t now = 0;
        if (!MonotonicNowNs(&now, error)) return false;
        if (first_check) {
            late_on_entry = now > deadline_ns;
            first_check = false;
        }
        if (now >= deadline_ns) {
            if (overrun) *overrun = late_on_entry;
            return true;
        }
        const std::uint64_t remaining = deadline_ns - now;
        if (remaining > kFinalSpinNs) {
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(remaining - kFinalSpinNs));
        }
    }
}

bool EnableDoNotFragment(int socket_fd, std::string* error) {
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
    const int policy = IP_PMTUDISC_DO;
    if (setsockopt(socket_fd, IPPROTO_IP, IP_MTU_DISCOVER,
                   &policy, sizeof(policy)) != 0)
        return Fail(std::string("setsockopt(IP_MTU_DISCOVER): ") +
                    std::strerror(errno), error);
#elif defined(IP_DONTFRAG)
    const int enabled = 1;
    if (setsockopt(socket_fd, IPPROTO_IP, IP_DONTFRAG,
                   &enabled, sizeof(enabled)) != 0)
        return Fail(std::string("setsockopt(IP_DONTFRAG): ") +
                    std::strerror(errno), error);
#else
    (void) socket_fd;
    (void) error;
#endif
    return true;
}

bool SendOne(int socket_fd, const VdifPacketView& packet,
             std::string* error) {
    ssize_t sent;
    do {
        sent = send(socket_fd, packet.data, packet.bytes, 0);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0)
        return Fail(std::string("UDP send failed: ") + std::strerror(errno),
                    error);
    if (static_cast<std::size_t>(sent) != packet.bytes)
        return Fail("UDP send did not transmit a complete record", error);
    return true;
}

#if defined(__linux__)
bool SendBatch(int socket_fd, const VdifSenderBatch& batch,
               std::vector<iovec>* vectors,
               std::vector<mmsghdr>* messages,
               VdifSenderStats* stats, std::string* error) {
    for (std::uint32_t i = 0; i < batch.size(); ++i) {
        (*vectors)[i].iov_base = const_cast<std::uint8_t*>(batch.packet(i).data);
        (*vectors)[i].iov_len = batch.packet(i).bytes;
        std::memset(&(*messages)[i], 0, sizeof((*messages)[i]));
        (*messages)[i].msg_hdr.msg_iov = &(*vectors)[i];
        (*messages)[i].msg_hdr.msg_iovlen = 1;
    }
    std::uint32_t offset = 0;
    while (offset < batch.size()) {
        const int sent = sendmmsg(socket_fd, &(*messages)[offset],
                                  batch.size() - offset, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return Fail(std::string("sendmmsg failed: ") +
                        std::strerror(errno), error);
        }
        if (sent == 0) return Fail("sendmmsg returned zero messages", error);
        for (int i = 0; i < sent; ++i) {
            if ((*messages)[offset + i].msg_len !=
                (*vectors)[offset + i].iov_len)
                return Fail("sendmmsg reported a short UDP record", error);
        }
        offset += static_cast<std::uint32_t>(sent);
        if (offset < batch.size()) {
            ++stats->short_batches;
            stats->retried_packets += batch.size() - offset;
        }
    }
    return true;
}
#endif

}  // namespace

bool RunUdpVdifSender(const VdifSenderSimConfig& config,
                      VdifSenderStats* stats,
                      std::string* error) {
    if (!stats) return Fail("sender statistics pointer is null", error);
    *stats = VdifSenderStats();
    if (config.schema_version != 2U || config.mode != "PACED")
        return Fail("paced UDP runtime requires schema v2 PACED config", error);
    if (!config.drop_groups.empty() || !config.duplicate_groups.empty() ||
        !config.invalid_header_groups.empty())
        return Fail("paced all-valid runtime does not accept fault lists", error);

    SocketHandle socket_fd(socket(AF_INET, SOCK_DGRAM, 0));
    if (socket_fd.get() < 0)
        return Fail(std::string("socket: ") + std::strerror(errno), error);
    sockaddr_in source = {};
    source.sin_family = AF_INET;
    source.sin_port = htons(config.source_port);
    if (inet_pton(AF_INET, config.source_ip.c_str(), &source.sin_addr) != 1)
        return Fail("source.ip must be a numeric IPv4 address", error);
    if (bind(socket_fd.get(), reinterpret_cast<const sockaddr*>(&source),
             sizeof(source)) != 0)
        return Fail(std::string("bind source endpoint: ") +
                    std::strerror(errno), error);
    const int send_buffer = 16 * 1024 * 1024;
    if (setsockopt(socket_fd.get(), SOL_SOCKET, SO_SNDBUF,
                   &send_buffer, sizeof(send_buffer)) != 0)
        return Fail(std::string("setsockopt(SO_SNDBUF): ") +
                    std::strerror(errno), error);
    if (!EnableDoNotFragment(socket_fd.get(), error)) return false;

    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(config.destination_port);
    if (inet_pton(AF_INET, config.destination_ip.c_str(),
                  &destination.sin_addr) != 1)
        return Fail("destination.ip must be a numeric IPv4 address", error);
    if (connect(socket_fd.get(), reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination)) != 0)
        return Fail(std::string("connect destination: ") +
                    std::strerror(errno), error);

    VdifSenderBatch batch;
    if (!batch.Initialize(config, error)) return false;
    std::chrono::system_clock::time_point realtime_start;
    if (!ParseUtc(config.start_utc, &realtime_start, error)) return false;
    if (realtime_start <= std::chrono::system_clock::now())
        return Fail("PACED start_utc must be in the future", error);
    std::this_thread::sleep_until(realtime_start);

    std::uint64_t start_ns = 0;
    if (!MonotonicNowNs(&start_ns, error)) return false;
    const SenderRatePlan rate_plan = {
        config.target_payload_bits_per_second, start_ns
    };
    stats->scheduled_packets = config.group_count;
#if defined(__linux__)
    stats->backend = "SENDMMSG";
    std::vector<iovec> vectors(batch.capacity());
    std::vector<mmsghdr> messages(batch.capacity());
#else
    stats->backend = "SEND";
#endif

    std::uint64_t first_group = 0;
    std::uint64_t cumulative_bytes = 0;
    while (first_group < config.group_count) {
        const std::uint64_t remaining = config.group_count - first_group;
        const std::uint32_t count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, batch.capacity()));
        std::uint64_t deadline = 0;
        if (!ComputePayloadDeadlineNs(rate_plan, cumulative_bytes,
                                      &deadline, error)) return false;
        bool overrun = false;
        if (!WaitUntilMonotonic(deadline, &overrun, error)) return false;
        if (overrun && first_group != 0) ++stats->overrun_batches;
        if (!batch.Prepare(first_group, count, error)) return false;
#if defined(__linux__)
        if (!SendBatch(socket_fd.get(), batch, &vectors, &messages,
                       stats, error)) {
            stats->failed_packets += count;
            return false;
        }
#else
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!SendOne(socket_fd.get(), batch.packet(i), error)) {
                stats->failed_packets += count - i;
                return false;
            }
        }
#endif
        ++stats->batches;
        stats->sent_packets += count;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (cumulative_bytes >
                std::numeric_limits<std::uint64_t>::max() -
                    batch.packet(i).bytes)
                return Fail("sent payload byte counter exceeds uint64 range",
                            error);
            cumulative_bytes += batch.packet(i).bytes;
        }
        stats->payload_bytes = cumulative_bytes;
        first_group += count;
    }
    std::uint64_t completion_deadline = 0;
    if (!ComputePayloadDeadlineNs(rate_plan, cumulative_bytes,
                                  &completion_deadline, error)) return false;
    bool completion_overrun = false;
    if (!WaitUntilMonotonic(completion_deadline, &completion_overrun, error))
        return false;
    std::uint64_t end_ns = 0;
    if (!MonotonicNowNs(&end_ns, error)) return false;
    stats->elapsed_ns = end_ns - start_ns;
    if (stats->sent_packets != stats->scheduled_packets)
        return Fail("sent packet count differs from scheduled packet count",
                    error);
    return true;
}

std::string FormatVdifSenderStatsJson(const VdifSenderSimConfig& config,
                                      const VdifSenderStats& stats) {
    const double actual_gbps = stats.elapsed_ns == 0 ? 0.0 :
        static_cast<double>(stats.payload_bytes) * 8.0 /
            static_cast<double>(stats.elapsed_ns);
    const double packets_per_second = stats.elapsed_ns == 0 ? 0.0 :
        static_cast<double>(stats.sent_packets) * 1e9 /
            static_cast<double>(stats.elapsed_ns);
    std::ostringstream output;
    output << '{'
           << "\"station_id\":" << config.station_id << ','
           << "\"source_ip\":\"" << config.source_ip << "\","
           << "\"source_port\":" << config.source_port << ','
           << "\"destination_ip\":\"" << config.destination_ip << "\","
           << "\"destination_port\":" << config.destination_port << ','
           << "\"scheduled_packets\":" << stats.scheduled_packets << ','
           << "\"sent_packets\":" << stats.sent_packets << ','
           << "\"retried_packets\":" << stats.retried_packets << ','
           << "\"failed_packets\":" << stats.failed_packets << ','
           << "\"payload_bytes\":" << stats.payload_bytes << ','
           << "\"elapsed_ns\":" << stats.elapsed_ns << ','
           << "\"target_payload_gbps\":" << std::fixed
           << std::setprecision(9)
           << static_cast<double>(config.target_payload_bits_per_second) / 1e9
           << ','
           << "\"actual_payload_gbps\":" << actual_gbps << ','
           << "\"packets_per_second\":" << packets_per_second << ','
           << "\"batches\":" << stats.batches << ','
           << "\"short_batches\":" << stats.short_batches << ','
           << "\"overrun_batches\":" << stats.overrun_batches << ','
           << "\"batch_packets\":" << config.batch_packets << ','
           << "\"payload_mode\":\"" << config.payload_mode << "\","
           << "\"backend\":\"" << stats.backend << "\"}"
           << std::endl;
    return output.str();
}

}  // namespace simulation
}  // namespace rdma_dada
