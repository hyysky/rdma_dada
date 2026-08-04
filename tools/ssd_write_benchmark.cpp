// Standalone SSD diagnostic; not a pipeline application.
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <string>

struct BenchmarkConfig {
    std::string output_path = "./ssd_test.bin";
    uint64_t total_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;  // 4 GiB
    uint64_t block_bytes = 4ULL * 1024ULL * 1024ULL;            // 4 MiB
    uint32_t pattern_byte = 0xA5;
    uint64_t fdatasync_interval_blocks = 0;
    bool use_direct = false;
    bool fsync_at_end = true;
    bool keep_file = false;
};

static void PrintUsage(const char *prog) {
    printf("SSD write benchmark (sequential write throughput)\n\n");
    printf("Usage:\n");
    printf("  %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --file <path>                 Output file path (default: ./ssd_test.bin)\n");
    printf("  --size-gb <num>               Total write size in GiB (default: 4)\n");
    printf("  --size-mb <num>               Total write size in MiB (overrides --size-gb)\n");
    printf("  --block-kb <num>              Block size in KiB (default: 4096)\n");
    printf("  --pattern <0-255>             Fill byte value (default: 165)\n");
    printf("  --fdatasync-interval <num>    Call fdatasync every N blocks (default: 0 = disabled)\n");
    printf("  --direct                      Use O_DIRECT (block size should align to 4096)\n");
    printf("  --no-fsync                    Skip fsync at benchmark end\n");
    printf("  --keep-file                   Keep generated test file\n");
    printf("  -h, --help                    Show this help\n");
}

static bool ParseArgs(int argc, char **argv, BenchmarkConfig *cfg) {
    static struct option long_options[] = {
        {"file", required_argument, NULL, 1},
        {"size-gb", required_argument, NULL, 2},
        {"size-mb", required_argument, NULL, 3},
        {"block-kb", required_argument, NULL, 4},
        {"pattern", required_argument, NULL, 5},
        {"fdatasync-interval", required_argument, NULL, 6},
        {"direct", no_argument, NULL, 7},
        {"no-fsync", no_argument, NULL, 8},
        {"keep-file", no_argument, NULL, 9},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int c = 0;
    while ((c = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        switch (c) {
            case 1:
                cfg->output_path = optarg;
                break;
            case 2: {
                uint64_t gb = strtoull(optarg, NULL, 10);
                cfg->total_bytes = gb * 1024ULL * 1024ULL * 1024ULL;
                break;
            }
            case 3: {
                uint64_t mb = strtoull(optarg, NULL, 10);
                cfg->total_bytes = mb * 1024ULL * 1024ULL;
                break;
            }
            case 4: {
                uint64_t kb = strtoull(optarg, NULL, 10);
                cfg->block_bytes = kb * 1024ULL;
                break;
            }
            case 5: {
                unsigned int p = (unsigned int)strtoul(optarg, NULL, 10);
                if (p > 255U) {
                    fprintf(stderr, "--pattern must be in [0, 255]\n");
                    return false;
                }
                cfg->pattern_byte = p;
                break;
            }
            case 6:
                cfg->fdatasync_interval_blocks = strtoull(optarg, NULL, 10);
                break;
            case 7:
                cfg->use_direct = true;
                break;
            case 8:
                cfg->fsync_at_end = false;
                break;
            case 9:
                cfg->keep_file = true;
                break;
            case 'h':
                PrintUsage(argv[0]);
                return false;
            default:
                PrintUsage(argv[0]);
                return false;
        }
    }

    if (cfg->total_bytes == 0) {
        fprintf(stderr, "Total size must be greater than 0\n");
        return false;
    }
    if (cfg->block_bytes == 0) {
        fprintf(stderr, "Block size must be greater than 0\n");
        return false;
    }

    if (cfg->use_direct) {
        if (cfg->block_bytes % 4096ULL != 0) {
            fprintf(stderr, "When using --direct, --block-kb must align to 4096 bytes\n");
            return false;
        }
    }

    return true;
}

static double SecondsSince(const struct timespec &start, const struct timespec &end) {
    const int64_t sec = (int64_t)end.tv_sec - (int64_t)start.tv_sec;
    const int64_t nsec = (int64_t)end.tv_nsec - (int64_t)start.tv_nsec;
    return (double)sec + (double)nsec / 1e9;
}

static int SyncFileData(int fd) {
#ifdef __APPLE__
    // macOS does not expose fdatasync; fsync provides the required fallback.
    return fsync(fd);
#else
    return fdatasync(fd);
#endif
}

int main(int argc, char **argv) {
    BenchmarkConfig cfg;
    if (!ParseArgs(argc, argv, &cfg)) {
        return 1;
    }

    int flags = O_CREAT | O_TRUNC | O_WRONLY;
    if (cfg.use_direct) {
#ifdef O_DIRECT
        flags |= O_DIRECT;
#else
        fprintf(stderr, "--direct is not supported on this platform\n");
        return 1;
#endif
    }

    int fd = open(cfg.output_path.c_str(), flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", cfg.output_path.c_str(), strerror(errno));
        return 1;
    }

    void *buf = NULL;
    const size_t alignment = cfg.use_direct ? 4096 : 64;
    if (posix_memalign(&buf, alignment, (size_t)cfg.block_bytes) != 0 || buf == NULL) {
        fprintf(stderr, "posix_memalign failed\n");
        close(fd);
        return 1;
    }
    memset(buf, (int)cfg.pattern_byte, (size_t)cfg.block_bytes);

    printf("Benchmark config:\n");
    printf("  file: %s\n", cfg.output_path.c_str());
    printf("  total size: %.2f GiB (%llu bytes)\n",
           (double)cfg.total_bytes / 1024.0 / 1024.0 / 1024.0,
           (unsigned long long)cfg.total_bytes);
    printf("  block size: %.2f MiB (%llu bytes)\n",
           (double)cfg.block_bytes / 1024.0 / 1024.0,
           (unsigned long long)cfg.block_bytes);
    printf("  direct IO: %s\n", cfg.use_direct ? "ON" : "OFF");
    printf("  fdatasync interval: %llu blocks\n",
           (unsigned long long)cfg.fdatasync_interval_blocks);
    printf("  fsync at end: %s\n", cfg.fsync_at_end ? "ON" : "OFF");

    uint64_t total_written = 0;
    uint64_t block_count = 0;
    struct timespec t0 = {0, 0};
    struct timespec t1 = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (total_written < cfg.total_bytes) {
        uint64_t to_write = cfg.block_bytes;
        const uint64_t remaining = cfg.total_bytes - total_written;
        if (remaining < to_write) {
            to_write = remaining;
        }

        uint64_t wrote_this_round = 0;
        while (wrote_this_round < to_write) {
            ssize_t w = write(fd,
                              (const char *)buf + wrote_this_round,
                              (size_t)(to_write - wrote_this_round));
            if (w < 0) {
                fprintf(stderr, "write failed at %llu bytes: %s\n",
                        (unsigned long long)total_written, strerror(errno));
                free(buf);
                close(fd);
                return 1;
            }
            wrote_this_round += (uint64_t)w;
        }

        total_written += wrote_this_round;
        block_count++;

        if (cfg.fdatasync_interval_blocks > 0 &&
            (block_count % cfg.fdatasync_interval_blocks) == 0) {
            if (SyncFileData(fd) != 0) {
                fprintf(stderr, "data sync failed: %s\n", strerror(errno));
                free(buf);
                close(fd);
                return 1;
            }
        }
    }

    if (cfg.fsync_at_end) {
        if (fsync(fd) != 0) {
            fprintf(stderr, "fsync failed: %s\n", strerror(errno));
            free(buf);
            close(fd);
            return 1;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double seconds = SecondsSince(t0, t1);
    const double mib = (double)total_written / 1024.0 / 1024.0;
    const double gib = (double)total_written / 1024.0 / 1024.0 / 1024.0;
    const double gbits = (double)total_written * 8.0 / 1e9;

    printf("\nResult:\n");
    printf("  bytes written: %llu\n", (unsigned long long)total_written);
    printf("  time: %.3f s\n", seconds);
    if (seconds > 0.0) {
        printf("  throughput: %.2f MiB/s (%.2f GiB/s, %.2f Gbps)\n",
               mib / seconds,
               gib / seconds,
               gbits / seconds);
    }

    free(buf);
    close(fd);

    if (!cfg.keep_file) {
        if (unlink(cfg.output_path.c_str()) != 0) {
            fprintf(stderr, "warning: failed to remove %s: %s\n",
                    cfg.output_path.c_str(), strerror(errno));
        }
    }

    return 0;
}
