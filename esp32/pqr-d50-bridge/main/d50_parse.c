#include "d50_parse.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void copy_field(char *dst, size_t dstsz, const char *src) {
    char tmp[64];
    size_t i = 0;
    while (src[i] && src[i] != ',' && i < sizeof(tmp) - 1) { tmp[i] = src[i]; i++; }
    tmp[i] = 0;
    trim(tmp);
    strncpy(dst, tmp, dstsz - 1);
    dst[dstsz - 1] = 0;
}

bool d50_parse_ident(const char *buf, size_t len, d50_ident_t *out) {
    memset(out, 0, sizeof(*out));
    // Find "PQR <model> V<fw>" and "ID: <id>".
    const char *p = NULL;
    for (size_t i = 0; i + 4 <= len; i++)
        if (!memcmp(buf + i, "PQR ", 4)) { p = buf + i; break; }
    if (p) {
        p += 4;
        copy_field(out->model, sizeof(out->model), p); // model up to space
        // model copy stops at comma; also stop at space:
        for (char *m = out->model; *m; m++) if (*m == ' ') { *m = 0; break; }
        const char *v = strstr(p, " V");
        if (v) {
            v += 2; size_t i = 0;
            while (v[i] && (isdigit((unsigned char)v[i]) || v[i] == '.') &&
                   i < sizeof(out->firmware) - 1) { out->firmware[i] = v[i]; i++; }
            out->firmware[i] = 0;
        }
    }
    const char *id = NULL;
    for (size_t i = 0; i + 3 <= len; i++)
        if (!memcmp(buf + i, "ID:", 3)) { id = buf + i + 3; break; }
    if (id) {
        while (*id == ' ') id++;
        size_t i = 0;
        while (isdigit((unsigned char)id[i]) && i < sizeof(out->unit_id) - 1) {
            out->unit_id[i] = id[i]; i++;
        }
        out->unit_id[i] = 0;
    }
    return out->model[0] && out->unit_id[0];
}

// Pull the next CR/LF-delimited line into `line` (NUL-terminated). Advances *i.
// Returns the line length (0 for a blank line that was skipped).
static size_t next_line(const char *buf, size_t len, size_t *i,
                        char *line, size_t linesz) {
    size_t j = 0;
    while (*i < len && buf[*i] != '\r' && buf[*i] != '\n' && j < linesz - 1)
        line[j++] = buf[(*i)++];
    line[j] = 0;
    while (*i < len && (buf[*i] == '\r' || buf[*i] == '\n')) (*i)++;
    return j;
}

static int count_commas(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s == ',') n++;
    return n;
}

// Split `line` into up to `maxf` comma fields. Returns fields filled.
static int split_fields(const char *line, char fields[][48], int maxf) {
    int fi = 0;
    const char *f = line;
    while (fi < maxf) {
        copy_field(fields[fi], 48, f);
        fi++;
        const char *nx = strchr(f, ',');
        if (!nx) break;
        f = nx + 1;
    }
    return fi;
}

size_t d50_parse_detail(const char *buf, size_t len,
                        d50_event_t *events, size_t max) {
    size_t count = 0, i = 0;
    char line[160];
    while (i < len && count < max) {
        if (next_line(buf, len, &i, line, sizeof(line)) == 0) continue;
        if (strstr(line, "PowerTronics")) continue;
        if (count_commas(line) < 4) continue;        // need 5 fields
        char fields[5][48];
        if (split_fields(line, fields, 5) < 5) continue;
        d50_event_t e; memset(&e, 0, sizeof(e));
        strncpy(e.date, fields[0], sizeof(e.date) - 1);
        strncpy(e.time, fields[1], sizeof(e.time) - 1);
        strncpy(e.channel, fields[2], sizeof(e.channel) - 1);
        strncpy(e.event_type, fields[3], sizeof(e.event_type) - 1);
        e.magnitude = atof(fields[4]);
        e.valid = true;
        events[count++] = e;
    }
    return count;
}

size_t d50_parse_datalog(const char *buf, size_t len,
                         d50_sample_t *samples, size_t max) {
    size_t count = 0, i = 0;
    char line[160];
    while (i < len && count < max) {
        if (next_line(buf, len, &i, line, sizeof(line)) == 0) continue;
        if (strstr(line, "PowerTronics")) continue;   // banner
        if (count_commas(line) < 5) continue;          // need 6 fields
        char fields[6][48];
        if (split_fields(line, fields, 6) < 6) continue;
        d50_sample_t s; memset(&s, 0, sizeof(s));
        strncpy(s.date, fields[0], sizeof(s.date) - 1);
        strncpy(s.time, fields[1], sizeof(s.time) - 1);
        strncpy(s.ch1_name, fields[2], sizeof(s.ch1_name) - 1);
        s.ch1_value = atof(fields[3]);
        strncpy(s.ch2_name, fields[4], sizeof(s.ch2_name) - 1);
        s.ch2_value = atof(fields[5]);
        s.valid = true;
        samples[count++] = s;
    }
    return count;
}
