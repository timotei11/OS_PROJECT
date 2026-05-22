#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

// constants
#define MAX_DISTRICTS  64
#define LINE_MAX_LEN   512
#define MONITOR_EXE    "./monitor_reports"
#define SCORER_EXE     "./scorer"

// global: PID of the hub_mon middle process
static pid_t hub_mon_pid = -1;

// utility: read one '\n'-terminated line from fd
/*
 * Returns number of bytes stored in buf (including '\0'), or 0 on EOF/error.
 * Reads byte-by-byte — fine for pipe traffic which is low-volume.
 */
static int read_line(int fd, char *buf, int max) {
    int i = 0;
    char c;
    while (i < max - 1) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            if (i == 0) return 0;   /* EOF with nothing read */
            break;
        }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return i;
}

// start_monitor
static void hub_mon_body(void) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("[hub_mon] pipe");
        exit(1);
    }

    pid_t mon_pid = fork();
    if (mon_pid < 0) {
        perror("[hub_mon] fork monitor");
        exit(1);
    }

    if (mon_pid == 0) {
        // child: become monitor_reports
        // redirect stdout -> pipe write-end
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            exit(1);
        }
        close(pipefd[1]);

        /* exec the monitor; it inherits the redirected stdout */
        execl(MONITOR_EXE, MONITOR_EXE, (char *)NULL);
        /* if we get here, exec failed */
        perror("execl monitor_reports");
        exit(1);
    }

    // hub_mon parent: read from pipe read-end
    close(pipefd[1]);   // hub_mon doesn't write to the pipe

    char line[LINE_MAX_LEN];
    int monitor_alive = 1;

    while (monitor_alive) {
        int n = read_line(pipefd[0], line, sizeof(line));
        if (n == 0) {
            // Pipe closed — monitor exited
            printf("[hub_mon] Monitor pipe closed (monitor has exited)\n");
            fflush(stdout);
            break;
        }

        // Strip trailing newline for clean display
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        /*
         * Parse structured prefix:  MSG:<TYPE>:<text>
         * If the line doesn't match we still show it verbatim.
         */
        if (strncmp(line, "MSG:", 4) == 0) {
            char *type_start = line + 4;
            char *colon      = strchr(type_start, ':');
            if (colon) {
                *colon = '\0';
                const char *type = type_start;
                const char *text = colon + 1;
                printf("[monitor/%s] %s\n", type, text);
                fflush(stdout);

                // QUIT means the monitor is shutting down
                if (strcmp(type, "QUIT") == 0) {
                    monitor_alive = 0;
                }
                // ERROR also means the monitor ended (duplicate check)
                if (strcmp(type, "ERROR") == 0) {
                    monitor_alive = 0;
                }
            } else {
                printf("[monitor] %s\n", type_start);
                fflush(stdout);
            }
        } else {
            printf("[monitor] %s\n", line);
            fflush(stdout);
        }
    }

    close(pipefd[0]);

    // Wait for the monitor child to avoid a zombie
    int status;
    waitpid(mon_pid, &status, 0);
    if (WIFEXITED(status))
        printf("[hub_mon] monitor_reports exited with status %d\n",
               WEXITSTATUS(status));
    else
        printf("[hub_mon] monitor_reports terminated abnormally\n");
    fflush(stdout);

    exit(0);   // hub_mon itself exits
}

/*
 * cmd_start_monitor()
 * Called by the hub when the user types "start_monitor".
 * Forks hub_mon, which runs hub_mon_body().
 */
static void cmd_start_monitor(void) {
    if (hub_mon_pid > 0) {
        // Check whether the previous hub_mon is still alive
        int status;
        pid_t r = waitpid(hub_mon_pid, &status, WNOHANG);
        if (r == 0) {
            printf("[hub] A monitor is already running (hub_mon PID=%d)\n",
                   (int)hub_mon_pid);
            return;
        }
        // It has exited — allow a new one
        hub_mon_pid = -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[hub] fork hub_mon");
        return;
    }
    if (pid == 0) {
        // child: become hub_mon
        hub_mon_body();
        // never reached
        exit(1);
    }

    // parent: remember hub_mon's PID
    hub_mon_pid = pid;
    printf("[hub] hub_mon started (PID=%d) — monitor output will appear above\n",
           (int)hub_mon_pid);
}

// calculate_scores

typedef struct {
    char  district[128];
    int   pipe_rd;          // read-end of the scorer's stdout pipe
    pid_t pid;
} ScorerJob;


 //Accumulated lines from all scorers, printed at the end.
 
typedef struct {
    char district[128];
    char inspector[64];
    int  total_severity;
    int  report_count;
} ScoreLine;

#define MAX_SCORE_LINES 4096

static ScoreLine score_lines[MAX_SCORE_LINES];
static int       num_score_lines = 0;

/*
 * Parse a SCORE line from the scorer:
 *   SCORE <district> <inspector> <total_severity> <report_count>
 */
static void parse_score_line(const char *line) {
    if (num_score_lines >= MAX_SCORE_LINES) return;

    ScoreLine *sl = &score_lines[num_score_lines];
    if (sscanf(line, "SCORE %127s %63s %d %d",
               sl->district, sl->inspector,
               &sl->total_severity, &sl->report_count) == 4) {
        num_score_lines++;
    }
}

static int cmp_score_lines(const void *a, const void *b) {
    const ScoreLine *sa = (const ScoreLine *)a;
    const ScoreLine *sb = (const ScoreLine *)b;
    // Sort by district ascending, then severity descending
    int d = strcmp(sa->district, sb->district);
    if (d != 0) return d;
    return sb->total_severity - sa->total_severity;
}

/*
 * cmd_calculate_scores()
 * districts[]  : array of district names
 * num_districts: count
 */
static void cmd_calculate_scores(char **districts, int num_districts) {
    if (num_districts == 0) {
        printf("[hub] calculate_scores: no districts specified\n");
        return;
    }
    if (num_districts > MAX_DISTRICTS) {
        printf("[hub] calculate_scores: too many districts (max %d)\n",
               MAX_DISTRICTS);
        return;
    }

    // Reset accumulated results 
    num_score_lines = 0;

    ScorerJob jobs[MAX_DISTRICTS];

    // spawn one scorer per district
    int active = 0;
    for (int i = 0; i < num_districts; i++) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            perror("[hub] pipe for scorer");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("[hub] fork scorer");
            close(pipefd[0]);
            close(pipefd[1]);
            continue;
        }

        if (pid == 0) {
            // scorer child
            close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                perror("dup2 scorer");
                exit(1);
            }
            close(pipefd[1]);
            execl(SCORER_EXE, SCORER_EXE, districts[i], (char *)NULL);
            perror("execl scorer");
            exit(1);
        }

        // parent: record the job
        close(pipefd[1]);
        strncpy(jobs[active].district, districts[i],
                sizeof(jobs[active].district) - 1);
        jobs[active].district[sizeof(jobs[active].district) - 1] = '\0';
        jobs[active].pipe_rd = pipefd[0];
        jobs[active].pid     = pid;
        active++;
    }

    // collect output from all scorers
    
    for (int i = 0; i < active; i++) {
        char line[LINE_MAX_LEN];
        while (read_line(jobs[i].pipe_rd, line, sizeof(line)) > 0) {
            // Strip newline
            int len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

            if (strncmp(line, "SCORE_ERROR", 11) == 0) {
                printf("[hub] Scorer warning: %s\n", line + 12);
            } else if (strncmp(line, "SCORE_DONE", 10) == 0) {
                // terminator — nothing to do
            } else if (strncmp(line, "SCORE ", 6) == 0) {
                parse_score_line(line);
            }
        }
        close(jobs[i].pipe_rd);

        int status;
        waitpid(jobs[i].pid, &status, 0);
    }

    // print combined report
    if (num_score_lines == 0) {
        printf("[hub] No workload data found for the specified districts.\n");
        return;
    }

    qsort(score_lines, num_score_lines, sizeof(ScoreLine), cmp_score_lines);

    printf("\n");
    printf("|=========================================================|\n");
    printf("|            COMBINED WORKLOAD REPORT                     |\n");
    printf("|===============|===================|========|============|\n");
    printf("| District      | Inspector         | Score  | Reports    |\n");
    printf("|===============|===================|========|============|\n");

    const char *last_district = "";
    for (int i = 0; i < num_score_lines; i++) {
        ScoreLine *sl = &score_lines[i];
        if (strcmp(sl->district, last_district) != 0) {
            if (i > 0)
                printf("|===============|===================|========|============|\n");
            last_district = sl->district;
        }
        printf("| %-13s | %-17s | %6d | %10d |\n",
               sl->district, sl->inspector,
               sl->total_severity, sl->report_count);
    }

    printf("|_______________|___________________|________|____________|\n");
    printf("\n");
}

// command parser 
static void trim_newline(char *s) {
    int len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

/* Tokenise buf in-place; return number of tokens (argv-style). */
static int tokenise(char *buf, char **tokens, int max_tokens) {
    int n = 0;
    char *p = buf;
    while (*p && n < max_tokens) {
        /* skip leading spaces */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tokens[n++] = p;
        /* advance to next space */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}


int main(void) {
    /*
     * we do explicit waitpid() for scorer jobs and hub_mon in
     * cmd_start_monitor(), so the behaviour is still deterministic.
     */
    struct sigaction sa_chld;
    sa_chld.sa_handler = SIG_DFL;   // keep default — we waitpid()
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    printf("city_hub — Phase 3 interactive hub\n");
    printf("Commands: start_monitor | calculate_scores <district>... | quit\n");
    printf("\n");

    char buf[1024];
    while (1) {
        printf("hub> ");
        fflush(stdout);

        if (fgets(buf, sizeof(buf), stdin) == NULL) {
            // EOF (Ctrl-D)
            printf("\n[hub] EOF — exiting\n");
            break;
        }
        trim_newline(buf);
        if (buf[0] == '\0') continue;   // blank line

        char *tokens[MAX_DISTRICTS + 2];
        int ntok = tokenise(buf, tokens, MAX_DISTRICTS + 2);
        if (ntok == 0) continue;

        const char *cmd = tokens[0];

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            printf("[hub] Goodbye.\n");
            break;
        }

        if (strcmp(cmd, "start_monitor") == 0) {
            cmd_start_monitor();
            continue;
        }

        if (strcmp(cmd, "calculate_scores") == 0) {
            cmd_calculate_scores(tokens + 1, ntok - 1);
            continue;
        }

        printf("[hub] Unknown command: '%s'\n", cmd);
        printf("      Commands: start_monitor | calculate_scores <district>... | quit\n");
    }

    if (hub_mon_pid > 0) {
        int status;
        pid_t r = waitpid(hub_mon_pid, &status, WNOHANG);
        if (r == 0) {
            printf("[hub] Note: hub_mon (PID=%d) is still running in the background.\n",
                   (int)hub_mon_pid);
        }
    }

    return 0;
}