#ifndef UDS_IPC_H
#define UDS_IPC_H

#include <pthread.h>
#include "frame_ring.h"

#define MAX_IPC_CLIENTS 8

typedef struct {
    char socket_path[108];
    int server_fd;
    frame_ring_t *ring;
    pthread_t thread;
    volatile int running;

    // Client tracking for broadcast
    int client_fds[MAX_IPC_CLIENTS];
    pthread_mutex_t client_lock;

    // Timelapse session tracking
    char current_lapse_dir[256];
    char current_lapse_filename[128];
    char current_lapse_videoname[128];
    int current_frame_index;
} uds_ipc_t;

int uds_ipc_init(uds_ipc_t *ipc, const char *path, frame_ring_t *ring);
int uds_ipc_start(uds_ipc_t *ipc);
void uds_ipc_stop(uds_ipc_t *ipc);
void uds_ipc_broadcast(uds_ipc_t *ipc, const char *msg);

#endif // UDS_IPC_H
