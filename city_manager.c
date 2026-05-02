/*
 * city_manager.c - City Infrastructure Issue Reporting System
 * OS Project - Phase 1 + Phase 2
 *
 * compile: gcc city_manager.c -o city_manager
 * usage:   ./city_manager --role <manager|inspector> --user <name> --<command> <district> [args...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

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

#define PERM_DIR     0750
#define PERM_REPORTS 0664
#define PERM_CFG     0640
#define PERM_LOG     0644
#define MONITOR_PID_FILE ".monitor_pid"

void mode_to_str(mode_t mode, char *buf) {
    buf[0] = (mode & S_IRUSR) ? 'r' : '-';
    buf[1] = (mode & S_IWUSR) ? 'w' : '-';
    buf[2] = (mode & S_IXUSR) ? 'x' : '-';
    buf[3] = (mode & S_IRGRP) ? 'r' : '-';
    buf[4] = (mode & S_IWGRP) ? 'w' : '-';
    buf[5] = (mode & S_IXGRP) ? 'x' : '-';
    buf[6] = (mode & S_IROTH) ? 'r' : '-';
    buf[7] = (mode & S_IWOTH) ? 'w' : '-';
    buf[8] = (mode & S_IXOTH) ? 'x' : '-';
    buf[9] = '\0';
}

int has_read_access(const char *path, const char *role) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (strcmp(role, "manager") == 0)
        return (st.st_mode & S_IRUSR) ? 1 : 0;
    else
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

void log_action(const char *district, const char *role,
                const char *user, const char *action) {
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/logged_district", district);

    struct stat st;
    if (stat(log_path, &st) == 0) {
        if (strcmp(role, "inspector") == 0 && !(st.st_mode & S_IWGRP))
            return;
    }

    int fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, PERM_LOG);
    if (fd < 0) return;
    chmod(log_path, PERM_LOG);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);

    char buf[512];
    int len = snprintf(buf, sizeof(buf), "[%s] %s %s %s\n", timebuf, user, role, action);
    write(fd, buf, len);
    close(fd);
}

void ensure_district(const char *district) {
    struct stat st;

    if (stat(district, &st) < 0) {
        if (mkdir(district, PERM_DIR) < 0) { perror("mkdir"); exit(1); }
        chmod(district, PERM_DIR);
    }

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/district.cfg", district);
    if (stat(cfg_path, &st) < 0) {
        int fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, PERM_CFG);
        if (fd >= 0) { write(fd, "threshold=1\n", 12); close(fd); }
        chmod(cfg_path, PERM_CFG);
    }

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/logged_district", district);
    if (stat(log_path, &st) < 0) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, PERM_LOG);
        if (fd >= 0) close(fd);
        chmod(log_path, PERM_LOG);
    }
}

void update_symlink(const char *district) {
    char link_name[512], target[512];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
    snprintf(target,    sizeof(target),    "%s/reports.dat", district);

    struct stat lst;
    if (lstat(link_name, &lst) == 0) unlink(link_name);
    if (symlink(target, link_name) < 0) perror("symlink");
}

void check_symlink(const char *district) {
    char link_name[512];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);

    struct stat lst, st;
    if (lstat(link_name, &lst) == 0 && S_ISLNK(lst.st_mode)) {
        if (stat(link_name, &st) < 0)
            fprintf(stderr, "WARNING: symlink %s is dangling\n", link_name);
    }
}

int next_report_id(const char *district) {
    char path[512];
    snprintf(path, sizeof(path), "%s/reports.dat", district);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;

    int max_id = 0;
    Report r;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report))
        if (r.id > max_id) max_id = r.id;
    close(fd);
    return max_id + 1;
}

/* - notify_monitor() */
int notify_monitor(void) {
    int fd = open(MONITOR_PID_FILE, O_RDONLY);
    if (fd < 0) return 0;

    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    buf[n] = '\0';
    pid_t monitor_pid = (pid_t)atoi(buf);
    if (monitor_pid <= 0) return 0;

    /* kill() with SIGUSR1 — returns 0 on success */
    if (kill(monitor_pid, SIGUSR1) < 0) return 0;
    return 1;
}

/* - add */
void cmd_add(const char *district, const char *role, const char *user) {
    (void)user; /* manager-only, user not logged for district removal */
    ensure_district(district);

    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    struct stat st;
    if (stat(dat_path, &st) == 0 && !has_write_access(dat_path, role)) {
        fprintf(stderr, "Error: role '%s' does not have write access\n", role);
        return;
    }

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
    getchar();
    printf("Description:");
    if (fgets(r.description, sizeof(r.description), stdin) == NULL) {
        fprintf(stderr, "Invalid input\n"); return;
    }
    r.description[strcspn(r.description, "\n")] = '\0';

    int fd = open(dat_path, O_WRONLY | O_APPEND | O_CREAT, PERM_REPORTS);
    if (fd < 0) { perror("open reports.dat"); return; }
    ssize_t written = write(fd, &r, sizeof(Report));
    close(fd);

    if (written != (ssize_t)sizeof(Report)) {
        fprintf(stderr, "Error: partial write\n"); return;
    }
    chmod(dat_path, PERM_REPORTS);
    update_symlink(district);
    log_action(district, role, user, "add");

    /* Phase 2: notify monitor and log the result */
    if (notify_monitor()) {
        printf("Monitor notified (SIGUSR1 sent)\n");
        log_action(district, role, user, "monitor notified via SIGUSR1");
    } else {
        printf("Monitor could not be notified (not running or .monitor_pid missing)\n");
        log_action(district, role, user, "monitor could not be notified");
    }

    printf("Report %d added to district %s\n", r.id, district);
}

/* - list */
void cmd_list(const char *district, const char *role, const char *user) {
    (void)user; /* manager-only, user not logged for district removal */
    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    struct stat st;
    if (stat(dat_path, &st) < 0) {
        fprintf(stderr, "No reports found for district '%s'\n", district); return;
    }
    if (!has_read_access(dat_path, role)) {
        fprintf(stderr, "Error: role '%s' has no read access\n", role); return;
    }

    char perm_str[10];
    mode_to_str(st.st_mode, perm_str);
    char mtimebuf[32];
    struct tm *tm_info = localtime(&st.st_mtime);
    strftime(mtimebuf, sizeof(mtimebuf), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("reports.dat: permissions=%s  size=%lld bytes  modified=%s\n",
           perm_str, (long long)st.st_size, mtimebuf);
    printf("Total records: %d\n\n", (int)(st.st_size / sizeof(Report)));

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

/* - view */
void cmd_view(const char *district, const char *role,
              const char *user, int report_id) {
    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    if (!has_read_access(dat_path, role)) {
        fprintf(stderr, "Error: role '%s' has no read access\n", role); return;
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
            printf("Inspector  : %s\n",           r.inspector);
            printf("Location   : %.6f, %.6f\n",   r.lat, r.lon);
            printf("Category   : %s\n",           r.category);
            printf("Severity   : %d\n",           r.severity);
            printf("Timestamp  : %s\n",           tbuf);
            printf("Description: %s\n",           r.description);
            break;
        }
    }
    close(fd);
    if (!found)
        fprintf(stderr, "Report %d not found in '%s'\n", report_id, district);
    log_action(district, role, user, "view");
}

/* - remove_report */
void cmd_remove_report(const char *district, const char *role,
                       const char *user, int report_id) {
    if (strcmp(role, "manager") != 0) {
        fprintf(stderr, "Error: only manager can remove reports\n"); return;
    }

    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    if (!has_write_access(dat_path, role)) {
        fprintf(stderr, "Error: no write access\n"); return;
    }

    int fd = open(dat_path, O_RDWR);
    if (fd < 0) { perror("open"); return; }

    struct stat st;
    fstat(fd, &st);
    int total = (int)(st.st_size / sizeof(Report));

    int target_idx = -1;
    Report r;
    for (int i = 0; i < total; i++) {
        lseek(fd, (off_t)i * sizeof(Report), SEEK_SET);
        if (read(fd, &r, sizeof(Report)) != sizeof(Report)) break;
        if (r.id == report_id) { target_idx = i; break; }
    }

    if (target_idx < 0) {
        fprintf(stderr, "Report %d not found in '%s'\n", report_id, district);
        close(fd); return;
    }

    for (int i = target_idx; i < total - 1; i++) {
        lseek(fd, (off_t)(i + 1) * sizeof(Report), SEEK_SET);
        read(fd, &r, sizeof(Report));
        lseek(fd, (off_t)i * sizeof(Report), SEEK_SET);
        write(fd, &r, sizeof(Report));
    }

    ftruncate(fd, (off_t)(total - 1) * sizeof(Report));
    close(fd);

    printf("Report %d removed from '%s'\n", report_id, district);
    log_action(district, role, user, "remove_report");
}

/* - update_threshold */
void cmd_update_threshold(const char *district, const char *role,
                          const char *user, int value) {
    if (strcmp(role, "manager") != 0) {
        fprintf(stderr, "Error: only manager can update threshold\n"); return;
    }

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/district.cfg", district);

    struct stat st;
    if (stat(cfg_path, &st) < 0) {
        fprintf(stderr, "Error: district.cfg not found\n"); return;
    }
    if ((st.st_mode & 0777) != PERM_CFG) {
        char perm_str[10];
        mode_to_str(st.st_mode, perm_str);
        fprintf(stderr, "Error: district.cfg has unexpected permissions %s. Refusing.\n", perm_str);
        return;
    }

    int fd = open(cfg_path, O_WRONLY | O_TRUNC);
    if (fd < 0) { perror("open district.cfg"); return; }

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "threshold=%d\n", value);
    write(fd, buf, len);
    close(fd);

    printf("Threshold for '%s' updated to %d\n", district, value);
    log_action(district, role, user, "update_threshold");
}

/* - remove_district */
void cmd_remove_district(const char *district, const char *role,
                         const char *user) {
    (void)user; /* manager-only, user not logged for district removal */
    if (strcmp(role, "manager") != 0) {
        fprintf(stderr, "Error: only manager can remove districts\n"); return;
    }

    struct stat st;
    if (stat(district, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district); return;
    }

    printf("Removing district '%s'...\n", district);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }

    if (pid == 0) {
        /* Child: replace itself with rm -rf <district> */
        char *args[] = { "rm", "-rf", (char *)district, NULL };
        execvp("rm", args);
        perror("execvp");
        exit(1);
    }

    /* Parent: wait for child to finish */
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char link_name[512];
        snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
        struct stat lst;
        if (lstat(link_name, &lst) == 0) unlink(link_name);
        printf("District '%s' removed successfully\n", district);
    } else {
        fprintf(stderr, "Error: rm -rf failed for '%s'\n", district);
    }
}

/* - FILTER — AI-ASSISTED */
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

int match_condition(Report *r, const char *field, const char *op, const char *value) {
    if (strcmp(field, "severity") == 0) {
        int v = atoi(value);
        if (strcmp(op, "==") == 0) return r->severity == v;
        if (strcmp(op, "!=") == 0) return r->severity != v;
        if (strcmp(op, "<")  == 0) return r->severity <  v;
        if (strcmp(op, "<=") == 0) return r->severity <= v;
        if (strcmp(op, ">")  == 0) return r->severity >  v;
        if (strcmp(op, ">=") == 0) return r->severity >= v;
    } else if (strcmp(field, "category") == 0) {
        int cmp = strcmp(r->category, value);
        if (strcmp(op, "==") == 0) return cmp == 0;
        if (strcmp(op, "!=") == 0) return cmp != 0;
    } else if (strcmp(field, "inspector") == 0) {
        int cmp = strcmp(r->inspector, value);
        if (strcmp(op, "==") == 0) return cmp == 0;
        if (strcmp(op, "!=") == 0) return cmp != 0;
    } else if (strcmp(field, "timestamp") == 0) {
        time_t v = (time_t)atol(value);
        if (strcmp(op, "==") == 0) return r->timestamp == v;
        if (strcmp(op, "!=") == 0) return r->timestamp != v;
        if (strcmp(op, "<")  == 0) return r->timestamp <  v;
        if (strcmp(op, "<=") == 0) return r->timestamp <= v;
        if (strcmp(op, ">")  == 0) return r->timestamp >  v;
        if (strcmp(op, ">=") == 0) return r->timestamp >= v;
    } else {
        fprintf(stderr, "Warning: unknown filter field '%s'\n", field);
    }
    return 0;
}

void cmd_filter(const char *district, const char *role, const char *user,
                char **conditions, int num_conditions) {
    char dat_path[512];
    snprintf(dat_path, sizeof(dat_path), "%s/reports.dat", district);

    if (!has_read_access(dat_path, role)) {
        fprintf(stderr, "Error: role '%s' has no read access\n", role); return;
    }

    char fields[16][64], ops[16][8], values[16][128];
    for (int i = 0; i < num_conditions; i++) {
        if (!parse_condition(conditions[i], fields[i], ops[i], values[i])) {
            fprintf(stderr, "Error: invalid condition '%s'\n", conditions[i]); return;
        }
    }

    int fd = open(dat_path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    Report r;
    int count = 0;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        int all_match = 1;
        for (int i = 0; i < num_conditions; i++) {
            if (!match_condition(&r, fields[i], ops[i], values[i])) {
                all_match = 0; break;
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

/*  MAIN */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --role <manager|inspector> --user <name> --<command> <district> [args]\n"
        "Commands:\n"
        "  --add <district>\n"
        "  --list <district>\n"
        "  --view <district> <report_id>\n"
        "  --remove_report <district> <report_id>     (manager only)\n"
        "  --update_threshold <district> <value>       (manager only)\n"
        "  --filter <district> <condition> [...]       e.g. severity:>=:2\n"
        "  --remove_district <district>                (manager only)\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    char *role = NULL, *user = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--role") == 0) role = argv[i + 1];
        if (strcmp(argv[i], "--user") == 0) user = argv[i + 1];
    }
    if (!role) { fprintf(stderr, "Error: --role required\n"); return 1; }
    if (!user) { fprintf(stderr, "Error: --user required\n"); return 1; }
    if (strcmp(role, "manager") != 0 && strcmp(role, "inspector") != 0) {
        fprintf(stderr, "Error: role must be manager or inspector\n"); return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--add") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: --add requires <district>\n"); return 1; }
            cmd_add(argv[i + 1], role, user); return 0;
        }
        if (strcmp(argv[i], "--list") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: --list requires <district>\n"); return 1; }
            cmd_list(argv[i + 1], role, user); return 0;
        }
        if (strcmp(argv[i], "--view") == 0) {
            if (i + 2 >= argc) { fprintf(stderr, "Error: --view requires <district> <id>\n"); return 1; }
            cmd_view(argv[i + 1], role, user, atoi(argv[i + 2])); return 0;
        }
        if (strcmp(argv[i], "--remove_report") == 0) {
            if (i + 2 >= argc) { fprintf(stderr, "Error: --remove_report requires <district> <id>\n"); return 1; }
            cmd_remove_report(argv[i + 1], role, user, atoi(argv[i + 2])); return 0;
        }
        if (strcmp(argv[i], "--update_threshold") == 0) {
            if (i + 2 >= argc) { fprintf(stderr, "Error: --update_threshold requires <district> <value>\n"); return 1; }
            cmd_update_threshold(argv[i + 1], role, user, atoi(argv[i + 2])); return 0;
        }
        if (strcmp(argv[i], "--filter") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: --filter requires <district>\n"); return 1; }
            cmd_filter(argv[i + 1], role, user, &argv[i + 2], argc - (i + 2)); return 0;
        }
        if (strcmp(argv[i], "--remove_district") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: --remove_district requires <district>\n"); return 1; }
            cmd_remove_district(argv[i + 1], role, user); return 0;
        }
    }

    fprintf(stderr, "Error: no command specified\n");
    usage(argv[0]);
    return 1;
}