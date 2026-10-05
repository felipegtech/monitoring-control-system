/*
 * conn.c — un hilo por conexión TCP.
 *
 * Implementa:
 *   - la máquina de recepción sobre TCP (maquinas-de-estado.md § 4): leer
 *     exactamente 10 bytes de encabezado, validar version y payload_length
 *     ANTES de reservar nada, leer exactamente payload_length bytes,
 *     procesar, responder exactamente un RESP o un ERROR y volver a leer;
 *   - la sesión de cliente administrador (§ 3): AUTH con PAM, tres intentos,
 *     QUERY solo con session_id vivo;
 *   - el lado TCP del nodo (§ 2): REGISTER, EVENT y DISCONNECTED al cerrar.
 *
 * El rol de la conexión (nodo o cliente) lo fija el primer mensaje aceptado:
 * REGISTER la vuelve conexión de nodo, AUTH la vuelve conexión de cliente.
 */
#include "server.h"
#include "auth.h"
#include "logger.h"
#include "protocol.h"
#include "registry.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef enum { ROLE_NONE, ROLE_NODE, ROLE_CLIENT } conn_role_t;

typedef struct {
    int         fd;
    char        peer[PEER_STR_LEN];
    conn_role_t role;
    uint32_t    node_id;       /* != 0 cuando el nodo está registrado */
    uint32_t    session_id;    /* != 0 cuando el cliente está autenticado */
    uint8_t     profile;
    int         auth_failures;
    char        username[256];
    int         close_now;     /* la máquina de sesión pidió cerrar */
    const char *close_reason;
} conn_t;

static pthread_mutex_t conns_mtx = PTHREAD_MUTEX_INITIALIZER;
static int open_conns = 0;
static uint32_t next_session_id = 1000;

int conn_open_count(void)
{
    pthread_mutex_lock(&conns_mtx);
    int n = open_conns;
    pthread_mutex_unlock(&conns_mtx);
    return n;
}

/* ------------------------------------------------------------------ E/S */

/* 1 = completo, 0 = el par cerró, -1 = error (errno), -2 = timeout */
static int recv_full(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR && !g_shutdown) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            return -1;
        }
        got += (size_t)r;
    }
    return 1;
}

static int send_full(int fd, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

static void set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { seconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* Envía encabezado + payload en una sola escritura lógica. */
static int send_msg(conn_t *c, uint8_t type, uint32_t entity,
                    const uint8_t *payload, size_t len)
{
    uint8_t buf[HEADER_SIZE + MAX_PAYLOAD];
    msg_header_t h = { PROTO_VERSION, type, entity, (uint32_t)len };

    header_encode(buf, &h);
    memcpy(buf + HEADER_SIZE, payload, len);
    if (send_full(c->fd, buf, HEADER_SIZE + len) < 0) {
        log_line(LOG_WARN, "TCP", c->peer, "--", "SEND_FAIL",
                 "reason=\"%s\"", strerror(errno));
        c->close_now = 1;
        c->close_reason = "send_failed";
        return -1;
    }
    return 0;
}

/* RESP con `body` ya serializado; `fmt` agrega detalle al log. */
static void send_resp(conn_t *c, uint32_t entity, uint8_t in_resp_to,
                      const uint8_t *body, size_t body_len,
                      const char *fmt, ...)
{
    uint8_t payload[MAX_PAYLOAD];
    char detail[512] = "";

    payload[0] = in_resp_to;
    if (body_len)
        memcpy(payload + 1, body, body_len);

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
    }
    if (send_msg(c, MSG_RESP, entity, payload, body_len + 1) == 0)
        log_line(LOG_INFO, "TCP", c->peer, "TX", "RESP",
                 "entity_id=%u len=%zu in_response_to=%s%s%s", entity,
                 body_len + 1, msg_type_name(in_resp_to), *detail ? " " : "",
                 detail);
}

static void send_error(conn_t *c, uint32_t entity, uint8_t in_resp_to,
                       uint8_t code, const char *text, const char *extra)
{
    uint8_t payload[3 + 255];
    size_t tlen = strlen(text);
    char quoted[600];

    if (tlen > 255) tlen = 255;
    payload[0] = in_resp_to;
    payload[1] = code;
    payload[2] = (uint8_t)tlen;
    memcpy(payload + 3, text, tlen);

    log_quote(quoted, sizeof quoted, text, tlen, 200);
    if (send_msg(c, MSG_ERROR, entity, payload, 3 + tlen) == 0) {
        const char *rname = msg_type_name(in_resp_to);
        char rbuf[8];
        if (strcmp(rname, "???") == 0) {   /* tipo desconocido: valor crudo */
            snprintf(rbuf, sizeof rbuf, "%u", in_resp_to);
            rname = rbuf;
        }
        log_line(code == ERR_INTERNAL_ERROR ? LOG_ERROR : LOG_WARN, "TCP",
                 c->peer, "TX", "ERROR",
                 "entity_id=%u len=%zu in_response_to=%s error_code=%s "
                 "message=%s%s%s", entity, 3 + tlen, rname,
                 error_code_name(code), quoted, extra ? " " : "",
                 extra ? extra : "");
    }
}

static void request_close(conn_t *c, const char *reason)
{
    c->close_now = 1;
    c->close_reason = reason;
}

/* -------------------------------------------------------------- REGISTER */

static void handle_register(conn_t *c, const msg_header_t *h,
                            const uint8_t *p)
{
    reader_t r;
    rd_init(&r, p, h->payload_length);
    uint8_t name_len = rd_u8(&r);
    const uint8_t *name = rd_bytes(&r, name_len);
    uint8_t node_type = rd_u8(&r);
    uint16_t interval = rd_u16(&r);

    if (!rd_done(&r) || name_len == 0) {
        log_line(LOG_WARN, "TCP", c->peer, "RX", "REGISTER",
                 "entity_id=%u len=%u reason=\"payload no interpretable\"",
                 h->entity_id, h->payload_length);
        send_error(c, h->entity_id, MSG_REGISTER, ERR_MALFORMED_MSG,
                   "payload de REGISTER mal formado", NULL);
        return;
    }

    char qname[600];
    log_quote(qname, sizeof qname, name, name_len, 200);
    log_line(LOG_INFO, "TCP", c->peer, "RX", "REGISTER",
             "entity_id=%u len=%u node_name=%s node_type=%u interval=%u",
             h->entity_id, h->payload_length, qname, node_type, interval);

    if (c->role == ROLE_CLIENT) {
        send_error(c, h->entity_id, MSG_REGISTER, ERR_MALFORMED_MSG,
                   "REGISTER no permitido en una conexion de cliente", NULL);
        return;
    }
    if (c->node_id != 0) {
        send_error(c, h->entity_id, MSG_REGISTER, ERR_MALFORMED_MSG,
                   "el nodo ya esta registrado en esta conexion", NULL);
        return;
    }
    for (unsigned i = 0; i < name_len; i++) {
        if (name[i] < 0x20 || name[i] == 0x7f) {
            send_error(c, h->entity_id, MSG_REGISTER, ERR_INVALID_PARAM,
                       "node_name contiene caracteres de control", NULL);
            return;
        }
    }

    uint32_t id;
    int rc = reg_register((const char *)name, name_len, node_type, interval,
                          &id);
    if (rc == REG_NAME_IN_USE) {
        send_error(c, h->entity_id, MSG_REGISTER, ERR_NAME_IN_USE,
                   "node_name en uso por un nodo conectado", NULL);
        return;
    }
    if (rc != REG_OK) {
        log_line(LOG_ERROR, "SYS", "-", "--", "REGISTRY_FULL",
                 "max_nodes=%d", MAX_NODES);
        send_error(c, h->entity_id, MSG_REGISTER, ERR_INTERNAL_ERROR,
                   "registro de nodos lleno", NULL);
        return;
    }

    c->role = ROLE_NODE;
    c->node_id = id;
    set_recv_timeout(c->fd, 0);   /* un nodo puede pasar horas sin EVENT */

    uint8_t body[4];
    writer_t w;
    wr_init(&w, body, sizeof body);
    wr_u32(&w, id);
    send_resp(c, id, MSG_REGISTER, body, w.pos, "assigned_node_id=%u", id);
    log_line(LOG_INFO, "SYS", "-", "--", "STATE_CHANGE",
             "node_id=%u from=- to=REGISTERED", id);
}

/* ----------------------------------------------------------------- EVENT */

static void handle_event(conn_t *c, const msg_header_t *h, const uint8_t *p)
{
    reader_t r;
    rd_init(&r, p, h->payload_length);
    uint8_t code = rd_u8(&r);
    uint8_t severity = rd_u8(&r);
    uint64_t node_ts = rd_u64(&r);
    uint16_t mlen = rd_u16(&r);
    const uint8_t *msg = rd_bytes(&r, mlen);

    if (!rd_done(&r)) {
        log_line(LOG_WARN, "TCP", c->peer, "RX", "EVENT",
                 "entity_id=%u len=%u reason=\"payload no interpretable\"",
                 h->entity_id, h->payload_length);
        send_error(c, h->entity_id, MSG_EVENT, ERR_MALFORMED_MSG,
                   "payload de EVENT mal formado", NULL);
        return;
    }

    char qmsg[600];
    log_quote(qmsg, sizeof qmsg, msg, mlen, 200);
    log_line(severity >= 2 ? LOG_WARN : LOG_INFO, "TCP", c->peer, "RX",
             "EVENT", "entity_id=%u len=%u event_code=%u severity=%u "
             "node_timestamp=%llu message=%s", h->entity_id,
             h->payload_length, code, severity, (unsigned long long)node_ts,
             qmsg);

    if (c->role == ROLE_CLIENT) {
        send_error(c, h->entity_id, MSG_EVENT, ERR_MALFORMED_MSG,
                   "EVENT no permitido en una conexion de cliente", NULL);
        return;
    }
    if (c->node_id == 0 || h->entity_id != c->node_id) {
        send_error(c, h->entity_id, MSG_EVENT, ERR_UNKNOWN_NODE,
                   "node_id desconocido, vuelva a enviar REGISTER", NULL);
        return;
    }
    if (severity > 2) {
        send_error(c, h->entity_id, MSG_EVENT, ERR_INVALID_PARAM,
                   "severity fuera de rango (0..2)", NULL);
        return;
    }
    if (reg_event(c->node_id, code, severity, node_ts, msg, mlen,
                  time(NULL)) != REG_OK) {
        send_error(c, h->entity_id, MSG_EVENT, ERR_UNKNOWN_NODE,
                   "node_id desconocido, vuelva a enviar REGISTER", NULL);
        return;
    }
    send_resp(c, c->node_id, MSG_EVENT, NULL, 0, NULL);
}

/* ------------------------------------------------------------------ AUTH */

static void handle_auth(conn_t *c, const msg_header_t *h, const uint8_t *p)
{
    reader_t r;
    rd_init(&r, p, h->payload_length);
    uint8_t ulen = rd_u8(&r);
    const uint8_t *user = rd_bytes(&r, ulen);
    uint8_t plen = rd_u8(&r);
    const uint8_t *pass = rd_bytes(&r, plen);

    if (!rd_done(&r) || ulen == 0 || memchr(user, 0, ulen) ||
        memchr(pass, 0, plen)) {
        log_line(LOG_WARN, "TCP", c->peer, "RX", "AUTH",
                 "entity_id=%u len=%u password=<redacted> "
                 "reason=\"payload no interpretable\"",
                 h->entity_id, h->payload_length);
        send_error(c, h->entity_id, MSG_AUTH, ERR_MALFORMED_MSG,
                   "payload de AUTH mal formado", NULL);
        return;
    }

    char username[256], password[256], quser[600];
    memcpy(username, user, ulen);
    username[ulen] = '\0';
    memcpy(password, pass, plen);
    password[plen] = '\0';
    log_quote(quser, sizeof quser, username, ulen, 200);

    if (c->role == ROLE_NODE) {
        log_line(LOG_WARN, "TCP", c->peer, "RX", "AUTH",
                 "entity_id=%u len=%u username=%s password=<redacted>",
                 h->entity_id, h->payload_length, quser);
        send_error(c, h->entity_id, MSG_AUTH, ERR_MALFORMED_MSG,
                   "AUTH no permitido en una conexion de nodo", NULL);
        memset(password, 0, sizeof password);
        return;
    }
    if (c->session_id != 0) {
        log_line(LOG_WARN, "TCP", c->peer, "RX", "AUTH",
                 "entity_id=%u len=%u username=%s password=<redacted>",
                 h->entity_id, h->payload_length, quser);
        send_error(c, c->session_id, MSG_AUTH, ERR_MALFORMED_MSG,
                   "la sesion ya esta autenticada", NULL);
        memset(password, 0, sizeof password);
        return;
    }

    c->role = ROLE_CLIENT;
    uint8_t profile = PROFILE_VIEWER;
    char why[256] = "";
    int rc = auth_check(username, password, &profile, why, sizeof why);
    memset(password, 0, sizeof password);

    log_line(rc == AUTH_OK ? LOG_INFO : LOG_WARN, "TCP", c->peer, "RX",
             "AUTH", "entity_id=%u len=%u username=%s password=<redacted>",
             h->entity_id, h->payload_length, quser);

    if (rc == AUTH_OK) {
        pthread_mutex_lock(&conns_mtx);
        c->session_id = next_session_id++;
        pthread_mutex_unlock(&conns_mtx);
        c->profile = profile;
        snprintf(c->username, sizeof c->username, "%s", username);
        set_recv_timeout(c->fd, CLIENT_IDLE_TIMEOUT);

        uint8_t body[5];
        writer_t w;
        wr_init(&w, body, sizeof body);
        wr_u32(&w, c->session_id);
        wr_u8(&w, profile);
        send_resp(c, c->session_id, MSG_AUTH, body, w.pos,
                  "session_id=%u profile=%s", c->session_id,
                  profile_name(profile));
        return;
    }

    c->auth_failures++;
    char extra[64];
    snprintf(extra, sizeof extra, "attempt=%d/%d", c->auth_failures,
             MAX_AUTH_ATTEMPTS);
    if (rc == AUTH_INTERNAL) {
        log_line(LOG_ERROR, "SYS", "-", "--", "AUTH_BACKEND",
                 "reason=\"%s\"", why);
        send_error(c, 0, MSG_AUTH, ERR_INTERNAL_ERROR,
                   "error interno de autenticacion", extra);
    } else {
        send_error(c, 0, MSG_AUTH, ERR_AUTH_FAILED,
                   "usuario o contrasena incorrectos", extra);
    }
    if (c->auth_failures >= MAX_AUTH_ATTEMPTS)
        request_close(c, "auth_attempts_exceeded");
}

/* ----------------------------------------------------------------- QUERY */

static void query_status(conn_t *c, uint32_t target)
{
    node_summary_t s;
    if (reg_status(target, &s) != REG_OK) {
        send_error(c, c->session_id, MSG_QUERY, ERR_UNKNOWN_NODE,
                   "target_node_id no existe en el registro", NULL);
        return;
    }
    uint8_t body[17];
    writer_t w;
    wr_init(&w, body, sizeof body);
    wr_u32(&w, s.id);
    wr_u8(&w, s.state);
    wr_u64(&w, s.last_timestamp);
    wr_f32(&w, s.last_value);
    send_resp(c, c->session_id, MSG_QUERY, body, w.pos,
              "query_type=STATUS node_id=%u state=%s", s.id,
              node_state_name(s.state));
}

static void query_history(conn_t *c, uint32_t target, uint16_t count)
{
    hist_point_t pts[HISTORY_CAP];
    size_t n;

    if (count == 0) count = 5;
    if (count > HISTORY_CAP) count = HISTORY_CAP;
    if (reg_history(target, count, pts, &n) != REG_OK) {
        send_error(c, c->session_id, MSG_QUERY, ERR_UNKNOWN_NODE,
                   "target_node_id no existe en el registro", NULL);
        return;
    }
    uint8_t body[2 + HISTORY_CAP * 12];
    writer_t w;
    wr_init(&w, body, sizeof body);
    wr_u16(&w, (uint16_t)n);
    for (size_t i = 0; i < n; i++) {
        wr_u64(&w, pts[i].timestamp);
        wr_f32(&w, pts[i].value);
    }
    send_resp(c, c->session_id, MSG_QUERY, body, w.pos,
              "query_type=HISTORY node_id=%u returned=%zu", target, n);
}

static void query_list(conn_t *c)
{
    node_summary_t *list = malloc(MAX_NODES * sizeof *list);
    uint8_t body[MAX_PAYLOAD - 1];
    writer_t w;
    size_t included = 0;

    if (!list) {
        log_line(LOG_ERROR, "SYS", "-", "--", "INTERNAL", "reason=\"malloc fallido\"");
        send_error(c, c->session_id, MSG_QUERY, ERR_INTERNAL_ERROR,
                   "error interno del servidor", NULL);
        return;
    }
    size_t n = reg_list(list, MAX_NODES);
    wr_init(&w, body, sizeof body);
    wr_u16(&w, 0);                          /* se corrige al final */
    for (size_t i = 0; i < n; i++) {
        size_t need = 4 + 1 + list[i].name_len + 1;
        if (w.pos + need > sizeof body) break;   /* acota a MAX_PAYLOAD */
        wr_u32(&w, list[i].id);
        wr_u8(&w, list[i].name_len);
        wr_bytes(&w, list[i].name, list[i].name_len);
        wr_u8(&w, list[i].state);
        included++;
    }
    free(list);
    body[0] = (uint8_t)(included >> 8);
    body[1] = (uint8_t)(included & 0xff);
    send_resp(c, c->session_id, MSG_QUERY, body, w.pos,
              "query_type=LIST_NODES returned=%zu", included);
}

static void query_events(conn_t *c, uint32_t target, uint16_t count)
{
    event_rec_t evs[EVENT_CAP];
    size_t n;

    if (count == 0) count = 5;
    if (count > EVENT_CAP) count = EVENT_CAP;
    if (reg_events(target, count, evs, &n) != REG_OK) {
        send_error(c, c->session_id, MSG_QUERY, ERR_UNKNOWN_NODE,
                   "target_node_id no existe en el registro", NULL);
        return;
    }
    uint8_t body[2 + EVENT_CAP * (19 + EVENT_MSG_KEEP)];
    writer_t w;
    wr_init(&w, body, sizeof body);
    wr_u16(&w, (uint16_t)n);
    for (size_t i = 0; i < n; i++) {
        wr_u64(&w, evs[i].server_ts);
        wr_u64(&w, evs[i].node_ts);
        wr_u8(&w, evs[i].code);
        wr_u8(&w, evs[i].severity);
        wr_u8(&w, evs[i].msg_len);
        wr_bytes(&w, evs[i].msg, evs[i].msg_len);
    }
    send_resp(c, c->session_id, MSG_QUERY, body, w.pos,
              "query_type=EVENTS node_id=%u returned=%zu", target, n);
}

static void query_purge(conn_t *c, uint32_t target)
{
    if (c->profile != PROFILE_ADMIN) {
        send_error(c, c->session_id, MSG_QUERY, ERR_PERMISSION_DENIED,
                   "PURGE_NODE requiere el perfil ADMIN", NULL);
        return;
    }
    int rc = reg_purge(target);
    if (rc == REG_UNKNOWN) {
        send_error(c, c->session_id, MSG_QUERY, ERR_UNKNOWN_NODE,
                   "target_node_id no existe en el registro", NULL);
        return;
    }
    if (rc == REG_NOT_PURGEABLE) {
        send_error(c, c->session_id, MSG_QUERY, ERR_INVALID_PARAM,
                   "solo se pueden purgar nodos DISCONNECTED", NULL);
        return;
    }
    send_resp(c, c->session_id, MSG_QUERY, NULL, 0,
              "query_type=PURGE_NODE node_id=%u", target);
    log_line(LOG_INFO, "SYS", "-", "--", "NODE_PURGED",
             "node_id=%u by=\"%s\"", target, c->username);
}

static void handle_query(conn_t *c, const msg_header_t *h, const uint8_t *p)
{
    reader_t r;
    rd_init(&r, p, h->payload_length);
    uint8_t qtype = rd_u8(&r);
    uint32_t target = rd_u32(&r);
    uint16_t count = rd_u16(&r);
    int ok = rd_done(&r);

    if (ok)
        log_line(LOG_INFO, "TCP", c->peer, "RX", "QUERY",
                 "entity_id=%u len=%u query_type=%s target_node_id=%u "
                 "count=%u", h->entity_id, h->payload_length,
                 query_type_name(qtype), target, count);
    else
        log_line(LOG_WARN, "TCP", c->peer, "RX", "QUERY",
                 "entity_id=%u len=%u reason=\"payload no interpretable\"",
                 h->entity_id, h->payload_length);

    /* QUERY antes de AUTH: SESSION_EXPIRED y cierre (§ 3). */
    if (c->session_id == 0) {
        send_error(c, h->entity_id, MSG_QUERY, ERR_SESSION_EXPIRED,
                   "se requiere AUTH antes de QUERY", NULL);
        if (c->role != ROLE_NODE)
            request_close(c, "query_before_auth");
        return;
    }
    if (h->entity_id != c->session_id) {
        send_error(c, h->entity_id, MSG_QUERY, ERR_SESSION_EXPIRED,
                   "session_id no corresponde a una sesion viva", NULL);
        return;
    }
    if (!ok) {
        send_error(c, c->session_id, MSG_QUERY, ERR_MALFORMED_MSG,
                   "payload de QUERY mal formado (se esperan 7 bytes)", NULL);
        return;
    }

    switch (qtype) {
    case Q_STATUS:     query_status(c, target);         break;
    case Q_HISTORY:    query_history(c, target, count); break;
    case Q_LIST_NODES: query_list(c);                   break;
    case Q_EVENTS:     query_events(c, target, count);  break;
    case Q_PURGE_NODE: query_purge(c, target);          break;
    default:
        send_error(c, c->session_id, MSG_QUERY, ERR_UNSUPPORTED_QUERY,
                   "query_type no reconocido", NULL);
    }
}

/* ------------------------------------------------------- bucle principal */

static void dispatch(conn_t *c, const msg_header_t *h, const uint8_t *p)
{
    switch (h->msg_type) {
    case MSG_REGISTER: handle_register(c, h, p); break;
    case MSG_EVENT:    handle_event(c, h, p);    break;
    case MSG_AUTH:     handle_auth(c, h, p);     break;
    case MSG_QUERY:    handle_query(c, h, p);    break;
    default:
        /* TELEMETRY no viaja por TCP; RESP/ERROR solo los emite el
         * servidor; cualquier otro valor no existe en el protocolo. */
        log_line(LOG_WARN, "TCP", c->peer, "RX", msg_type_name(h->msg_type),
                 "entity_id=%u len=%u msg_type=%u reason=\"msg_type no "
                 "valido sobre TCP\"", h->entity_id, h->payload_length,
                 h->msg_type);
        send_error(c, h->entity_id, h->msg_type, ERR_MALFORMED_MSG,
                   "msg_type no valido sobre TCP", NULL);
    }
}

static void *conn_thread(void *arg)
{
    conn_t *c = arg;
    uint8_t hdr_buf[HEADER_SIZE];
    uint8_t *payload = malloc(MAX_PAYLOAD);
    const char *reason = "peer_closed";

    if (!payload) {
        log_line(LOG_ERROR, "TCP", c->peer, "--", "CONN_CLOSE",
                 "reason=\"malloc fallido\"");
        goto out;
    }

    for (;;) {
        /* LEYENDO_HEADER */
        int rc = recv_full(c->fd, hdr_buf, HEADER_SIZE);
        if (rc != 1) {
            reason = rc == 0 ? "peer_closed" : rc == -2 ? "timeout" : "recv_error";
            break;
        }

        /* VALIDANDO */
        msg_header_t h;
        header_decode(hdr_buf, &h);
        if (h.version != PROTO_VERSION) {
            log_line(LOG_WARN, "TCP", c->peer, "RX",
                     msg_type_name(h.msg_type),
                     "entity_id=%u len=%u version=%u reason=\"version no "
                     "soportada\"", h.entity_id, h.payload_length, h.version);
            send_error(c, h.entity_id, h.msg_type, ERR_VERSION_MISMATCH,
                       "version de protocolo no soportada", NULL);
            reason = "version_mismatch";
            break;
        }
        if (h.payload_length > MAX_PAYLOAD) {
            log_line(LOG_WARN, "TCP", c->peer, "RX",
                     msg_type_name(h.msg_type),
                     "entity_id=%u len=%u reason=\"payload_length excede "
                     "MAX_PAYLOAD\"", h.entity_id, h.payload_length);
            send_error(c, h.entity_id, h.msg_type, ERR_MALFORMED_MSG,
                       "payload_length excede MAX_PAYLOAD", NULL);
            reason = "malformed";
            break;
        }

        /* LEYENDO_PAYLOAD */
        if (h.payload_length > 0) {
            rc = recv_full(c->fd, payload, h.payload_length);
            if (rc != 1) {
                reason = rc == 0 ? "peer_closed_mid_message"
                       : rc == -2 ? "timeout_mid_message" : "recv_error";
                log_line(LOG_WARN, "TCP", c->peer, "RX",
                         msg_type_name(h.msg_type),
                         "entity_id=%u len=%u reason=\"mensaje incompleto\"",
                         h.entity_id, h.payload_length);
                break;
            }
        }

        /* PROCESANDO: exactamente una respuesta */
        dispatch(c, &h, payload);
        if (c->close_now) {
            reason = c->close_reason;
            break;
        }
        if (g_shutdown) {
            reason = "shutdown";
            break;
        }
    }

out:
    if (c->node_id != 0) {
        int old = reg_disconnect(c->node_id);
        if (old >= 0)
            log_line(LOG_INFO, "SYS", "-", "--", "STATE_CHANGE",
                     "node_id=%u from=%s to=DISCONNECTED", c->node_id,
                     node_state_name((uint8_t)old));
    }
    log_line(LOG_INFO, "TCP", c->peer, "--", "CONN_CLOSE",
             "reason=\"%s\" role=%s", reason,
             c->role == ROLE_NODE ? "node" :
             c->role == ROLE_CLIENT ? "client" : "none");

    close(c->fd);
    free(payload);
    free(c);

    pthread_mutex_lock(&conns_mtx);
    open_conns--;
    pthread_mutex_unlock(&conns_mtx);
    return NULL;
}

int conn_spawn(int fd, const struct sockaddr *peer)
{
    conn_t *c = calloc(1, sizeof *c);
    char peer_str[PEER_STR_LEN];

    peer_to_str(peer, peer_str);
    if (!c) {
        log_line(LOG_ERROR, "TCP", peer_str, "--", "CONN_REJECT",
                 "reason=\"malloc fallido\"");
        close(fd);
        return -1;
    }
    c->fd = fd;
    memcpy(c->peer, peer_str, sizeof c->peer);

    pthread_mutex_lock(&conns_mtx);
    if (open_conns >= MAX_CONNS) {
        pthread_mutex_unlock(&conns_mtx);
        log_line(LOG_WARN, "TCP", c->peer, "--", "CONN_REJECT",
                 "reason=\"limite de conexiones\" max=%d", MAX_CONNS);
        send_error(c, 0, 0, ERR_INTERNAL_ERROR, "servidor lleno", NULL);
        close(fd);
        free(c);
        return -1;
    }
    open_conns++;
    pthread_mutex_unlock(&conns_mtx);

    /* Keepalive agresivo: un nodo que desaparece sin FIN (cable cortado,
     * máquina apagada) se detecta en ~1 min y pasa a DISCONNECTED. */
    int one = 1, idle = 30, intvl = 10, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    set_recv_timeout(fd, FIRST_MSG_TIMEOUT);
    log_line(LOG_INFO, "TCP", c->peer, "--", "CONN_OPEN", NULL);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* Los hilos de conexión nacen con SIGINT/SIGTERM bloqueadas para que
     * la señal la reciba siempre el hilo principal, que está en accept(). */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    int rc = pthread_create(&th, &attr, conn_thread, c);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        log_line(LOG_ERROR, "TCP", c->peer, "--", "CONN_REJECT",
                 "reason=\"pthread_create: %s\"", strerror(rc));
        close(fd);
        free(c);
        pthread_mutex_lock(&conns_mtx);
        open_conns--;
        pthread_mutex_unlock(&conns_mtx);
        return -1;
    }
    return 0;
}
