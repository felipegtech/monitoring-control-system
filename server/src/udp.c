/*
 * udp.c — hilo dedicado al único socket SOCK_DGRAM por el que llega toda la
 * telemetría (maquinas-de-estado.md § 4, "Contraparte en UDP").
 *
 * Un datagrama conserva sus fronteras, así que un recvfrom() entrega el
 * mensaje completo o nada. Nunca se responde por UDP: un datagrama inválido
 * se descarta y queda registrado en el log.
 */
#include "server.h"
#include "logger.h"
#include "protocol.h"
#include "registry.h"

#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void drop(const char *peer, const msg_header_t *h, size_t got,
                 const char *reason)
{
    if (h)
        log_line(LOG_WARN, "UDP", peer, "RX", msg_type_name(h->msg_type),
                 "entity_id=%u len=%u bytes=%zu dropped=1 reason=\"%s\"",
                 h->entity_id, h->payload_length, got, reason);
    else
        log_line(LOG_WARN, "UDP", peer, "RX", "???",
                 "bytes=%zu dropped=1 reason=\"%s\"", got, reason);
}

void *udp_thread(void *arg)
{
    int fd = *(int *)arg;
    uint8_t buf[2048];

    for (;;) {
        struct sockaddr_storage from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0,
                             (struct sockaddr *)&from, &flen);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_line(LOG_ERROR, "UDP", "-", "--", "RECV_FAIL",
                     "reason=\"%s\"", strerror(errno));
            continue;
        }
        time_t now = time(NULL);   /* única fuente de tiempo (invariante 5) */
        char peer[PEER_STR_LEN];
        peer_to_str((struct sockaddr *)&from, peer);

        if ((size_t)n < HEADER_SIZE) {
            drop(peer, NULL, (size_t)n, "datagrama menor que el encabezado");
            continue;
        }
        msg_header_t h;
        header_decode(buf, &h);

        if (h.version != PROTO_VERSION) {
            drop(peer, &h, (size_t)n, "version no soportada");
            continue;
        }
        if (h.msg_type != MSG_TELEMETRY) {
            drop(peer, &h, (size_t)n, "solo TELEMETRY viaja por UDP");
            continue;
        }
        if (h.payload_length != TELEMETRY_PAYLOAD_SIZE ||
            (size_t)n != HEADER_SIZE + TELEMETRY_PAYLOAD_SIZE) {
            drop(peer, &h, (size_t)n,
                 "payload_length no coincide con TELEMETRY (13 bytes)");
            continue;
        }

        reader_t r;
        rd_init(&r, buf + HEADER_SIZE, TELEMETRY_PAYLOAD_SIZE);
        uint8_t metric = rd_u8(&r);
        uint64_t node_ts = rd_u64(&r);
        float value = rd_f32(&r);

        if (!isfinite(value)) {
            drop(peer, &h, (size_t)n, "value no es un numero finito");
            continue;
        }

        uint8_t old;
        int rc = reg_telemetry(h.entity_id, value, now, &old);
        if (rc == REG_UNKNOWN) {
            drop(peer, &h, (size_t)n, "entity_id sin entrada en el registro");
            continue;
        }
        if (rc == REG_DISCONNECTED) {
            drop(peer, &h, (size_t)n, "nodo DISCONNECTED");
            continue;
        }

        log_line(LOG_INFO, "UDP", peer, "RX", "TELEMETRY",
                 "entity_id=%u len=%u metric_type=%u node_timestamp=%llu "
                 "value=%.2f", h.entity_id, h.payload_length, metric,
                 (unsigned long long)node_ts, value);
        if (old != NODE_ACTIVE)
            log_line(LOG_INFO, "SYS", "-", "--", "STATE_CHANGE",
                     "node_id=%u from=%s to=ACTIVE", h.entity_id,
                     node_state_name(old));
    }
    return NULL;
}
