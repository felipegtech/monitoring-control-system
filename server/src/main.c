/*
 * main.c — servidor central del sistema de monitoreo y control.
 *
 *   ./server <puerto> <archivoDeLogs>
 *
 * Un número de puerto, dos sockets: SOCK_STREAM (REGISTER, EVENT, AUTH,
 * QUERY) y SOCK_DGRAM (TELEMETRY). Concurrencia: un hilo por conexión TCP,
 * un hilo para el socket UDP y un hilo de barrido que detecta STALE
 * (docs/service-spec.md § Modelo de concurrencia).
 */
#include "server.h"
#include "auth.h"
#include "logger.h"
#include "protocol.h"
#include "registry.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_signal = 0;

static void on_signal(int sig)
{
    g_shutdown = 1;
    g_signal = sig;
}

/*
 * Abre un socket pasivo en <puerto> con getaddrinfo(AI_PASSIVE). Se prefiere
 * IPv6 con IPV6_V6ONLY=0 (dual-stack: acepta también IPv4 como ::ffff:x) y,
 * si la máquina no tiene IPv6, se usa IPv4. Equivale a escuchar en
 * INADDR_ANY: no se fija ninguna dirección (despliegue-y-nombres.md).
 */
static int open_passive(const char *port, int socktype)
{
    static const int families[] = { AF_INET6, AF_INET };

    for (size_t f = 0; f < 2; f++) {
        struct addrinfo hints = {0}, *res = NULL, *ai;
        hints.ai_family = families[f];
        hints.ai_socktype = socktype;
        hints.ai_flags = AI_PASSIVE;

        int rc = getaddrinfo(NULL, port, &hints, &res);
        if (rc != 0) continue;

        for (ai = res; ai; ai = ai->ai_next) {
            int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;

            int one = 1, zero = 0;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (ai->ai_family == AF_INET6)
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);

            if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
                (socktype != SOCK_STREAM || listen(fd, 64) == 0)) {
                freeaddrinfo(res);
                return fd;
            }
            close(fd);
        }
        freeaddrinfo(res);
    }
    return -1;
}

static void *sweeper_thread(void *arg)
{
    (void)arg;
    transition_t tr[64];

    for (;;) {
        sleep(SWEEP_INTERVAL);
        size_t n = reg_sweep(time(NULL), tr, 64);
        /* El log se escribe fuera del candado del registro (regla 6). */
        for (size_t i = 0; i < n; i++) {
            if (tr[i].purged)
                log_line(LOG_INFO, "SYS", "-", "--", "NODE_PURGED",
                         "node_id=%u reason=\"DISCONNECTED por %lds\"",
                         tr[i].id, tr[i].age);
            else
                log_line(LOG_WARN, "SYS", "-", "--", "STATE_CHANGE",
                         "node_id=%u from=%s to=%s last_telemetry_age=%lds",
                         tr[i].id, node_state_name(tr[i].from),
                         node_state_name(tr[i].to), tr[i].age);
        }
    }
    return NULL;
}

static int parse_port(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 1 || v > 65535)
        return -1;
    return (int)v;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "uso: %s <puerto> <archivoDeLogs>\n", argv[0]);
        return 2;
    }
    if (parse_port(argv[1]) < 0) {
        fprintf(stderr, "puerto invalido: %s (1..65535)\n", argv[1]);
        return 2;
    }
    /* Sin log no se arranca (logging.md § Invocación). */
    if (log_init(argv[2]) < 0) {
        fprintf(stderr, "no se pudo abrir el archivo de log '%s': %s\n",
                argv[2], strerror(errno));
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;           /* sin SA_RESTART: accept() retorna EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    reg_init();

    int tcp_fd = open_passive(argv[1], SOCK_STREAM);
    if (tcp_fd < 0) {
        log_line(LOG_ERROR, "SYS", "-", "--", "STARTUP_FAIL",
                 "reason=\"no se pudo abrir el socket TCP en el puerto %s: %s\"",
                 argv[1], strerror(errno));
        return 1;
    }
    static int udp_fd;
    udp_fd = open_passive(argv[1], SOCK_DGRAM);
    if (udp_fd < 0) {
        log_line(LOG_ERROR, "SYS", "-", "--", "STARTUP_FAIL",
                 "reason=\"no se pudo abrir el socket UDP en el puerto %s: %s\"",
                 argv[1], strerror(errno));
        return 1;
    }

    log_line(LOG_INFO, "SYS", "-", "--", "STARTUP",
             "tcp_port=%s udp_port=%s log=\"%s\" pid=%d auth=%s "
             "pam_service=%s admin_group=%s", argv[1], argv[1], argv[2],
             (int)getpid(), auth_backend(), auth_pam_service(),
             auth_admin_group());
    if (strcmp(auth_backend(), "stub") == 0)
        log_line(LOG_WARN, "SYS", "-", "--", "AUTH_STUB",
                 "reason=\"binario de pruebas: no usa PAM\"");

    /* Los hilos auxiliares nacen con las señales bloqueadas. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    pthread_t th_udp, th_sweep;
    if (pthread_create(&th_udp, NULL, udp_thread, &udp_fd) != 0 ||
        pthread_create(&th_sweep, NULL, sweeper_thread, NULL) != 0) {
        log_line(LOG_ERROR, "SYS", "-", "--", "STARTUP_FAIL",
                 "reason=\"pthread_create\"");
        return 1;
    }
    pthread_detach(th_udp);
    pthread_detach(th_sweep);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    while (!g_shutdown) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        int fd = accept(tcp_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            log_line(LOG_ERROR, "SYS", "-", "--", "ACCEPT_FAIL",
                     "reason=\"%s\"", strerror(errno));
            /* EMFILE/ENFILE: esperar a que se liberen descriptores */
            usleep(100 * 1000);
            continue;
        }
        conn_spawn(fd, (struct sockaddr *)&peer);
    }

    log_line(LOG_INFO, "SYS", "-", "--", "SHUTDOWN",
             "reason=\"%s\" open_conns=%d",
             g_signal == SIGTERM ? "SIGTERM" : "SIGINT", conn_open_count());
    close(tcp_fd);
    close(udp_fd);
    log_close();
    return 0;
}
