#include "influx.h"
#include <stdio.h>
#include <string.h>
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

int influx_format_sample(char *out, size_t outsz,
                         const char *measurement,
                         const char *unit_id,
                         const d50_sample_t *s,
                         int64_t ts_unix) {
    int n;
    if (ts_unix > 0) {
        n = snprintf(out, outsz, "%s,unit=%s %s=%.1f,%s=%.1f %lld000000000",
                     measurement, unit_id,
                     s->ch1_name, s->ch1_value,
                     s->ch2_name, s->ch2_value,
                     (long long)ts_unix);
    } else {
        n = snprintf(out, outsz, "%s,unit=%s %s=%.1f,%s=%.1f",
                     measurement, unit_id,
                     s->ch1_name, s->ch1_value,
                     s->ch2_name, s->ch2_value);
    }
    if (n < 0 || (size_t)n >= outsz) return -1;
    return n;
}
