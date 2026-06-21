#include "influx.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static int month_num(const char *mon) {
    static const char *names[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++)
        if (!strncmp(mon, names[i], 3)) return i;
    return -1;
}

int64_t d50_timestamp_to_unix(const char *date, const char *time) {
    // date: "Mon/DD/YY"   time: "HH:MM:SS" or "HH:MM:SS.ff"
    char mon[4] = {0};
    int dd = 0, yy = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(date, "%3[A-Za-z]/%d/%d", mon, &dd, &yy) != 3) return 0;
    if (sscanf(time, "%d:%d:%d", &hh, &mm, &ss) < 3) return 0;
    int mi = month_num(mon);
    if (mi < 0) return 0;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = (yy < 70 ? 2000 + yy : 1900 + yy) - 1900;
    tmv.tm_mon = mi;
    tmv.tm_mday = dd;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = ss;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);          // interprets as local time
    return (t == (time_t)-1) ? 0 : (int64_t)t;
}

void influx_normalize_label(char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        *s = isalnum(c) ? (char)tolower(c) : '_';
    }
}

// lowercase a short field-key copy (e.g. "Hot" -> "hot") for clean metric names
static void lc_key(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    for (; src[i] && i < dstsz - 1; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

int influx_format_sample(char *out, size_t outsz,
                         const char *measurement,
                         const char *unit_id,
                         const d50_sample_t *s,
                         int64_t ts_unix) {
    char k1[D50_MAXCH], k2[D50_MAXCH];
    lc_key(k1, sizeof(k1), s->ch1_name);   // "hot"
    lc_key(k2, sizeof(k2), s->ch2_name);   // "neu"
    int n;
    if (ts_unix > 0)
        n = snprintf(out, outsz, "%s,unit=%s %s=%.1f,%s=%.1f %lld000000000",
                     measurement, unit_id, k1, s->ch1_value, k2, s->ch2_value,
                     (long long)ts_unix);
    else
        n = snprintf(out, outsz, "%s,unit=%s %s=%.1f,%s=%.1f",
                     measurement, unit_id, k1, s->ch1_value, k2, s->ch2_value);
    if (n < 0 || (size_t)n >= outsz) return -1;
    return n;
}

int influx_format_event(char *out, size_t outsz,
                        const char *measurement,
                        const char *unit_id,
                        const d50_event_t *e,
                        int64_t ts_unix) {
    char type[24];
    strncpy(type, e->event_type, sizeof(type) - 1);
    type[sizeof(type) - 1] = 0;
    influx_normalize_label(type);
    int n;
    if (ts_unix > 0)
        n = snprintf(out, outsz,
                     "%s,unit=%s,type=%s magnitude=%.1f %lld000000000",
                     measurement, unit_id, type, e->magnitude, (long long)ts_unix);
    else
        n = snprintf(out, outsz, "%s,unit=%s,type=%s magnitude=%.1f",
                     measurement, unit_id, type, e->magnitude);
    if (n < 0 || (size_t)n >= outsz) return -1;
    return n;
}
