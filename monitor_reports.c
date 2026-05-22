#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
 
#define MONITOR_PID_FILE ".monitor_pid"
 
// signal flags 
static volatile sig_atomic_t got_sigusr1 = 0;
static volatile sig_atomic_t got_sigint  = 0;
 
static void handler_sigusr1(int sig) { (void)sig; got_sigusr1 = 1; }
static void handler_sigint (int sig) { (void)sig; got_sigint  = 1; }
 
// helpers
 
/*
 * emit()  —  write one structured line to stdout.
 * type  : "READY" | "INFO" | "ERROR" | "QUIT"
 * text  : human-readable payload (no newline)
 */
static void emit(const char *type, const char *text) {
    /* Use write() so the output is not buffered — hub_mon reads per-line */
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "MSG:%s:%s\n", type, text);
    write(STDOUT_FILENO, buf, len);
}
 
static void get_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}
 
// PID-file helpers
 
/*
 * read_pid_file()
 * Returns the PID stored in .monitor_pid, or -1 if the file does not exist
 * or cannot be read.
 */
static pid_t read_pid_file(void) {
    int fd = open(MONITOR_PID_FILE, O_RDONLY);
    if (fd < 0) return -1;
 
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
 
    buf[n] = '\0';
    pid_t p = (pid_t)atoi(buf);
    return (p > 0) ? p : -1;
}
 
/*
 * write_pid_file()  —  (over)write .monitor_pid with our PID.
 */
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
 
static void delete_pid_file(void) {
    unlink(MONITOR_PID_FILE);
}
 
// main
 
int main(void) {
  
    pid_t existing = read_pid_file();
    if (existing > 0) {
        /*
         * Send a SIGUSR1-probe: kill(pid, 0) checks whether the process
         * exists without actually sending a signal.
         */
        if (kill(existing, 0) == 0) {
            // Process is alive — report the conflict and exit.
            char errbuf[128];
            snprintf(errbuf, sizeof(errbuf),
                     "Another monitor is already running with PID %d — aborting",
                     (int)existing);
            emit("ERROR", errbuf);
            // flush is implicit because emit() uses write(), not stdio
            return 1;
        }
        // Stale PID file — safe to overwrite
    }
 
    // write our own PID file 
    write_pid_file();
 
    // announce ourselves
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Monitor started — PID=%d written to %s",
                 (int)getpid(), MONITOR_PID_FILE);
        emit("READY", buf);
    }
 
    // set up signal handlers
    struct sigaction sa_usr1, sa_int;
 
    sa_usr1.sa_handler = handler_sigusr1;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa_usr1, NULL) < 0) {
        emit("ERROR", "sigaction(SIGUSR1) failed");
        delete_pid_file();
        return 1;
    }
 
    sa_int.sa_handler = handler_sigint;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    if (sigaction(SIGINT, &sa_int, NULL) < 0) {
        emit("ERROR", "sigaction(SIGINT) failed");
        delete_pid_file();
        return 1;
    }
 
    emit("INFO", "Waiting for signals (SIGUSR1=new report, SIGINT=quit)");
 
    // main loop
    while (1) {
        pause();   // suspend until any signal arrives
 
        char tbuf[32];
        get_timestamp(tbuf, sizeof(tbuf));
 
        if (got_sigusr1) {
            got_sigusr1 = 0;
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[%s] SIGUSR1 received — a new report was added", tbuf);
            emit("INFO", buf);
        }
 
        if (got_sigint) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[%s] SIGINT received — shutting down", tbuf);
            emit("QUIT", buf);
            break;
        }
    }
 
    // cleanup
    delete_pid_file();
    emit("QUIT", ".monitor_pid deleted — goodbye");
    return 0;
}