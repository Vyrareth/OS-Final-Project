#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

static FILE           *log_fp   = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *filepath) {
    pthread_mutex_lock(&log_lock);
    log_fp = filepath ? fopen(filepath, "a") : stderr;
    if (!log_fp) log_fp = stderr;
    pthread_mutex_unlock(&log_lock);
}

void log_msg(LogLevel level, const char *fmt, ...) {
    static const char *labels[] = { "INFO", "WARN", "ERROR" };
    time_t now = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&now));

    va_list ap;
    va_start(ap, fmt);

    pthread_mutex_lock(&log_lock);
    FILE *fp = log_fp ? log_fp : stderr;
    fprintf(fp, "[%s][%s] ", tbuf, labels[level]);
    vfprintf(fp, fmt, ap);
    fprintf(fp, "\n");
    fflush(fp);
    pthread_mutex_unlock(&log_lock);

    va_end(ap);
}

void log_close(void) {
    pthread_mutex_lock(&log_lock);
    if (log_fp && log_fp != stderr) fclose(log_fp);
    log_fp = NULL;
    pthread_mutex_unlock(&log_lock);
}
