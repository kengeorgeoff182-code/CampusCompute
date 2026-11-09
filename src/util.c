/*
 * util.c - small helpers shared by every CampusCompute module.
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int cc_parse_long(const char *s, long min, long max, long *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0' || isspace((unsigned char)*s))
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return -1;
    if (errno == ERANGE || v < min || v > max)
        return -2;
    *out = v;
    return 0;
}

void cc_errf(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    fputs("error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void cc_sleep_ms(long ms)
{
    struct timespec req, rem;

    if (ms <= 0)
        return;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

void cc_format_timestamp(const struct timespec *ts, char *buf, size_t len)
{
    struct tm tmv;
    char base[32];

    if (localtime_r(&ts->tv_sec, &tmv) == NULL) {
        snprintf(buf, len, "0000-00-00 00:00:00.000");
        return;
    }
    strftime(base, sizeof base, "%Y-%m-%d %H:%M:%S", &tmv);
    snprintf(buf, len, "%s.%03ld", base, ts->tv_nsec / 1000000L);
}

void cc_rule(char c, int width)
{
    for (int i = 0; i < width; i++)
        putchar(c);
    putchar('\n');
}

void cc_banner(const char *title)
{
    putchar('\n');
    cc_rule('=', 72);
    printf("  %s\n", title);
    cc_rule('=', 72);
}

int cc_ensure_parent_dir(const char *path, char *err, size_t errlen)
{
    char dir[512];
    const char *slash = strrchr(path, '/');
    struct stat st;
    size_t n;

    if (slash == NULL || slash == path)
        return 0;               /* current directory or filesystem root */
    n = (size_t)(slash - path);
    if (n >= sizeof dir) {
        snprintf(err, errlen, "path too long: %s", path);
        return -1;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';

    if (stat(dir, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        snprintf(err, errlen, "'%s' exists but is not a directory", dir);
        return -1;
    }
    if (errno != ENOENT || mkdir(dir, 0755) != 0) {
        snprintf(err, errlen, "cannot create directory '%s': %s", dir,
                 strerror(errno));
        return -1;
    }
    return 0;
}

void cc_write_str(int fd, const char *s)
{
    size_t left = strlen(s);

    while (left > 0) {
        ssize_t w = write(fd, s, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        s += w;
        left -= (size_t)w;
    }
}
