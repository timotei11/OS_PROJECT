/*
 * city_manager.c - City Infrastructure Issue Reporting System
 * SO/OS Project - Phase 1
 *
 * Compile: gcc city_manager.c -o city_manager
 * Usage:   ./city_manager --role <manager|inspector> --user <name> --<command> <district> [args...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

/* ─── Fixed-size Report record ─────────────────────────────────────────────
 * Layout (216 bytes on 64-bit Linux due to alignment padding):
 *   int      id           →  4 bytes  (offset 0)
 *   char     inspector[]  → 64 bytes  (offset 4)
 *   [4-byte padding]                  (offset 68 → aligned to 72 for double)
 *   double   lat          →  8 bytes  (offset 72)
 *   double   lon          →  8 bytes  (offset 80)
 *   char     category[]   → 32 bytes  (offset 88)
 *   int      severity     →  4 bytes  (offset 120)
 *   [4-byte padding]                  (offset 124 → aligned to 128 for time_t)
 *   time_t   timestamp    →  8 bytes  (offset 128)
 *   char     description[]→ 80 bytes  (offset 136)
 *                           ─────────
 *                    total: 216 bytes  (sizeof(Report) on this platform)
 *
 * IMPORTANT: all code uses sizeof(Report) — never a hardcoded number.
 * The diagram in the spec shows 208 as an illustrative example; your
 * compiler may produce a different size depending on alignment rules.
 * All records are written/read as raw bytes with read()/write().
 * lseek(fd, N * sizeof(Report), SEEK_SET) jumps directly to record N.
 */
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

/* ─── Permission constants (octal) ─────────────────────────────────────────
 * District directory : rwxr-x---  (0750)
 * reports.dat        : rw-rw-r--  (0664)
 * district.cfg       : rw-r-----  (0640)
 * logged_district    : rw-r--r--  (0644)
 *
 * Simulation model:
 *   manager  = owner  (user bits)
 *   inspector = group member (group bits)
 */
#define PERM_DIR     0750
#define PERM_REPORTS 0664
#define PERM_CFG     0640
#define PERM_LOG     0644

/* ─── Helper: convert mode bits → 9-char string like "rw-rw-r--" ─────────
 * We check each permission bit individually using the S_I* macros.
 * buf must point to a buffer of at least 10 bytes (9 chars + '\0').
 */
void mode_to_str(mode_t mode, char *buf) {
    buf[0] = (mode & S_IRUSR) ? 'r' : '-';  /* owner read    */
    buf[1] = (mode & S_IWUSR) ? 'w' : '-';  /* owner write   */
    buf[2] = (mode & S_IXUSR) ? 'x' : '-';  /* owner execute */
    buf[3] = (mode & S_IRGRP) ? 'r' : '-';  /* group read    */
    buf[4] = (mode & S_IWGRP) ? 'w' : '-';  /* group write   */
    buf[5] = (mode & S_IXGRP) ? 'x' : '-';  /* group execute */
    buf[6] = (mode & S_IROTH) ? 'r' : '-';  /* other read    */
    buf[7] = (mode & S_IWOTH) ? 'w' : '-';  /* other write   */
    buf[8] = (mode & S_IXOTH) ? 'x' : '-';  /* other execute */
    buf[9] = '\0';
}

/* ─── Permission checkers ───────────────────────────────────────────────────
 * We call stat() to get the current permission bits of a file, then isolate
 * only the bits relevant to the declared role (owner bits for manager,
 * group bits for inspector). We do NOT compare the whole mode word.
 */
int has_read_access(const char *path, const char *role) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (strcmp(role, "manager") == 0)
        return (st.st_mode & S_IRUSR) ? 1 : 0;
    else  /* inspector */
        return (st.st_mode & S_IRGRP) ? 1 : 0;
}

int has_write_access(const char *path, const char *role) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (strcmp(role, "manager") == 0)
        return (st.st_mode & S_IWUSR) ? 1 : 0;
    else
        return (st.st_mode & S_IWGRP) ? 1 : 0;
}

/* ─── Logging ───────────────────────────────────────────────────────────────
 * logged_district is 0644 → only manager (owner) may write.
 * We enforce this by checking write access before appending.
 * Format: [YYYY-MM-DD HH:MM:SS] <user> <role> <action>
 */
void log_action(const char *district, const char *role,
                const char *user, const char *action) {
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/logged_district", district);

    /* Per spec: logged_district is 0644, only owner (manager) writes.
     * Inspectors are forbidden from writing to it.
     * We still call this function for all operations so the log is always
     * written by the system (simulating the OS enforcing the permission). */
    struct stat st;
    if (stat(log_path, &st) == 0) {
        if (strcmp(role, "inspector") == 0 && !(st.st_mode & S_IWGRP)) {
            /* Inspector cannot write — silently skip logging, or note it */
            return;
        }
    }

    int fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, PERM_LOG);
    if (fd < 0) return;
    chmod(log_path, PERM_LOG);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
                       "[%s] %s %s %s\n", timebuf, user, role, action);
    write(fd, buf, len);
    close(fd);
}

/* ─── District initialisation ───────────────────────────────────────────────
 * Creates the district directory and its required files if they don't exist.
 * Sets permissions with chmod() after creation.
 */
void ensure_district(const char *district) {
    struct stat st;

    /* Create district directory */
    if (stat(district, &st) < 0) {
        if (mkdir(district, PERM_DIR) < 0) {
            perror("mkdir");
            exit(1);
        }
        chmod(district, PERM_DIR);
    }

    /* Create district.cfg (severity threshold config) */
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/district.cfg", district);
    if (stat(cfg_path, &st) < 0) {
        int fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, PERM_CFG);
        if (fd >= 0) {
            write(fd, "threshold=1\n", 12);
            close(fd);
        }
        chmod(cfg_path, PERM_CFG);
    }

    /* Create logged_district (operation log) */
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/logged_district", district);
    if (stat(log_path, &st) < 0) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, PERM_LOG);
        if (fd >= 0) close(fd);
        chmod(log_path, PERM_LOG);
    }
}

/* ─── Symbolic link management ──────────────────────────────────────────────
 * Creates active_reports-<district> → <district>/reports.dat
 * Uses lstat() (not stat()) so we can detect symlinks vs. regular files.
 * Removes any existing link first to avoid EEXIST.
 */
void update_symlink(const char *district) {
    char link_name[512];
    char target[512];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
    snprintf(target,    sizeof(target),    "%s/reports.dat", district);

    struct stat lst;
    /* lstat() sees the link itself — if it exists, remove it */
    if (lstat(link_name, &lst) == 0) {
        unlink(link_name);
    }
    if (symlink(target, link_name) < 0) {
        perror("symlink");
    }
}

/* ─── Dangling symlink check ────────────────────────────────────────────────
 * lstat() succeeds on a dangling symlink (the link exists but the target
 * doesn't). stat() follows the link and fails. We use both to detect it.
 */
void check_symlink(const char *district) {
    char link_name[512];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);

    struct stat lst, st;
    if (lstat(link_name, &lst) == 0 && S_ISLNK(lst.st_mode)) {
        if (stat(link_name, &st) < 0) {
            fprintf(stderr,
                "WARNING: symlink %s is dangling (target does not exist)\n",
                link_name);
        }
    }
}

/* ─── Find next available report ID ────────────────────────────────────────
 * Scans all records in reports.dat and returns max_id + 1.
 * Returns 1 if the file is empty or doesn't exist.
 */
int next_report_id(const char *district) {
    char path[512];
    snprintf(path, sizeof(path), "%s/reports.dat", district);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;

    int max_id = 0;
    Report r;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        if (r.id > max_id) max_id = r.id;
    }
    close(fd);
    return max_id + 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * COMMANDS
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── add <district> ─────────────────────────────────────────────────────────
 * Both roles may add reports.
 * Creates the district structure if it doesn't exist.
 * Appends a fixed-size Report record to reports.dat.
 * Sets reports.dat permissions to 0664 (rw-rw-r--).
 */
void cmd_add(const char *district, const char *role, const char *user) {
    ensure_district(district);

    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    /* Check permission before writing (simulate role-based access) */
    struct stat st;
    if (stat(dat_path, &st) == 0) {
        if (!has_write_access(dat_path, role)) {
            fprintf(stderr,
                "Error: role '%s' does not have write access to reports.dat\n",
                role);
            return;
        }
    }

    /* Gather report data interactively */
    Report r;
    memset(&r, 0, sizeof(Report));
    r.id = next_report_id(district);
    strncpy(r.inspector, user, sizeof(r.inspector) - 1);
    r.timestamp = time(NULL);

    printf("X: ");
    if (scanf("%lf", &r.lat) != 1) { fprintf(stderr, "Invalid input\n"); return; }
    printf("Y: ");
    if (scanf("%lf", &r.lon) != 1) { fprintf(stderr, "Invalid input\n"); return; }
    printf("Category (road/lighting/flooding/other): ");
    if (scanf("%31s", r.category) != 1) { fprintf(stderr, "Invalid input\n"); return; }
    printf("Severity level (1/2/3): ");
    if (scanf("%d", &r.severity) != 1 || r.severity < 1 || r.severity > 3) {
        fprintf(stderr, "Severity must be 1, 2, or 3\n"); return;
    }
    /* Consume the leftover newline before fgets */
    getchar();
    printf("Description:");
    if (fgets(r.description, sizeof(r.description), stdin) == NULL) {
        fprintf(stderr, "Invalid input\n"); return;
    }
    /* Strip trailing newline */
    r.description[strcspn(r.description, "\n")] = '\0';

    /* Append record to reports.dat */
    int fd = open(dat_path, O_WRONLY | O_APPEND | O_CREAT, PERM_REPORTS);
    if (fd < 0) { perror("open reports.dat"); return; }
    ssize_t written = write(fd, &r, sizeof(Report));
    close(fd);

    if (written != sizeof(Report)) {
        fprintf(stderr, "Error: partial write to reports.dat\n");
        return;
    }
    chmod(dat_path, PERM_REPORTS);  /* ensure 664 */

    update_symlink(district);
    log_action(district, role, user, "add");

    printf("Report %d added to district %s\n", r.id, district);
}

/* ── list <district> ────────────────────────────────────────────────────────
 * Lists all reports.
 * Always prints permission bits of reports.dat in symbolic form,
 * plus file size and last modification time.
 */
void cmd_list(const char *district, const char *role, const char *user) {
    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    struct stat st;
    if (stat(dat_path, &st) < 0) {
        fprintf(stderr, "No reports found for district '%s'\n", district);
        return;
    }
    if (!has_read_access(dat_path, role)) {
        fprintf(stderr,
            "Error: role '%s' does not have read access to reports.dat\n", role);
        return;
    }

    /* Print file metadata */
    char perm_str[10];
    mode_to_str(st.st_mode, perm_str);
    char mtimebuf[32];
    struct tm *tm_info = localtime(&st.st_mtime);
    strftime(mtimebuf, sizeof(mtimebuf), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("reports.dat: permissions=%s  size=%lld bytes  modified=%s\n",
           perm_str, (long long)st.st_size, mtimebuf);

    int total = (int)(st.st_size / sizeof(Report));
    printf("Total records: %d\n\n", total);

    /* Print header */
    printf("%-5s %-20s %9s %9s %-12s %-8s %-20s %s\n",
           "ID", "Inspector", "Lat", "Lon", "Category", "Severity", "Timestamp", "Description");
    printf("%-5s %-20s %9s %9s %-12s %-8s %-20s %s\n",
           "----", "-------------------", "---------", "---------",
           "--------", "--------", "-------------------", "-----------");

    int fd = open(dat_path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    Report r;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        char tbuf[24];
        struct tm *rtm = localtime(&r.timestamp);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", rtm);
        printf("%-5d %-20s %9.4f %9.4f %-12s %-8d %-20s %s\n",
               r.id, r.inspector, r.lat, r.lon,
               r.category, r.severity, tbuf, r.description);
    }
    close(fd);

    check_symlink(district);
    log_action(district, role, user, "list");
}

/* ── view <district> <report_id> ────────────────────────────────────────────
 * Prints full details of a single report. Available to both roles.
 * Scans records sequentially; uses lseek implicitly via read().
 */
void cmd_view(const char *district, const char *role,
              const char *user, int report_id) {
    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    if (!has_read_access(dat_path, role)) {
        fprintf(stderr, "Error: role '%s' has no read access\n", role);
        return;
    }

    int fd = open(dat_path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    Report r;
    int found = 0;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        if (r.id == report_id) {
            found = 1;
            char tbuf[32];
            struct tm *rtm = localtime(&r.timestamp);
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", rtm);
            printf("=== Report %d ===\n", r.id);
            printf("Inspector  : %s\n",     r.inspector);
            printf("Location   : %.6f, %.6f\n", r.lat, r.lon);
            printf("Category   : %s\n",     r.category);
            printf("Severity   : %d\n",     r.severity);
            printf("Timestamp  : %s\n",     tbuf);
            printf("Description: %s\n",     r.description);
            break;
        }
    }
    close(fd);

    if (!found)
        fprintf(stderr, "Report %d not found in district '%s'\n",
                report_id, district);

    log_action(district, role, user, "view");
}

/* ── remove_report <district> <report_id> ──────────────────────────────────
 * Manager only.
 * Uses lseek() to shift subsequent records left by one position,
 * then truncates the file with ftruncate().
 *
 * Algorithm:
 *   1. Find the index of the target record.
 *   2. For i from target_idx to total-2:
 *        read record at position (i+1)
 *        write it at position i
 *   3. ftruncate(fd, (total-1) * sizeof(Report))
 */
void cmd_remove_report(const char *district, const char *role,
                       const char *user, int report_id) {
    if (strcmp(role, "manager") != 0) {
        fprintf(stderr, "Error: only the manager role can remove reports\n");
        return;
    }

    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    if (!has_write_access(dat_path, role)) {
        fprintf(stderr, "Error: no write access to reports.dat\n");
        return;
    }

    int fd = open(dat_path, O_RDWR);
    if (fd < 0) { perror("open"); return; }

    struct stat st;
    fstat(fd, &st);
    int total = (int)(st.st_size / sizeof(Report));

    /* Step 1: locate the target record's index */
    int target_idx = -1;
    Report r;
    for (int i = 0; i < total; i++) {
        lseek(fd, (off_t)i * sizeof(Report), SEEK_SET);
        if (read(fd, &r, sizeof(Report)) != sizeof(Report)) break;
        if (r.id == report_id) {
            target_idx = i;
            break;
        }
    }

    if (target_idx < 0) {
        fprintf(stderr, "Report %d not found in district '%s'\n",
                report_id, district);
        close(fd);
        return;
    }

    /* Step 2: shift every record after target one position to the left */
    for (int i = target_idx; i < total - 1; i++) {
        lseek(fd, (off_t)(i + 1) * sizeof(Report), SEEK_SET);
        read(fd, &r, sizeof(Report));

        lseek(fd, (off_t)i * sizeof(Report), SEEK_SET);
        write(fd, &r, sizeof(Report));
    }

    /* Step 3: shrink the file by one record */
    ftruncate(fd, (off_t)(total - 1) * sizeof(Report));
    close(fd);

    printf("Report %d removed from district '%s'\n", report_id, district);
    log_action(district, role, user, "remove_report");
}

/* ── update_threshold <district> <value> ────────────────────────────────────
 * Manager only.
 * Reads stat() on district.cfg and REFUSES to write if permissions
 * have been changed away from 0640.
 */
void cmd_update_threshold(const char *district, const char *role,
                          const char *user, int value) {
    if (strcmp(role, "manager") != 0) {
        fprintf(stderr, "Error: only the manager role can update the threshold\n");
        return;
    }

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/district.cfg", district);

    struct stat st;
    if (stat(cfg_path, &st) < 0) {
        fprintf(stderr, "Error: district.cfg not found in '%s'\n", district);
        return;
    }

    /* Verify permissions are EXACTLY 0640 (rw-r-----) */
    if ((st.st_mode & 0777) != PERM_CFG) {
        char perm_str[10];
        mode_to_str(st.st_mode, perm_str);
        fprintf(stderr,
            "Error: district.cfg has unexpected permissions %s "
            "(expected rw-r-----, 640). Refusing to write.\n",
            perm_str);
        return;
    }

    int fd = open(cfg_path, O_WRONLY | O_TRUNC);
    if (fd < 0) { perror("open district.cfg"); return; }

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "threshold=%d\n", value);
    write(fd, buf, len);
    close(fd);

    printf("Threshold for district '%s' updated to %d\n", district, value);
    log_action(district, role, user, "update_threshold");
}

/* ══════════════════════════════════════════════════════════════════════════
 * FILTER COMMAND — AI-ASSISTED FUNCTIONS
 * (Documented in ai_usage.md as required by the spec.)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * parse_condition() — AI-generated, reviewed and kept as-is.
 *
 * Splits "field:operator:value" into three separate strings.
 * Finds the first ':' to end the field, finds the next ':' to end the
 * operator, and copies the rest as value.
 * Returns 1 on success, 0 if the string is malformed.
 *
 * Examples:
 *   "severity:>=:2"   → field="severity", op=">=",  value="2"
 *   "category:==:road"→ field="category", op="==",  value="road"
 */
int parse_condition(const char *input, char *field, char *op, char *value) {
    const char *p1 = strchr(input, ':');
    if (!p1) return 0;

    int flen = (int)(p1 - input);
    strncpy(field, input, flen);
    field[flen] = '\0';

    const char *p2 = strchr(p1 + 1, ':');
    if (!p2) return 0;

    int olen = (int)(p2 - (p1 + 1));
    strncpy(op, p1 + 1, olen);
    op[olen] = '\0';

    strcpy(value, p2 + 1);
    return 1;
}

/*
 * match_condition() — AI-generated, reviewed and modified.
 *
 * Tests a Report against a parsed condition.
 * Returns 1 if the condition is satisfied, 0 otherwise.
 *
 * Notes on my review:
 * - The AI correctly used atoi() for integer fields and strcmp() for strings.
 * - I added the timestamp branch and verified atol() is correct for time_t.
 * - String fields (category, inspector) only support == and != because
 *   lexicographic ordering of these fields is not meaningful in context.
 */
int match_condition(Report *r, const char *field, const char *op, const char *value) {
    if (strcmp(field, "severity") == 0) {
        int v = atoi(value);
        if (strcmp(op, "==") == 0) return r->severity == v;
        if (strcmp(op, "!=") == 0) return r->severity != v;
        if (strcmp(op, "<")  == 0) return r->severity <  v;
        if (strcmp(op, "<=") == 0) return r->severity <= v;
        if (strcmp(op, ">")  == 0) return r->severity >  v;
        if (strcmp(op, ">=") == 0) return r->severity >= v;
    }
    else if (strcmp(field, "category") == 0) {
        int cmp = strcmp(r->category, value);
        if (strcmp(op, "==") == 0) return cmp == 0;
        if (strcmp(op, "!=") == 0) return cmp != 0;
    }
    else if (strcmp(field, "inspector") == 0) {
        int cmp = strcmp(r->inspector, value);
        if (strcmp(op, "==") == 0) return cmp == 0;
        if (strcmp(op, "!=") == 0) return cmp != 0;
    }
    else if (strcmp(field, "timestamp") == 0) {
        time_t v = (time_t)atol(value);  /* value is a Unix epoch integer */
        if (strcmp(op, "==") == 0) return r->timestamp == v;
        if (strcmp(op, "!=") == 0) return r->timestamp != v;
        if (strcmp(op, "<")  == 0) return r->timestamp <  v;
        if (strcmp(op, "<=") == 0) return r->timestamp <= v;
        if (strcmp(op, ">")  == 0) return r->timestamp >  v;
        if (strcmp(op, ">=") == 0) return r->timestamp >= v;
    }
    else {
        fprintf(stderr, "Warning: unknown filter field '%s'\n", field);
    }
    return 0;
}

/* ── filter <district> <condition> [condition...] ───────────────────────────
 * Reads records one by one, parses each condition, tests all conditions
 * (AND logic), prints matching records.
 */
void cmd_filter(const char *district, const char *role, const char *user,
                char **conditions, int num_conditions) {
    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    if (!has_read_access(dat_path, role)) {
        fprintf(stderr, "Error: role '%s' has no read access\n", role);
        return;
    }

    /* Parse all conditions up front so we can report errors early */
    char fields[16][64], ops[16][8], values[16][128];
    for (int i = 0; i < num_conditions; i++) {
        if (!parse_condition(conditions[i], fields[i], ops[i], values[i])) {
            fprintf(stderr,
                "Error: invalid condition '%s' (expected field:op:value)\n",
                conditions[i]);
            return;
        }
    }

    int fd = open(dat_path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    Report r;
    int count = 0;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        /* AND logic: every condition must match */
        int all_match = 1;
        for (int i = 0; i < num_conditions; i++) {
            if (!match_condition(&r, fields[i], ops[i], values[i])) {
                all_match = 0;
                break;
            }
        }
        if (all_match) {
            char tbuf[24];
            struct tm *rtm = localtime(&r.timestamp);
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", rtm);
            printf("[%d] %-20s | %-12s | %.4f,%.4f | sev:%d | %s | %s\n",
                   r.id, r.inspector, r.category,
                   r.lat, r.lon, r.severity, tbuf, r.description);
            count++;
        }
    }
    close(fd);
    printf("(%d record(s) matched)\n", count);
    log_action(district, role, user, "filter");
}

/* ══════════════════════════════════════════════════════════════════════════
 * ARGUMENT PARSING AND MAIN
 * ══════════════════════════════════════════════════════════════════════════ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --role <manager|inspector> --user <name> --<command> <district> [args]\n"
        "Commands:\n"
        "  --add <district>\n"
        "  --list <district>\n"
        "  --view <district> <report_id>\n"
        "  --remove_report <district> <report_id>   (manager only)\n"
        "  --update_threshold <district> <value>     (manager only)\n"
        "  --filter <district> <condition> [...]     e.g. severity:>=:2\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    /* ── First pass: collect --role and --user from anywhere in argv ── */
    char *role = NULL, *user = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--role") == 0) role = argv[i + 1];
        if (strcmp(argv[i], "--user") == 0) user = argv[i + 1];
    }
    if (!role) { fprintf(stderr, "Error: --role <manager|inspector> is required\n"); return 1; }
    if (!user) { fprintf(stderr, "Error: --user <name> is required\n"); return 1; }
    if (strcmp(role, "manager") != 0 && strcmp(role, "inspector") != 0) {
        fprintf(stderr, "Error: role must be 'manager' or 'inspector'\n"); return 1;
    }

    /* ── Second pass: find the command flag ── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--add") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: --add requires <district>\n"); return 1; }
            cmd_add(argv[i + 1], role, user);
            return 0;
        }
        if (strcmp(argv[i], "--list") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: --list requires <district>\n"); return 1; }
            cmd_list(argv[i + 1], role, user);
            return 0;
        }
        if (strcmp(argv[i], "--view") == 0) {
            if (i + 2 >= argc) { fprintf(stderr, "Error: --view requires <district> <report_id>\n"); return 1; }
            cmd_view(argv[i + 1], role, user, atoi(argv[i + 2]));
            return 0;
        }
        if (strcmp(argv[i], "--remove_report") == 0) {
            if (i + 2 >= argc) { fprintf(stderr, "Error: --remove_report requires <district> <report_id>\n"); return 1; }
            cmd_remove_report(argv[i + 1], role, user, atoi(argv[i + 2]));
            return 0;
        }
        if (strcmp(argv[i], "--update_threshold") == 0) {
            if (i + 2 >= argc) { fprintf(stderr, "Error: --update_threshold requires <district> <value>\n"); return 1; }
            cmd_update_threshold(argv[i + 1], role, user, atoi(argv[i + 2]));
            return 0;
        }
        if (strcmp(argv[i], "--filter") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: --filter requires <district> [conditions...]\n"); return 1; }
            const char *district = argv[i + 1];
            /* conditions start at argv[i+2] */
            int num_cond = argc - (i + 2);
            cmd_filter(district, role, user, &argv[i + 2], num_cond);
            return 0;
        }
    }

    fprintf(stderr, "Error: no command specified\n");
    usage(argv[0]);
    return 1;
}