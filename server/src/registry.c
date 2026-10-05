/*
 * registry.c — tabla de nodos y máquina de estados del nodo vista por el
 * servidor (docs/maquinas-de-estado.md § 2).
 */
#include "registry.h"
#include "protocol.h"

#include <pthread.h>
#include <string.h>

typedef struct {
    int      in_use;
    uint32_t id;
    char     name[MAX_NAME_LEN + 1];
    uint8_t  name_len;
    uint8_t  node_type;
    uint16_t interval;
    uint8_t  state;
    time_t   last_telemetry_at;    /* 0 = nunca */
    float    last_value;
    time_t   disconnected_at;

    hist_point_t hist[HISTORY_CAP];
    size_t   hist_head;            /* próxima posición a escribir */
    size_t   hist_count;

    event_rec_t events[EVENT_CAP];
    size_t   ev_head;
    size_t   ev_count;
} node_t;

static node_t nodes[MAX_NODES];
static uint32_t next_node_id = 1;
static pthread_mutex_t reg_mtx = PTHREAD_MUTEX_INITIALIZER;

void reg_init(void)
{
    pthread_mutex_lock(&reg_mtx);
    memset(nodes, 0, sizeof nodes);
    next_node_id = 1;
    pthread_mutex_unlock(&reg_mtx);
}

/* Debe llamarse con reg_mtx tomado. */
static node_t *find_by_id(uint32_t id)
{
    for (size_t i = 0; i < MAX_NODES; i++)
        if (nodes[i].in_use && nodes[i].id == id)
            return &nodes[i];
    return NULL;
}

static void summarize(const node_t *n, node_summary_t *out)
{
    out->id = n->id;
    out->state = n->state;
    out->node_type = n->node_type;
    out->name_len = n->name_len;
    memcpy(out->name, n->name, n->name_len);
    out->name[n->name_len] = '\0';
    out->last_timestamp = (uint64_t)n->last_telemetry_at;
    out->last_value = n->last_value;
}

int reg_register(const char *name, size_t name_len, uint8_t node_type,
                 uint16_t interval_hint, uint32_t *out_id)
{
    node_t *slot = NULL;

    if (name_len > MAX_NAME_LEN) name_len = MAX_NAME_LEN;

    pthread_mutex_lock(&reg_mtx);
    for (size_t i = 0; i < MAX_NODES; i++) {
        node_t *n = &nodes[i];
        if (!n->in_use || n->name_len != name_len ||
            memcmp(n->name, name, name_len) != 0)
            continue;
        if (n->state != NODE_DISCONNECTED) {
            pthread_mutex_unlock(&reg_mtx);
            return REG_NAME_IN_USE;
        }
        /* DISCONNECTED con el mismo nombre: se reinicia la entrada con un
         * node_id nuevo (invariante 2). */
        slot = n;
        break;
    }
    if (!slot) {
        for (size_t i = 0; i < MAX_NODES; i++)
            if (!nodes[i].in_use) { slot = &nodes[i]; break; }
    }
    if (!slot) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_FULL;
    }

    memset(slot, 0, sizeof *slot);
    slot->in_use = 1;
    slot->id = next_node_id++;
    memcpy(slot->name, name, name_len);
    slot->name_len = (uint8_t)name_len;
    slot->node_type = node_type;
    slot->interval = interval_hint ? interval_hint : DEFAULT_INTERVAL;
    slot->state = NODE_REGISTERED;
    *out_id = slot->id;
    pthread_mutex_unlock(&reg_mtx);
    return REG_OK;
}

int reg_disconnect(uint32_t id)
{
    int old;
    pthread_mutex_lock(&reg_mtx);
    node_t *n = find_by_id(id);
    if (!n) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_UNKNOWN;
    }
    old = n->state;
    n->state = NODE_DISCONNECTED;
    n->disconnected_at = time(NULL);
    pthread_mutex_unlock(&reg_mtx);
    return old;
}

int reg_telemetry(uint32_t id, float value, time_t now, uint8_t *old_state)
{
    pthread_mutex_lock(&reg_mtx);
    node_t *n = find_by_id(id);
    if (!n) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_UNKNOWN;
    }
    *old_state = n->state;
    if (n->state == NODE_DISCONNECTED) {      /* invariante 1: absorbente */
        pthread_mutex_unlock(&reg_mtx);
        return REG_DISCONNECTED;
    }
    n->last_telemetry_at = now;
    n->last_value = value;
    n->hist[n->hist_head].timestamp = (uint64_t)now;
    n->hist[n->hist_head].value = value;
    n->hist_head = (n->hist_head + 1) % HISTORY_CAP;
    if (n->hist_count < HISTORY_CAP) n->hist_count++;
    n->state = NODE_ACTIVE;
    pthread_mutex_unlock(&reg_mtx);
    return REG_OK;
}

int reg_event(uint32_t id, uint8_t code, uint8_t severity, uint64_t node_ts,
              const uint8_t *msg, size_t msg_len, time_t now)
{
    pthread_mutex_lock(&reg_mtx);
    node_t *n = find_by_id(id);
    if (!n || n->state == NODE_DISCONNECTED) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_UNKNOWN;
    }
    event_rec_t *e = &n->events[n->ev_head];
    if (msg_len > EVENT_MSG_KEEP) msg_len = EVENT_MSG_KEEP;
    e->server_ts = (uint64_t)now;
    e->node_ts = node_ts;
    e->code = code;
    e->severity = severity;
    e->msg_len = (uint8_t)msg_len;
    memcpy(e->msg, msg, msg_len);
    n->ev_head = (n->ev_head + 1) % EVENT_CAP;
    if (n->ev_count < EVENT_CAP) n->ev_count++;
    pthread_mutex_unlock(&reg_mtx);
    return REG_OK;
}

int reg_status(uint32_t id, node_summary_t *out)
{
    pthread_mutex_lock(&reg_mtx);
    node_t *n = find_by_id(id);
    if (!n) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_UNKNOWN;
    }
    summarize(n, out);
    pthread_mutex_unlock(&reg_mtx);
    return REG_OK;
}

int reg_history(uint32_t id, size_t max, hist_point_t *out, size_t *count)
{
    pthread_mutex_lock(&reg_mtx);
    node_t *n = find_by_id(id);
    if (!n) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_UNKNOWN;
    }
    size_t k = n->hist_count < max ? n->hist_count : max;
    for (size_t i = 0; i < k; i++) {
        size_t idx = (n->hist_head + HISTORY_CAP - 1 - i) % HISTORY_CAP;
        out[i] = n->hist[idx];
    }
    *count = k;
    pthread_mutex_unlock(&reg_mtx);
    return REG_OK;
}

int reg_events(uint32_t id, size_t max, event_rec_t *out, size_t *count)
{
    pthread_mutex_lock(&reg_mtx);
    node_t *n = find_by_id(id);
    if (!n) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_UNKNOWN;
    }
    size_t k = n->ev_count < max ? n->ev_count : max;
    for (size_t i = 0; i < k; i++) {
        size_t idx = (n->ev_head + EVENT_CAP - 1 - i) % EVENT_CAP;
        out[i] = n->events[idx];
    }
    *count = k;
    pthread_mutex_unlock(&reg_mtx);
    return REG_OK;
}

size_t reg_list(node_summary_t *out, size_t max)
{
    size_t k = 0;
    pthread_mutex_lock(&reg_mtx);
    for (size_t i = 0; i < MAX_NODES && k < max; i++)
        if (nodes[i].in_use)
            summarize(&nodes[i], &out[k++]);
    pthread_mutex_unlock(&reg_mtx);
    return k;
}

int reg_purge(uint32_t id)
{
    pthread_mutex_lock(&reg_mtx);
    node_t *n = find_by_id(id);
    if (!n) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_UNKNOWN;
    }
    if (n->state != NODE_DISCONNECTED) {
        pthread_mutex_unlock(&reg_mtx);
        return REG_NOT_PURGEABLE;
    }
    n->in_use = 0;
    pthread_mutex_unlock(&reg_mtx);
    return REG_OK;
}

size_t reg_sweep(time_t now, transition_t *out, size_t max)
{
    size_t k = 0;
    pthread_mutex_lock(&reg_mtx);
    for (size_t i = 0; i < MAX_NODES && k < max; i++) {
        node_t *n = &nodes[i];
        if (!n->in_use) continue;

        if (n->state == NODE_ACTIVE &&
            now - n->last_telemetry_at > (time_t)STALE_MULTIPLIER * n->interval) {
            out[k].id = n->id;
            out[k].from = NODE_ACTIVE;
            out[k].to = NODE_STALE;
            out[k].age = (long)(now - n->last_telemetry_at);
            out[k].purged = 0;
            k++;
            n->state = NODE_STALE;
        } else if (n->state == NODE_DISCONNECTED &&
                   now - n->disconnected_at > PURGE_AFTER_SECS) {
            out[k].id = n->id;
            out[k].from = NODE_DISCONNECTED;
            out[k].to = NODE_DISCONNECTED;
            out[k].age = (long)(now - n->disconnected_at);
            out[k].purged = 1;
            k++;
            n->in_use = 0;
        }
    }
    pthread_mutex_unlock(&reg_mtx);
    return k;
}
