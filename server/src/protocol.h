/*
 * protocol.h — constantes, enumeraciones y serialización del protocolo.
 *
 * Fuente normativa: docs/protocol.md. Todo campo entero de más de un byte
 * viaja en orden de red (big-endian). La serialización se hace campo por
 * campo, nunca con memcpy de structs, para que el relleno que inserta el
 * compilador no altere el formato en el cable.
 */
#ifndef MCS_PROTOCOL_H
#define MCS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_VERSION 1
#define HEADER_SIZE   10   /* 1 + 1 + 4 + 4: son 10 bytes, no 8 */
#define MAX_PAYLOAD   4096
#define TELEMETRY_PAYLOAD_SIZE 13
#define QUERY_PAYLOAD_SIZE     7

/* msg_type */
enum {
    MSG_REGISTER  = 1,
    MSG_EVENT     = 2,
    MSG_TELEMETRY = 3,
    MSG_QUERY     = 4,
    MSG_RESP      = 5,
    MSG_ERROR     = 6,
    MSG_AUTH      = 7
};

/* query_type */
enum {
    Q_STATUS     = 1,
    Q_HISTORY    = 2,
    Q_LIST_NODES = 3,
    Q_EVENTS     = 4,
    Q_PURGE_NODE = 5
};

/* error_code */
enum {
    ERR_AUTH_FAILED       = 1,
    ERR_UNKNOWN_NODE      = 2,
    ERR_MALFORMED_MSG     = 3,
    ERR_UNSUPPORTED_QUERY = 4,
    ERR_SESSION_EXPIRED   = 5,
    ERR_INTERNAL_ERROR    = 6,
    ERR_VERSION_MISMATCH  = 7,
    ERR_NAME_IN_USE       = 8,
    ERR_PERMISSION_DENIED = 9,
    ERR_INVALID_PARAM     = 10
};

/* Estado del nodo según el servidor (maquinas-de-estado.md § 2) */
enum {
    NODE_REGISTERED   = 0,
    NODE_ACTIVE       = 1,
    NODE_STALE        = 2,
    NODE_DISCONNECTED = 3
};

/* Perfil de usuario devuelto en RESP(AUTH) */
enum {
    PROFILE_VIEWER = 0,
    PROFILE_ADMIN  = 1
};

typedef struct {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t entity_id;
    uint32_t payload_length;
} msg_header_t;

/* Codifica / decodifica el encabezado de 10 bytes. */
void header_encode(uint8_t out[HEADER_SIZE], const msg_header_t *h);
void header_decode(const uint8_t in[HEADER_SIZE], msg_header_t *h);

/*
 * Lector acotado sobre un búfer recibido. Toda lectura comprueba que quedan
 * bytes suficientes; si no, marca `err` y devuelve 0, de modo que un payload
 * truncado se detecta como MALFORMED_MSG en vez de leer fuera del búfer.
 */
typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    int err;
} reader_t;

void     rd_init(reader_t *r, const uint8_t *buf, size_t len);
uint8_t  rd_u8(reader_t *r);
uint16_t rd_u16(reader_t *r);
uint32_t rd_u32(reader_t *r);
uint64_t rd_u64(reader_t *r);
float    rd_f32(reader_t *r);
/* Devuelve un puntero a `n` bytes dentro del búfer (sin copiar). */
const uint8_t *rd_bytes(reader_t *r, size_t n);
/* 1 si se consumió exactamente todo el búfer sin errores. */
int      rd_done(const reader_t *r);

/* Escritor acotado sobre un búfer de salida de tamaño fijo. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    int err;
} writer_t;

void wr_init(writer_t *w, uint8_t *buf, size_t cap);
void wr_u8(writer_t *w, uint8_t v);
void wr_u16(writer_t *w, uint16_t v);
void wr_u32(writer_t *w, uint32_t v);
void wr_u64(writer_t *w, uint64_t v);
void wr_f32(writer_t *w, float v);
void wr_bytes(writer_t *w, const void *p, size_t n);

/* Nombres legibles para el log. */
const char *msg_type_name(uint8_t t);
const char *query_type_name(uint8_t t);
const char *error_code_name(uint8_t c);
const char *node_state_name(uint8_t s);
const char *profile_name(uint8_t p);

#endif
