/*
 * monitor_reports.c - Background monitor for city infrastructure reports
 * OS Project - Phase 2
 *
 * compile: gcc monitor_reports.c -o monitor_reports
 * usage:   ./monitor_reports
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#define MONITOR_PID_FILE ".monitor_pid"

/* ─── Signal flags ──────────────────────────────────────────────────────────
 * volatile sig_atomic_t is the correct type for variables written in signal
 * handlers — it guarantees atomic read/write even if interrupted mid-operation.
 */
static volatile sig_atomic_t got_sigusr1 = 0;
static volatile sig_atomic_t got_sigint  = 0;

/* ─── Signal handlers ───────────────────────────────────────────────────────
 * We only set a flag here. The actual work (printf, etc.) is done in the
 * main loop — printf is not async-signal-safe so calling it directly in a
 * handler is undefined behaviour.
 */
static void handler_sigusr1(int sig) {
    (void)sig;
    got_sigusr1 = 1;
}

static void handler_sigint(int sig) {
    (void)sig;
    got_sigint = 1;
}

/* ─── Write PID to .monitor_pid ─────────────────────────────────────────── */
static void write_pid_file(void) {
    int fd = open(MONITOR_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open .monitor_pid");
        exit(1);
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    write(fd, buf, len);
    close(fd);
}

/* ─── Delete .monitor_pid on exit ───────────────────────────────────────── */
static void delete_pid_file(void) {
    unlink(MONITOR_PID_FILE);
}

/* ─── Current timestamp string ──────────────────────────────────────────── */
static void get_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

int main(void) {
    /* Step 1: write PID file */
    write_pid_file();
    printf("[monitor] Started. PID=%d written to %s\n",
           (int)getpid(), MONITOR_PID_FILE);
    printf("[monitor] Waiting for signals... (SIGUSR1=new report, SIGINT=quit)\n");
    fflush(stdout);

    /* Step 2: set up signal handlers with sigaction()
     *
     * sigaction() is more portable and controllable than signal().
     * sa_handler   = our function
     * sigemptyset  = no additional signals blocked during handler
     * sa_flags = 0 = default behaviour (SA_RESTART could also be used)
     */
    struct sigaction sa_usr1, sa_int;

    sa_usr1.sa_handler = handler_sigusr1;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa_usr1, NULL) < 0) {
        perror("sigaction SIGUSR1");
        delete_pid_file();
        return 1;
    }

    sa_int.sa_handler = handler_sigint;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    if (sigaction(SIGINT, &sa_int, NULL) < 0) {
        perror("sigaction SIGINT");
        delete_pid_file();
        return 1;
    }

    /* Step 3: main loop — sleep until a signal wakes us up */
    while (1) {
        /* pause() suspends the process until any signal is received */
        pause();

        char tbuf[32];
        get_timestamp(tbuf, sizeof(tbuf));

        if (got_sigusr1) {
            got_sigusr1 = 0;
            printf("[monitor][%s] SIGUSR1 received — a new report was added\n",
                   tbuf);
            fflush(stdout);
        }

        if (got_sigint) {
            printf("[monitor][%s] SIGINT received — shutting down\n", tbuf);
            fflush(stdout);
            break;
        }
    }

    /* Step 4: cleanup */
    delete_pid_file();
    printf("[monitor] .monitor_pid deleted. Goodbye.\n");
    return 0;
}