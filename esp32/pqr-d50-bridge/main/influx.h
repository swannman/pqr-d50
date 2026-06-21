// Portable InfluxDB line-protocol formatting for D50 samples.
// HTTP transport lives in main.cpp (ESP-only); this part is host-testable.
#ifndef INFLUX_H
#define INFLUX_H

#include "d50_parse.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build one Influx line-protocol record for a data-log sample:
//   pqr_d50,unit=<id> hot=<v>,neu=<v> <ts_ns>
// `ts_unix` is the sample's epoch seconds (0 => omit timestamp, server stamps).
// Returns the number of chars written (excluding NUL), or -1 on truncation.
int influx_format_sample(char *out, size_t outsz,
                         const char *measurement,
                         const char *unit_id,
                         const d50_sample_t *s,
                         int64_t ts_unix);

// Convert a D50 "Mon/DD/YY" + "HH:MM:SS[.ff]" pair to epoch seconds (UTC-naive,
// i.e. treated as local wall-clock). Returns 0 on parse failure.
int64_t d50_timestamp_to_unix(const char *date, const char *time);

#ifdef __cplusplus
}
#endif
#endif
