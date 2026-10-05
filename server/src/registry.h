/*
 * registry.h — registro compartido de nodos (docs/service-spec.md § Modelo
 * de concurrencia). Un único mutex global protege toda la tabla. Ninguna
 * función devuelve punteros al interior del registro: todo se copia hacia
 * afuera para que la sección crítica sea corta y el trabajo lento (armar la
 * RESP, escribir el log) ocurra fuera del candado (logging.md, regla 6).
 */
#ifndef MCS_REGISTRY_H
#define MCS_REGISTRY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MAX_NODES          256
#define MAX_NAME_LEN       255
#define HISTORY_CAP        20    /* ≥5 exigido por el requerimiento 5 */
#define EVENT_CAP          10
#define EVENT_MSG_KEEP     200   /* bytes de `message` que se conservan */
#define STALE_MULTIPLIER   3
#define DEFAULT_INTERVAL   5     /* si interval_hint llega en 0 */
#define PURGE_AFTER_SECS   600   /* DISCONNECTED se purga a los 10 min */

typedef struct {
    uint64_t timestamp;   /* hora de recepción en el servidor */
    float    value;
} hist_point_t;

typedef struct {
    uint64_t server_ts;
    uint64_t node_ts;
    uint8_t  code;
    uint8_t  severity;
    uint8_t  msg_len;
    char     msg[EVENT_MSG_KEEP];
} event_rec_t;

typedef struct {
    uint32_t id;
    uint8_t  state;
    uint8_t  node_type;
    uint8_t  name_len;
    char     name[MAX_NAME_LEN + 1];
    uint64_t last_timestamp;  /* 0 si aún no hay telemetría */
    float    last_value;
} node_summary_t;

/* Transición producida por el barrido, para registrarla fuera del candado. */
typedef struct {
    uint32_t id;
    uint8_t  from, to;
    long     age;         /* segundos desde la última telemetría */
    int      purged;      /* 1 si la entrada se eliminó del registro */
} transition_t;

enum {
    REG_OK            = 0,
    REG_UNKNOWN       = -1,
    REG_NAME_IN_USE   = -2,
    REG_FULL          = -3,
    REG_DISCONNECTED  = -4,
    REG_NOT_PURGEABLE = -5
};

void reg_init(void);

int  reg_register(const char *name, size_t name_len, uint8_t node_type,
                  uint16_t interval_hint, uint32_t *out_id);
/* Marca DISCONNECTED; devuelve el estado anterior o REG_UNKNOWN. */
int  reg_disconnect(uint32_t id);
/* Inserta la lectura; en *old_state queda el estado previo. */
int  reg_telemetry(uint32_t id, float value, time_t now, uint8_t *old_state);
int  reg_event(uint32_t id, uint8_t code, uint8_t severity, uint64_t node_ts,
               const uint8_t *msg, size_t msg_len, time_t now);

int  reg_status(uint32_t id, node_summary_t *out);
/* Copia hasta `max` puntos, del más reciente al más antiguo. */
int  reg_history(uint32_t id, size_t max, hist_point_t *out, size_t *n);
int  reg_events(uint32_t id, size_t max, event_rec_t *out, size_t *n);
size_t reg_list(node_summary_t *out, size_t max);
int  reg_purge(uint32_t id);

/* Barrido periódico: ACTIVE→STALE y purga de DISCONNECTED vencidos. */
size_t reg_sweep(time_t now, transition_t *out, size_t max);

#endif
