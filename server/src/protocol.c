/*
 * protocol.c — serialización campo por campo en orden de red.
 *
 * Decisiones diferidas de la Fase 1 resueltas aquí
 * (trazabilidad-requerimientos.md § 6):
 *   - 64 bits: no existe htonll estándar, así que se hacen dos htonl sobre
 *     las mitades alta y baja.
 *   - float: se copia a un uint32_t con memcpy (sin violar aliasing) y se
 *     convierte con htonl como cualquier entero de 4 bytes.
 */
#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>

void header_encode(uint8_t out[HEADER_SIZE], const msg_header_t *h)
{
    writer_t w;
    wr_init(&w, out, HEADER_SIZE);
    wr_u8(&w, h->version);
    wr_u8(&w, h->msg_type);
    wr_u32(&w, h->entity_id);
    wr_u32(&w, h->payload_length);
}

void header_decode(const uint8_t in[HEADER_SIZE], msg_header_t *h)
{
    reader_t r;
    rd_init(&r, in, HEADER_SIZE);
    h->version        = rd_u8(&r);
    h->msg_type       = rd_u8(&r);
    h->entity_id      = rd_u32(&r);
    h->payload_length = rd_u32(&r);
}

/* ---------- lector ---------- */

void rd_init(reader_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

static int rd_need(reader_t *r, size_t n)
{
    if (r->err || r->len - r->pos < n) {
        r->err = 1;
        return 0;
    }
    return 1;
}

uint8_t rd_u8(reader_t *r)
{
    if (!rd_need(r, 1)) return 0;
    return r->buf[r->pos++];
}

uint16_t rd_u16(reader_t *r)
{
    uint16_t v;
    if (!rd_need(r, 2)) return 0;
    memcpy(&v, r->buf + r->pos, 2);
    r->pos += 2;
    return ntohs(v);
}

uint32_t rd_u32(reader_t *r)
{
    uint32_t v;
    if (!rd_need(r, 4)) return 0;
    memcpy(&v, r->buf + r->pos, 4);
    r->pos += 4;
    return ntohl(v);
}

uint64_t rd_u64(reader_t *r)
{
    uint64_t hi = rd_u32(r);
    uint64_t lo = rd_u32(r);
    return (hi << 32) | lo;
}

float rd_f32(reader_t *r)
{
    uint32_t bits = rd_u32(r);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

const uint8_t *rd_bytes(reader_t *r, size_t n)
{
    const uint8_t *p;
    if (!rd_need(r, n)) return NULL;
    p = r->buf + r->pos;
    r->pos += n;
    return p;
}

int rd_done(const reader_t *r)
{
    return !r->err && r->pos == r->len;
}

/* ---------- escritor ---------- */

void wr_init(writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = 0;
    w->err = 0;
}

static int wr_need(writer_t *w, size_t n)
{
    if (w->err || w->cap - w->pos < n) {
        w->err = 1;
        return 0;
    }
    return 1;
}

void wr_u8(writer_t *w, uint8_t v)
{
    if (!wr_need(w, 1)) return;
    w->buf[w->pos++] = v;
}

void wr_u16(writer_t *w, uint16_t v)
{
    uint16_t n = htons(v);
    wr_bytes(w, &n, 2);
}

void wr_u32(writer_t *w, uint32_t v)
{
    uint32_t n = htonl(v);
    wr_bytes(w, &n, 4);
}

void wr_u64(writer_t *w, uint64_t v)
{
    wr_u32(w, (uint32_t)(v >> 32));
    wr_u32(w, (uint32_t)(v & 0xffffffffu));
}

void wr_f32(writer_t *w, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    wr_u32(w, bits);
}

void wr_bytes(writer_t *w, const void *p, size_t n)
{
    if (!wr_need(w, n)) return;
    memcpy(w->buf + w->pos, p, n);
    w->pos += n;
}

/* ---------- nombres para el log ---------- */

const char *msg_type_name(uint8_t t)
{
    switch (t) {
    case MSG_REGISTER:  return "REGISTER";
    case MSG_EVENT:     return "EVENT";
    case MSG_TELEMETRY: return "TELEMETRY";
    case MSG_QUERY:     return "QUERY";
    case MSG_RESP:      return "RESP";
    case MSG_ERROR:     return "ERROR";
    case MSG_AUTH:      return "AUTH";
    default:            return "???";
    }
}

const char *query_type_name(uint8_t t)
{
    switch (t) {
    case Q_STATUS:     return "STATUS";
    case Q_HISTORY:    return "HISTORY";
    case Q_LIST_NODES: return "LIST_NODES";
    case Q_EVENTS:     return "EVENTS";
    case Q_PURGE_NODE: return "PURGE_NODE";
    default:           return "???";
    }
}

const char *error_code_name(uint8_t c)
{
    switch (c) {
    case ERR_AUTH_FAILED:       return "AUTH_FAILED";
    case ERR_UNKNOWN_NODE:      return "UNKNOWN_NODE";
    case ERR_MALFORMED_MSG:     return "MALFORMED_MSG";
    case ERR_UNSUPPORTED_QUERY: return "UNSUPPORTED_QUERY";
    case ERR_SESSION_EXPIRED:   return "SESSION_EXPIRED";
    case ERR_INTERNAL_ERROR:    return "INTERNAL_ERROR";
    case ERR_VERSION_MISMATCH:  return "VERSION_MISMATCH";
    case ERR_NAME_IN_USE:       return "NAME_IN_USE";
    case ERR_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ERR_INVALID_PARAM:     return "INVALID_PARAM";
    default:                    return "???";
    }
}

const char *node_state_name(uint8_t s)
{
    switch (s) {
    case NODE_REGISTERED:   return "REGISTERED";
    case NODE_ACTIVE:       return "ACTIVE";
    case NODE_STALE:        return "STALE";
    case NODE_DISCONNECTED: return "DISCONNECTED";
    default:                return "???";
    }
}

const char *profile_name(uint8_t p)
{
    switch (p) {
    case PROFILE_VIEWER: return "VIEWER";
    case PROFILE_ADMIN:  return "ADMIN";
    default:             return "???";
    }
}
