/*
 * logger.h — log en consola y archivo con el formato de docs/logging.md:
 *
 *   <timestamp> <nivel> <transporte> <ip>:<puerto> <dirección> <evento> [detalle]
 */
#ifndef MCS_LOGGER_H
#define MCS_LOGGER_H

#include <stddef.h>
#include <sys/socket.h>

typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

#define PEER_STR_LEN 64   /* "[ipv6]:puerto" cabe holgado */

/* Abre el archivo en modo append. Devuelve 0 o -1 (errno conservado). */
int  log_init(const char *path);
void log_close(void);

/*
 * Escribe una línea completa. `transport` es "TCP", "UDP" o "SYS"; `peer`
 * es "ip:puerto" o "-"; `dir` es "RX", "TX" o "--". `fmt` es el detalle
 * (pares clave=valor) y puede ser NULL.
 */
void log_line(log_level_t lvl, const char *transport, const char *peer,
              const char *dir, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

/* Convierte una dirección de socket a "ip:puerto" (IPv4 mapeada → IPv4). */
void peer_to_str(const struct sockaddr *sa, char out[PEER_STR_LEN]);

/*
 * Copia texto libre entre comillas, escapando `"` y `\` y sustituyendo
 * saltos de línea por `\n` literal; trunca a `max_chars` bytes de origen
 * seguidos de "…" (logging.md, reglas 3 y 4).
 */
void log_quote(char *dst, size_t cap, const void *src, size_t len,
               size_t max_chars);

#endif
