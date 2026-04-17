#ifndef LOG_H
#define LOG_H

typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

/*
 * log_init  — call once at program startup (pass a filepath or NULL for stderr)
 * log_msg   — thread-safe; can be called from any thread at any time
 * log_close — flush and close the log file
 */
void log_init(const char *filepath);
void log_msg(LogLevel level, const char *fmt, ...);
void log_close(void);

#define LOG_INFO(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif /* LOG_H */
