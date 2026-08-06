#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rdma_dada/io/psrdada/header_codec.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

//psrdada related includes
#include "futils.h"
#include "dada_def.h"
#include "ascii_header.h"

int read_dada_header_from_file(const char *dada_header_file_name, dada_header_t *dada_header){

  if (!dada_header_file_name || !dada_header) return EXIT_FAILURE;

  char *dada_header_buffer = (char *)malloc(DADA_DEFAULT_HEADER_SIZE);
  if (!dada_header_buffer) return EXIT_FAILURE;
  memset(dada_header_buffer, 0, DADA_DEFAULT_HEADER_SIZE);

  if (fileread(dada_header_file_name, dada_header_buffer, DADA_DEFAULT_HEADER_SIZE) < 0) {
    free(dada_header_buffer);
    return EXIT_FAILURE;
  }
  int result = read_dada_header(dada_header_buffer, dada_header);

  free(dada_header_buffer);

  return result;
}

int read_dada_header(const char *dada_header_buffer, dada_header_t *dada_header){
  if (!dada_header_buffer || !dada_header) return EXIT_FAILURE;
  dada_header_t parsed = {};

#define GET_HEADER_FIELD(KEY, FORMAT, POINTER)                               \
  do {                                                                        \
    if (ascii_header_get(dada_header_buffer, KEY, FORMAT, POINTER) != 1) {    \
      fprintf(stderr, "DADA_HEADER_ERROR: missing or invalid %s\n", KEY);     \
      return EXIT_FAILURE;                                                    \
    }                                                                         \
  } while (0)

  GET_HEADER_FIELD("PIPELINE_VERSION", "%" SCNu32, &parsed.pipeline_version);
  GET_HEADER_FIELD("DATA_STAGE", "%15s", parsed.data_stage);
  GET_HEADER_FIELD("UTC_START", "%1023s", parsed.utc_start);
  GET_HEADER_FIELD("MJD_START", "%lf", &parsed.mjd);
  GET_HEADER_FIELD("NANT", "%" SCNu32, &parsed.nant);
  GET_HEADER_FIELD("NCHAN", "%" SCNu32, &parsed.nchan);
  GET_HEADER_FIELD("NPOL", "%" SCNu32, &parsed.npol);
  GET_HEADER_FIELD("NBIT", "%" SCNu32, &parsed.nbit);
  GET_HEADER_FIELD("ORDER", "%1023s", parsed.order);
  GET_HEADER_FIELD("PKT_HEADER", "%" SCNu64, &parsed.pkt_header);
  GET_HEADER_FIELD("PKT_DATA", "%" SCNu64, &parsed.pkt_data);
  GET_HEADER_FIELD("PKT_NSAMP", "%" SCNu64, &parsed.pkt_nsamp);
  GET_HEADER_FIELD("PKT_TSAMP", "%lf", &parsed.pkt_tsamp);
  GET_HEADER_FIELD("RECORD_HEADER_BYTES", "%" SCNu64,
                   &parsed.record_header_bytes);
  GET_HEADER_FIELD("RECORD_BYTES", "%" SCNu64, &parsed.record_bytes);
  GET_HEADER_FIELD("RESOLUTION", "%" SCNu64, &parsed.resolution);
  GET_HEADER_FIELD("BYTES_PER_SECOND", "%" SCNu64,
                   &parsed.bytes_per_second);
  GET_HEADER_FIELD("RAW_BYTES_PER_SECOND", "%" SCNu64,
                   &parsed.raw_bytes_per_second);
  GET_HEADER_FIELD("FILE_SIZE", "%" SCNu64, &parsed.filebytes);

#undef GET_HEADER_FIELD

  if (parsed.pipeline_version != DADA_PIPELINE_CONTRACT_VERSION) {
    fprintf(stderr, "DADA_HEADER_ERROR: unsupported PIPELINE_VERSION %" PRIu32 "\n",
            parsed.pipeline_version);
    return EXIT_FAILURE;
  }
  if (parsed.nbit != 16) {
    fprintf(stderr, "DADA_HEADER_ERROR: contract version 1 requires NBIT=16\n");
    return EXIT_FAILURE;
  }
  if (parsed.pkt_header != 32) {
    fprintf(stderr,
            "DADA_HEADER_ERROR: Project VDIF v1 requires PKT_HEADER=32\n");
    return EXIT_FAILURE;
  }
  if (strcmp(parsed.data_stage, "RAW") == 0) {
    if (strcmp(parsed.order, "TFP") != 0 ||
        parsed.record_header_bytes != 32 ||
        parsed.record_bytes != parsed.pkt_header + parsed.pkt_data ||
        parsed.resolution != parsed.record_bytes) {
      fprintf(stderr, "DADA_HEADER_ERROR: inconsistent RAW record metadata\n");
      return EXIT_FAILURE;
    }
  } else if (strcmp(parsed.data_stage, "COMPUTE") == 0) {
    if (strcmp(parsed.order, "TFPA") != 0 ||
        parsed.record_header_bytes != 0 || parsed.record_bytes == 0 ||
        parsed.resolution != parsed.record_bytes) {
      fprintf(stderr, "DADA_HEADER_ERROR: inconsistent COMPUTE record metadata\n");
      return EXIT_FAILURE;
    }
  } else {
    fprintf(stderr, "DADA_HEADER_ERROR: unsupported DATA_STAGE %s\n",
            parsed.data_stage);
    return EXIT_FAILURE;
  }

  *dada_header = parsed;
  return EXIT_SUCCESS;
}

int write_dada_header_to_file(const dada_header_t dada_header, const char *dada_header_file_name){

  if (!dada_header_file_name) return EXIT_FAILURE;
  FILE *fp = fopen(dada_header_file_name, "w");
  if (!fp) return EXIT_FAILURE;
  char *dada_header_buffer = (char *)malloc(DADA_DEFAULT_HEADER_SIZE);
  if (!dada_header_buffer) {
    fclose(fp);
    return EXIT_FAILURE;
  }
  memset(dada_header_buffer, 0, DADA_DEFAULT_HEADER_SIZE);

  snprintf(dada_header_buffer, DADA_DEFAULT_HEADER_SIZE,
           "HDR_VERSION  1.0\nHDR_SIZE     %" PRIu64 "\n",
           (uint64_t)DADA_DEFAULT_HEADER_SIZE);
  if (write_dada_header(dada_header, dada_header_buffer) < 0) {
    free(dada_header_buffer);
    fclose(fp);
    return EXIT_FAILURE;
  }
  fprintf(fp, "%s\n", dada_header_buffer);

  free(dada_header_buffer);
  fclose(fp);

  return EXIT_SUCCESS;
}

int write_dada_header(const dada_header_t dada_header, char *dada_header_buffer){
  if (!dada_header_buffer) return EXIT_FAILURE;

#define SET_HEADER_FIELD(KEY, FORMAT, VALUE)                                 \
  do {                                                                        \
    if (ascii_header_set(dada_header_buffer, KEY, FORMAT, VALUE) < 0) {       \
      fprintf(stderr, "DADA_HEADER_ERROR: could not set %s\n", KEY);          \
      return EXIT_FAILURE;                                                    \
    }                                                                         \
  } while (0)

  SET_HEADER_FIELD("PIPELINE_VERSION", "%" PRIu32, dada_header.pipeline_version);
  SET_HEADER_FIELD("DATA_STAGE", "%s", dada_header.data_stage);
  SET_HEADER_FIELD("UTC_START", "%s", dada_header.utc_start);
  SET_HEADER_FIELD("MJD_START", "%.15f", dada_header.mjd);
  SET_HEADER_FIELD("NANT", "%" PRIu32, dada_header.nant);
  SET_HEADER_FIELD("NCHAN", "%" PRIu32, dada_header.nchan);
  SET_HEADER_FIELD("NPOL", "%" PRIu32, dada_header.npol);
  SET_HEADER_FIELD("NBIT", "%" PRIu32, dada_header.nbit);
  SET_HEADER_FIELD("ORDER", "%s", dada_header.order);
  SET_HEADER_FIELD("PKT_HEADER", "%" PRIu64, dada_header.pkt_header);
  SET_HEADER_FIELD("PKT_DATA", "%" PRIu64, dada_header.pkt_data);
  SET_HEADER_FIELD("PKT_NSAMP", "%" PRIu64, dada_header.pkt_nsamp);
  SET_HEADER_FIELD("PKT_TSAMP", "%.15g", dada_header.pkt_tsamp);
  SET_HEADER_FIELD("PKT_NPOL", "%" PRIu32, dada_header.npol);
  SET_HEADER_FIELD("PKT_NBIT", "%" PRIu32, dada_header.nbit);
  SET_HEADER_FIELD("TSAMP", "%.15g", dada_header.pkt_tsamp);
  SET_HEADER_FIELD("RECORD_HEADER_BYTES", "%" PRIu64,
                   dada_header.record_header_bytes);
  SET_HEADER_FIELD("RECORD_BYTES", "%" PRIu64, dada_header.record_bytes);
  SET_HEADER_FIELD("RESOLUTION", "%" PRIu64, dada_header.resolution);
  SET_HEADER_FIELD("BYTES_PER_SECOND", "%" PRIu64,
                   dada_header.bytes_per_second);
  SET_HEADER_FIELD("PAYLOAD_BYTES_PER_SECOND", "%" PRIu64,
                   dada_header.bytes_per_second);
  SET_HEADER_FIELD("RAW_BYTES_PER_SECOND", "%" PRIu64,
                   dada_header.raw_bytes_per_second);
  SET_HEADER_FIELD("FILE_SIZE", "%" PRIu64, dada_header.filebytes);

#undef SET_HEADER_FIELD
  return EXIT_SUCCESS;
}

// Calculate Modified Julian Date from Gregorian calendar
// MJD = JD - 2400000.5, where JD is Julian Date
static double gregorian_calendar_to_mjd(int year, int month, int day) {
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;

    // Julian Day Number calculation
    int jdn = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;

    // Convert JDN to MJD (MJD = JD - 2400000.5, and JD = JDN + 0.5 for midnight)
    double mjd = jdn - 2400001;  // = (jdn + 0.5) - 2400000.5 - 0.5

    return mjd;
}

// Get current MJD with fractional day (includes time)
double get_current_mjd() {
    time_t tmi;
    time(&tmi);
    struct tm* utc = gmtime(&tmi);

    int year = utc->tm_year + 1900;
    int mon  = utc->tm_mon + 1;
    int mday = utc->tm_mday;

    // Calculate MJD for the date (midnight)
    double mjd = gregorian_calendar_to_mjd(year, mon, mday);

    // Add fractional day for the time (hours, minutes, seconds)
    double day_fraction = (utc->tm_hour + utc->tm_min / 60.0 + utc->tm_sec / 3600.0) / 24.0;

    return mjd + day_fraction;
}

// Get current UTC time in format: YYYY-MM-DD-HH:MM:SS
// buffer should be at least 20 characters
void get_current_utc(char* buffer, size_t buffer_size) {
    time_t tmi;
    time(&tmi);
    struct tm* utc = gmtime(&tmi);

    snprintf(buffer, buffer_size, "%04d-%02d-%02d-%02d:%02d:%02d",
             utc->tm_year + 1900,
             utc->tm_mon + 1,
             utc->tm_mday,
             utc->tm_hour,
             utc->tm_min,
             utc->tm_sec);
}
