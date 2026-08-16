#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <pthread.h>
#include "frame_ring.h"

typedef struct {
    int port;
    int server_fd;
    frame_ring_t *ring;
    pthread_t accept_thread;
    volatile int running;
} http_server_t;

int http_server_init(http_server_t *server, int port, frame_ring_t *ring);
int http_server_start(http_server_t *server);
void http_server_stop(http_server_t *server);

#endif // HTTP_SERVER_H
