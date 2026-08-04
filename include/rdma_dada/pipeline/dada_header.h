#pragma once

#define DADA_STRLEN 1024
#define DADA_PIPELINE_CONTRACT_VERSION 1

#include <cstdint>

typedef struct dada_header_t {
    char utc_start[DADA_STRLEN];
    char data_stage[16];
    char order[DADA_STRLEN];
    uint32_t pipeline_version;
    double mjd;
    uint32_t nant;
    uint32_t nchan;
    uint32_t npol;
    uint32_t nbit;
    uint64_t pkt_header;
    uint64_t pkt_data;
    uint64_t pkt_nsamp;
    double pkt_tsamp;
    uint64_t record_header_bytes;
    uint64_t record_bytes;
    uint64_t resolution;
    uint64_t bytes_per_second;
    uint64_t raw_bytes_per_second;
    uint64_t filebytes;
} dada_header_t;
