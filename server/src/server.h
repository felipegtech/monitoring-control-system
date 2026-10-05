/*
 * server.h — piezas compartidas entre main.c, conn.c y udp.c.
 */
#ifndef MCS_SERVER_H
#define MCS_SERVER_H

#include <signal.h>
#include <sys/socket.h>

#define MAX_CONNS             128  /* conexiones TCP simultáneas */
#define FIRST_MSG_TIMEOUT     30   /* s para enviar REGISTER o AUTH */
#define CLIENT_IDLE_TIMEOUT   600  /* s sin QUERY antes de cerrar */
#define MAX_AUTH_ATTEMPTS     3
#define SWEEP_INTERVAL        1    /* s entre barridos de STALE */

extern volatile sig_atomic_t g_shutdown;

/* Lanza un hilo desacoplado que atiende la conexión aceptada. */
int  conn_spawn(int fd, const struct sockaddr *peer);
int  conn_open_count(void);

/* Hilo dedicado al socket UDP de telemetría. */
void *udp_thread(void *arg);   /* arg: puntero a int con el descriptor */

#endif
