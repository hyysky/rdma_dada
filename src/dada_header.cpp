#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dada_header.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

//psrdada related includes
#include "futils.h"
#include "dada_def.h"
#include "ascii_header.h"

int read_dada_header_from_file(const char *dada_header_file_name, dada_header_t *dada_header){

  char *dada_header_buffer = (char *)malloc(DADA_DEFAULT_HEADER_SIZE);
  memset(dada_header_buffer, 0, DADA_DEFAULT_HEADER_SIZE);

  fileread(dada_header_file_name, dada_header_buffer, DADA_DEFAULT_HEADER_SIZE);
  read_dada_header(dada_header_buffer, dada_header);

  free(dada_header_buffer);

  return EXIT_SUCCESS;
}

int read_dada_header(const char *dada_header_buffer, dada_header_t *dada_header){


  if (ascii_header_get(dada_header_buffer, "NANT", "%d", &dada_header->nant) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting NANT, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_get(dada_header_buffer, "PKT_HEADER", "%d", &dada_header->pkt_header) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting PKT_HEADER, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_get(dada_header_buffer, "PKT_DATA", "%d", &dada_header->pkt_data) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting PKT_DATA, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_get(dada_header_buffer, "PKT_NSAMP", "%d", &dada_header->pkt_nsamp) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting PKT_NSAMP, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_get(dada_header_buffer, "PKT_TSAMP", "%lf", &dada_header->pkt_tsamp) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting PKT_TSAMP, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }


  if (ascii_header_get(dada_header_buffer, "PKT_NPOL", "%d", &dada_header->pkt_npol) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting NPOL, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_get(dada_header_buffer, "PKT_NBIT", "%d", &dada_header->pkt_nbit) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting NBIT, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_get(dada_header_buffer, "BYTES_PER_SECOND", "%d", &dada_header->bytes_per_second) < 0)  {
    fprintf(stderr, "WRITE_DADA_HEADER_ERROR: Error getting BYTES_PER_SECOND, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }


  return EXIT_SUCCESS;
}

int write_dada_header_to_file(const dada_header_t dada_header, const char *dada_header_file_name){

  FILE *fp = fopen(dada_header_file_name, "w");
  char *dada_header_buffer = (char *)malloc(DADA_DEFAULT_HEADER_SIZE);
  memset(dada_header_buffer, 0, DADA_DEFAULT_HEADER_SIZE);

  sprintf(dada_header_buffer, "HDR_VERSION  1.0\nHDR_SIZE     4096\n");
  write_dada_header(dada_header, dada_header_buffer);
  fprintf(fp, "%s\n", dada_header_buffer);

  free(dada_header_buffer);
  fclose(fp);

  return EXIT_SUCCESS;
}

int write_dada_header(const dada_header_t dada_header, char *dada_header_buffer){


  if (ascii_header_set(dada_header_buffer, "NANT", "%d", dada_header.nant) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting NANT, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }


  if (ascii_header_set(dada_header_buffer, "PKT_HEADER", "%d", dada_header.pkt_header) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting PKT_HEADER, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

if (ascii_header_set(dada_header_buffer, "PKT_DATA", "%d", dada_header.pkt_data) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting PKT_DATA, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_set(dada_header_buffer, "PKT_NSAMP", "%d", dada_header.pkt_nsamp) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting PKT_NSAMP, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

    if (ascii_header_set(dada_header_buffer, "PKT_TSAMP", "%f", dada_header.pkt_tsamp) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting PKT_TSAMP, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_set(dada_header_buffer, "PKT_NPOL", "%d", dada_header.pkt_npol) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting PKT_NPOL, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_set(dada_header_buffer, "PKT_NBIT", "%d", dada_header.pkt_nbit) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting PKT_NBIT, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

  if (ascii_header_set(dada_header_buffer, "BYTES_PER_SECOND", "%d", dada_header.bytes_per_second) < 0)  {
        fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting BYTES_PER_SECOND, "
                "which happens at %s, line [%d].\n",
                __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    if (ascii_header_set(dada_header_buffer, "FILE_SIZE",  "%" PRIu64 "", dada_header.filebytes) < 0)  {
    fprintf(stderr, "READ_DADA_HEADER_ERROR: Error setting PKT_NBIT, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }

    if (ascii_header_set(dada_header_buffer, "MJD_START", "%.15f", dada_header.mjd) < 0)  {
    fprintf(stderr, "UDP2DB_ERROR: Error setting MJD_START, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }
  
  if (ascii_header_set(dada_header_buffer, "UTC_START", "%s", dada_header.utc_start) < 0)  {
    fprintf(stderr, "UDP2DB_ERROR: Error setting UTC_START, "
            "which happens at %s, line [%d].\n",
            __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }
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