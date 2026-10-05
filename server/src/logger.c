/*
 * logger.c — implementa las reglas de docs/logging.md:
 *   - la línea se compone completa en un búfer local, fuera del candado;
 *   - se escribe en stdout y en el archivo dentro de un mutex propio;
 *   - fflush() en cada línea;
 *   - un fallo de escritura en ejecución se reporta en stderr y no detiene
 *     el servicio.
 */
#include "logger.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static FILE *log_fp = NULL;
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;

int log_init(const char *path)
{
    log_fp = fopen(path, "a");
    return log_fp ? 0 : -1;
}

void log_close(void)
{
    pthread_mutex_lock(&log_mtx);
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    pthread_mutex_unlock(&log_mtx);
}

/* ISO 8601 con milisegundos y desplazamiento "-05:00". */
static void format_timestamp(char *out, size_t cap)
{
    struct timeval tv;
    struct tm tm;
    char base[32], zone[8];

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
    strftime(zone, sizeof zone, "%z", &tm);           /* "-0500" */
    snprintf(out, cap, "%.20s.%03d%.3s:%.2s", base, (int)(tv.tv_usec / 1000) % 1000,
             zone, zone + 3);
}

static const char *level_name(log_level_t lvl)
{
    switch (lvl) {
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "INFO";
    }
}

void log_line(log_level_t lvl, const char *transport, const char *peer,
              const char *dir, const char *event, const char *fmt, ...)
{
    char line[2048];
    char ts[48];
    int n;

    format_timestamp(ts, sizeof ts);
    n = snprintf(line, sizeof line, "%s %s %s %s %s %s", ts, level_name(lvl),
                 transport, peer, dir, event);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;

    if (fmt && (size_t)n < sizeof line - 2) {
        va_list ap;
        line[n++] = ' ';
        va_start(ap, fmt);
        int m = vsnprintf(line + n, sizeof line - n, fmt, ap);
        va_end(ap);
        if (m > 0) n += m;
        if ((size_t)n >= sizeof line) n = sizeof line - 1;
    }
    line[n] = '\0';

    pthread_mutex_lock(&log_mtx);
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    if (log_fp) {
        if (fprintf(log_fp, "%s\n", line) < 0 || fflush(log_fp) != 0)
            fprintf(stderr, "logger: fallo al escribir el archivo de log\n");
    }
    pthread_mutex_unlock(&log_mtx);
}

void peer_to_str(const struct sockaddr *sa, char out[PEER_STR_LEN])
{
    char ip[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof ip);
        port = ntohs(s4->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        port = ntohs(s6->sin6_port);
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            /* El socket dual-stack entrega IPv4 como ::ffff:a.b.c.d */
            inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], ip, sizeof ip);
        } else {
            char ip6[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &s6->sin6_addr, ip6, sizeof ip6);
            snprintf(ip, sizeof ip, "[%.*s]", (int)sizeof ip - 3, ip6);
        }
    }
    snprintf(out, PEER_STR_LEN, "%s:%u", ip, port);
}

void log_quote(char *dst, size_t cap, const void *src, size_t len,
               size_t max_chars)
{
    const unsigned char *s = src;
    size_t o = 0, i;
    int truncated = len > max_chars;

    if (cap < 8) {
        if (cap) dst[0] = '\0';
        return;
    }
    if (truncated) len = max_chars;

    dst[o++] = '"';
    for (i = 0; i < len && o + 6 < cap; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\r' || c == '\t' || c < 0x20 || c == 0x7f) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    if (truncated && o + 5 < cap) {
        memcpy(dst + o, "\xe2\x80\xa6", 3);   /* "…" en UTF-8 */
        o += 3;
    }
    dst[o++] = '"';
    dst[o] = '\0';
}
