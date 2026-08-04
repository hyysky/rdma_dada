#pragma once

#include "rdma_dada/pipeline/dada_header.h"

#include <cstddef>

// Conversion between the portable pipeline header and PSRDADA ASCII blocks.
// This adapter belongs to the Linux IO layer and requires PSRDADA.
int read_dada_header(const char* dada_header_buffer,
                     dada_header_t* dada_header);
int write_dada_header(dada_header_t dada_header, char* dada_header_buffer);
int read_dada_header_from_file(const char* dada_header_file_name,
                               dada_header_t* dada_header);
int write_dada_header_to_file(dada_header_t dada_header,
                              const char* dada_header_file_name);
double get_current_mjd();
void get_current_utc(char* buffer, std::size_t buffer_size);
