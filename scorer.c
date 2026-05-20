/*
 * scorer.c  —  Phase 3 external scorer program
 *
 * Usage:  scorer <district>
 *
 * Reads <district>/reports.dat (binary Report records), aggregates the
 * severity sum per inspector, and prints a plain-text workload summary to
 * stdout.  city_hub redirects stdout to a pipe via dup2() before exec().
 *
 * Output format (one line per inspector, sorted by score descending):
 *   SCORE <district> <inspector> <total_severity> <report_count>
 * followed by a terminator line:
 *   SCORE_DONE <district>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* ── must match city_manager.c ─────────────────────────────────────────── */
typedef struct {
    int    id;
    char   inspector[64];
    double lat;
    double lon;
    char   category[32];
    int    severity;
    time_t timestamp;
    char   description[80];
} Report;

/* ── per-inspector accumulator ─────────────────────────────────────────── */
#define MAX_INSPECTORS 256

typedef struct {
    char name[64];
    int  total_severity;
    int  report_count;
} InspectorScore;

static InspectorScore scores[MAX_INSPECTORS];
static int            num_inspectors = 0;

static InspectorScore *find_or_create(const char *name) {
    for (int i = 0; i < num_inspectors; i++) {
        if (strcmp(scores[i].name, name) == 0)
            return &scores[i];
    }
    if (num_inspectors >= MAX_INSPECTORS) return NULL;
    InspectorScore *s = &scores[num_inspectors++];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    s->total_severity = 0;
    s->report_count   = 0;
    return s;
}

/* ── comparison for qsort (descending total_severity) ───────────────────── */
static int cmp_score(const void *a, const void *b) {
    const InspectorScore *sa = (const InspectorScore *)a;
    const InspectorScore *sb = (const InspectorScore *)b;
    return sb->total_severity - sa->total_severity;   /* descending */
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: scorer <district>\n");
        return 1;
    }
    const char *district = argv[1];

    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    int fd = open(dat_path, O_RDONLY);
    if (fd < 0) {
        /* District may be empty or not yet created — output a clear message */
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
                           "SCORE_ERROR %s reports.dat not found\n", district);
        write(STDOUT_FILENO, buf, len);
        return 1;
    }

    Report r;
    while (read(fd, &r, sizeof(Report)) == (ssize_t)sizeof(Report)) {
        InspectorScore *s = find_or_create(r.inspector);
        if (s) {
            s->total_severity += r.severity;
            s->report_count++;
        }
    }
    close(fd);

    /* Sort descending by workload score */
    qsort(scores, num_inspectors, sizeof(InspectorScore), cmp_score);

    /* Emit one line per inspector then a terminator */
    for (int i = 0; i < num_inspectors; i++) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
                           "SCORE %s %s %d %d\n",
                           district,
                           scores[i].name,
                           scores[i].total_severity,
                           scores[i].report_count);
        write(STDOUT_FILENO, buf, len);
    }

    {
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "SCORE_DONE %s\n", district);
        write(STDOUT_FILENO, buf, len);
    }

    return 0;
}