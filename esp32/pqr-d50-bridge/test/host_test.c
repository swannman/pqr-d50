// Host test for the portable D50 parser + Influx formatter.
// Build:  cc -I../main host_test.c ../main/d50_parse.c ../main/influx.c -o host_test
// Run:    ./host_test <a-real.dlg>   (or with no arg, uses a built-in sample)
#include "d50_parse.h"
#include "influx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SAMPLE =
    "\n\rPowerTronics PQR D50 V20.55   - Data Log Report as of Jun/21/26 - ID: 086101\r\r\n\r\n"
    "Jun/10/26, 13:56:34, Hot, 108.9, Neu, 0.6,\n\r"
    "Jun/21/26, 11:28:05, Hot, 121.1, Neu, 0.0,\n\r"
    "Jun/21/26, 11:29:05, Hot, 121.1, Neu, 0.1,\n\r";

int main(int argc, char **argv) {
    char *buf; size_t len;
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror("open"); return 1; }
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc(len + 1); fread(buf, 1, len, f); buf[len] = 0; fclose(f);
    } else {
        buf = strdup(SAMPLE); len = strlen(buf);
    }

    d50_ident_t id;
    if (d50_parse_ident(buf, len, &id))
        printf("ident: model=%s fw=%s id=%s\n", id.model, id.firmware, id.unit_id);
    else
        printf("ident: (no banner / not an identity reply)\n");

    d50_sample_t samples[256];
    size_t n = d50_parse_datalog(buf, len, samples, 256);
    printf("parsed %zu data-log samples\n", n);

    int fails = 0;
    char line[160];
    for (size_t i = 0; i < n; i++) {
        d50_sample_t *s = &samples[i];
        int64_t ts = d50_timestamp_to_unix(s->date, s->time);
        if (influx_format_sample(line, sizeof(line), "pqr_d50",
                                 id.unit_id[0] ? id.unit_id : "unknown",
                                 s, ts) < 0) { fails++; continue; }
        printf("  %s\n", line);
        if (ts == 0) { printf("    !! timestamp parse failed\n"); fails++; }
        if (s->ch1_value < 0 || s->ch1_value > 1000) fails++;
    }
    free(buf);
    printf(fails ? "\nFAIL (%d)\n" : "\nOK\n", fails);
    return fails ? 1 : 0;
}
