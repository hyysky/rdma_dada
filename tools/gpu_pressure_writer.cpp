#include "rdma_dada/testing/gpu_pressure_plan.h"

#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <time.h>

extern "C" {
#include <dada_hdu.h>
#include <ipcbuf.h>
#include <ipcio.h>
#include <multilog.h>
}

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) { stop_requested = 1; }

struct Options {
    key_t key;
    std::string header_path;
    std::string metrics_path;
    std::uint64_t block_bytes;
    std::uint64_t block_count;
    std::uint64_t blocks_per_second;
};

void Usage(const char* program) {
    std::fprintf(
        stderr,
        "usage: %s --key HEX --header PATH --block-bytes N "
        "--block-count N --blocks-per-second N --metrics-json PATH\n",
        program);
}

bool ParseUint64(const char* text, std::uint64_t* value) {
    if (!text || !*text || !value || text[0] == '-') return false;
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ParseKey(const char* text, key_t* key) {
    if (!text || !*text || !key || text[0] == '-') return false;
    errno = 0;
    char* end = NULL;
    const unsigned long parsed = std::strtoul(text, &end, 16);
    if (errno != 0 || !end || *end != '\0' ||
        parsed > std::numeric_limits<unsigned int>::max()) {
        return false;
    }
    *key = static_cast<key_t>(parsed);
    return true;
}

bool ParseOptions(int argc, char** argv, Options* options) {
    if (!options) return false;
    Options parsed = {};
    for (int index = 1; index < argc; ++index) {
        const std::string name(argv[index]);
        if (name == "--help" || name == "-h") return false;
        if (index + 1 >= argc) return false;
        const char* value = argv[++index];
        if (name == "--key") {
            if (!ParseKey(value, &parsed.key)) return false;
        } else if (name == "--header") {
            parsed.header_path = value;
        } else if (name == "--metrics-json") {
            parsed.metrics_path = value;
        } else if (name == "--block-bytes") {
            if (!ParseUint64(value, &parsed.block_bytes)) return false;
        } else if (name == "--block-count") {
            if (!ParseUint64(value, &parsed.block_count)) return false;
        } else if (name == "--blocks-per-second") {
            if (!ParseUint64(value, &parsed.blocks_per_second)) return false;
        } else {
            return false;
        }
    }
    if (parsed.key == 0 || parsed.header_path.empty() ||
        parsed.metrics_path.empty() || parsed.block_bytes == 0U ||
        parsed.block_count == 0U || parsed.blocks_per_second == 0U) {
        return false;
    }
    *options = parsed;
    return true;
}

std::uint64_t MonotonicNs() {
    struct timespec value = {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

bool SleepUntil(std::uint64_t absolute_ns) {
    struct timespec deadline = {};
    deadline.tv_sec = static_cast<time_t>(absolute_ns / 1000000000ULL);
    deadline.tv_nsec = static_cast<long>(absolute_ns % 1000000000ULL);
    while (!stop_requested) {
        const int result = clock_nanosleep(
            CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        if (result == 0) return true;
        if (result != EINTR) return false;
    }
    return false;
}

std::vector<char> ReadFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return std::vector<char>();
    return std::vector<char>(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
}

bool WriteMetrics(const Options& options, std::uint64_t blocks,
                  std::uint64_t bytes, std::uint64_t elapsed_ns,
                  std::uint64_t acquire_wait_total,
                  std::uint64_t acquire_wait_max,
                  std::uint64_t publish_late_total,
                  std::uint64_t publish_late_max,
                  std::uint64_t initialized_blocks,
                  std::uint64_t initialization_bytes) {
    const double actual_gbps = elapsed_ns == 0U ? 0.0 :
        static_cast<double>(bytes) * 8.0 / static_cast<double>(elapsed_ns);
    std::ofstream output(options.metrics_path.c_str(), std::ios::trunc);
    if (!output) return false;
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"blocks_per_second\": " << options.blocks_per_second << ",\n"
           << "  \"planned_blocks\": " << options.block_count << ",\n"
           << "  \"published_blocks\": " << blocks << ",\n"
           << "  \"block_bytes\": " << options.block_bytes << ",\n"
           << "  \"published_bytes\": " << bytes << ",\n"
           << "  \"active_elapsed_ns\": " << elapsed_ns << ",\n"
           << "  \"actual_payload_gbps\": " << std::setprecision(17)
           << actual_gbps << ",\n"
           << "  \"ring_acquire_wait_ns_total\": "
           << acquire_wait_total << ",\n"
           << "  \"ring_acquire_wait_ns_max\": " << acquire_wait_max << ",\n"
           << "  \"publish_late_ns_total\": " << publish_late_total << ",\n"
           << "  \"publish_late_ns_max\": " << publish_late_max << ",\n"
           << "  \"initialized_ring_blocks\": " << initialized_blocks << ",\n"
           << "  \"initialization_bytes\": " << initialization_bytes << ",\n"
           << "  \"header_bytes\": 4096,\n"
           << "  \"eod_sent\": true\n"
           << "}\n";
    return static_cast<bool>(output);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        (std::strcmp(argv[1], "--help") == 0 ||
         std::strcmp(argv[1], "-h") == 0)) {
        Usage(argv[0]);
        return 0;
    }
    Options options = {};
    if (!ParseOptions(argc, argv, &options)) {
        Usage(argv[0]);
        return 2;
    }
    const std::vector<char> header = ReadFile(options.header_path);
    if (header.size() != 4096U) {
        std::fprintf(stderr, "input header must be exactly 4096 bytes\n");
        return 2;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    multilog_t* log = multilog_open("gpu_pressure_writer", 0);
    if (!log) return 1;
    multilog_add(log, stderr);
    dada_hdu_t* hdu = dada_hdu_create(log);
    bool connected = false;
    bool locked = false;
    int exit_code = 1;
    if (!hdu) goto cleanup;
    dada_hdu_set_key(hdu, options.key);
    if (dada_hdu_connect(hdu) < 0) goto cleanup;
    connected = true;
    if (dada_hdu_lock_write(hdu) < 0) goto cleanup;
    locked = true;
    if (ipcbuf_get_bufsz(reinterpret_cast<ipcbuf_t*>(hdu->data_block)) !=
        options.block_bytes) {
        std::fprintf(stderr, "compute ring block size does not match plan\n");
        goto cleanup;
    }
    if (ipcbuf_get_bufsz(hdu->header_block) != header.size()) {
        std::fprintf(stderr, "compute ring header size does not match input\n");
        goto cleanup;
    }
    {
        char* destination = ipcbuf_get_next_write(hdu->header_block);
        if (!destination) goto cleanup;
        std::memcpy(destination, &header[0], header.size());
        if (ipcbuf_enable_eod(hdu->header_block) < 0 ||
            ipcbuf_mark_filled(hdu->header_block, header.size()) < 0) {
            goto cleanup;
        }
    }

    {
        const std::uint64_t start_ns = MonotonicNs();
        std::uint64_t acquire_total = 0U;
        std::uint64_t acquire_max = 0U;
        std::uint64_t late_total = 0U;
        std::uint64_t late_max = 0U;
        std::uint64_t blocks = 0U;
        std::set<void*> initialized;
        for (std::uint64_t index = 0U;
             index < options.block_count && !stop_requested; ++index) {
            const std::uint64_t deadline = start_ns +
                rdma_dada::testing::ScheduledOffsetNs(
                    index, options.blocks_per_second);
            if (!SleepUntil(deadline)) break;
            const std::uint64_t acquire_start = MonotonicNs();
            std::uint64_t block_id = 0U;
            char* block = ipcio_open_block_write(hdu->data_block, &block_id);
            const std::uint64_t acquire_wait = MonotonicNs() - acquire_start;
            acquire_total += acquire_wait;
            if (acquire_wait > acquire_max) acquire_max = acquire_wait;
            if (!block) break;
            if (initialized.insert(block).second) {
                std::memset(block, 0, static_cast<std::size_t>(options.block_bytes));
            }
            std::memcpy(block, &index, sizeof(index));
            if (ipcio_close_block_write(hdu->data_block,
                                        options.block_bytes) < 0) {
                break;
            }
            ++blocks;
            const std::uint64_t published = MonotonicNs();
            const std::uint64_t late = published > deadline ?
                published - deadline : 0U;
            late_total += late;
            if (late > late_max) late_max = late;
        }
        const std::uint64_t end_deadline = start_ns +
            rdma_dada::testing::ScheduledOffsetNs(
                options.block_count, options.blocks_per_second);
        if (!stop_requested) (void)SleepUntil(end_deadline);
        const std::uint64_t end_ns = MonotonicNs();
        if (ipcio_stop(hdu->data_block) < 0) goto cleanup;
        if (blocks != options.block_count || stop_requested) {
            std::fprintf(stderr,
                         "pressure writer stopped after %" PRIu64
                         " of %" PRIu64 " blocks\n",
                         blocks, options.block_count);
            goto cleanup;
        }
        std::uint64_t bytes = 0U;
        if (blocks > std::numeric_limits<std::uint64_t>::max() /
                         options.block_bytes) {
            goto cleanup;
        }
        bytes = blocks * options.block_bytes;
        if (!WriteMetrics(
                options, blocks, bytes, end_ns - start_ns,
                acquire_total, acquire_max, late_total, late_max,
                initialized.size(),
                initialized.size() * options.block_bytes)) {
            std::fprintf(stderr, "cannot write pressure metrics\n");
            goto cleanup;
        }
        exit_code = 0;
    }

cleanup:
    if (locked && dada_hdu_unlock_write(hdu) < 0) exit_code = 1;
    if (connected && dada_hdu_disconnect(hdu) < 0) exit_code = 1;
    if (hdu) dada_hdu_destroy(hdu);
    multilog_close(log);
    return exit_code;
}
