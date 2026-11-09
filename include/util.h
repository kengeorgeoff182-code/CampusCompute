/*
 * util.h - small helpers shared by every CampusCompute module.
 */
#ifndef CC_UTIL_H
#define CC_UTIL_H

#include <stddef.h>
#include <time.h>

#define CC_VERSION "1.0.0"

/* Process exit codes used by the command-line interface. */
#define CC_EXIT_OK    0   /* command completed                         */
#define CC_EXIT_FAIL  1   /* command ran but a check/operation failed  */
#define CC_EXIT_USAGE 2   /* bad command line / malformed input        */

/*
 * Strictly parse a base-10 integer. No leading/trailing junk is accepted.
 * Returns 0 on success, -1 if 's' is not an integer, -2 if it is outside
 * [min, max].
 */
int cc_parse_long(const char *s, long min, long max, long *out);

/* Print "error: <message>" to stderr (printf-style). */
void cc_errf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Sleep for 'ms' milliseconds, resuming after EINTR. */
void cc_sleep_ms(long ms);

/* "YYYY-MM-DD HH:MM:SS.mmm" in local time. */
void cc_format_timestamp(const struct timespec *ts, char *buf, size_t len);

/* Section banner / horizontal rule helpers for consistent output. */
void cc_banner(const char *title);
void cc_rule(char c, int width);

/*
 * Make sure the directory part of 'path' exists (one level only).
 * Returns 0 on success, -1 with a message in 'err' on failure.
 */
int cc_ensure_parent_dir(const char *path, char *err, size_t errlen);

/* write(2) the whole string to fd, ignoring errors (async-signal-safe). */
void cc_write_str(int fd, const char *s);

#endif /* CC_UTIL_H */
