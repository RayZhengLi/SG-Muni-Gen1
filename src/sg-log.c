// log.c
#include "sg-log.h"
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

#ifndef PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE_NP
#endif

static FILE *log_fp = NULL;
static char log_filename[256] = {0};
static size_t log_max_size = 0;
static pthread_mutex_t log_mutex;

static void log_rotate_if_needed() {
    struct stat st;
    if (stat(log_filename, &st) == 0) {
        if ((size_t)st.st_size >= log_max_size) {
            fclose(log_fp);
            log_fp = fopen(log_filename, "w");
            if (!log_fp) {
                perror("Failed to rotate log file");
            }
        }
    }
}

static void log_write(LogLevel level, const char *format, va_list args) {
    if (!log_fp) return;

    pthread_mutex_lock(&log_mutex);

    log_rotate_if_needed();

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

    const char *level_str = "";
    switch (level) {
        case LOG_LEVEL_DEBUG: level_str = "DEBUG"; break;
        case LOG_LEVEL_INFO:  level_str = "INFO";  break;
        case LOG_LEVEL_ERROR: level_str = "ERROR"; break;
    }

    fprintf(log_fp, "[%s] [%s] ", timebuf, level_str);
    vfprintf(log_fp, format, args);
    fprintf(log_fp, "\n");
    fflush(log_fp);

    pthread_mutex_unlock(&log_mutex);
}

int log_init(const char *filename, size_t max_size) {
    if (!filename) return -1;
    strncpy(log_filename, filename, sizeof(log_filename) - 1);
    log_max_size = max_size;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&log_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    log_fp = fopen(log_filename, "a");
    if (!log_fp) {
        perror("Failed to open log file");
        return -1;
    }
    return 0;
}

void log_close(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    pthread_mutex_unlock(&log_mutex);

    pthread_mutex_destroy(&log_mutex);
}

void log_debug(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_write(LOG_LEVEL_DEBUG, format, args);
    va_end(args);
}

void log_info(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_write(LOG_LEVEL_INFO, format, args);
    va_end(args);
}

void log_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_write(LOG_LEVEL_ERROR, format, args);
    va_end(args);
}
