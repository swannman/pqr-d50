// Portable (no-ESP-deps) parser for PQR D50 ASCII reports.
// Mirrors pqr_d50/parsers.py; unit-testable on a host with gcc/clang.
#ifndef D50_PARSE_H
#define D50_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D50_MAXCH 8     // labels like "Hot"/"Neu" are short
#define D50_MAXTS 24    // "Jun/21/26, 11:28:05"

// One Data Log (C4) sample: "date, time, ch1, v1, ch2, v2,"
typedef struct {
    char date[12];
    char time[12];
    char ch1_name[D50_MAXCH];
    double ch1_value;
    char ch2_name[D50_MAXCH];
    double ch2_value;
    bool valid;
} d50_sample_t;

// One Detail Report (C3) event: "date, time, channel, event, magnitude,"
typedef struct {
    char date[12];
    char time[12];
    char channel[D50_MAXCH];
    char event_type[24];
    double magnitude;
    bool valid;
} d50_event_t;

// Parsed identity banner fields.
typedef struct {
    char model[16];
    char firmware[12];
    char unit_id[16];
} d50_ident_t;

// Parse the identity banner (C1 reply). Returns true if model+id found.
bool d50_parse_ident(const char *buf, size_t len, d50_ident_t *out);

// Parse a Data Log (C4) buffer into samples[]. Returns the count written
// (<= max). Skips the banner and any non-data lines.
size_t d50_parse_datalog(const char *buf, size_t len,
                         d50_sample_t *samples, size_t max);

// Parse a Detail Report (C3) buffer into events[]. Returns the count written.
size_t d50_parse_detail(const char *buf, size_t len,
                        d50_event_t *events, size_t max);

#ifdef __cplusplus
}
#endif
#endif
