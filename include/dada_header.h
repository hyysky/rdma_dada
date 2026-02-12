#ifndef __DADA_HEADER_H
#define __DADA_HEADER_H

#define DADA_STRLEN 1024

#ifdef __cplusplus
extern "C" {
#endif

#include "inttypes.h"
#include <stddef.h>

  typedef struct dada_header_t{
    char utc_start[DADA_STRLEN];
    uint64_t mjd;
    int nant;
    int pkt_header;
    int pkt_data;
    int pkt_nsamp;
    double pkt_tsamp;
    int pkt_npol;
    int pkt_nbit;
    int bytes_per_second;
    uint64_t filebytes;
  }dada_header_t;

  int read_dada_header(const char *dada_header_buffer, dada_header_t *dada_header);

  int write_dada_header(const dada_header_t dada_header, char *dada_header_buffer);

  int read_dada_header_from_file(const char *dada_header_file_name, dada_header_t *dada_header);

  int write_dada_header_to_file(const dada_header_t dada_header,const char *dada_header_file_name);

  double get_current_mjd();

  void get_current_utc(char* buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif
