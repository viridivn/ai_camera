#include "http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>

#define HTTP_HEADER \
    "HTTP/1.1 200 OK\r\n" \
    "Cache-Control: no-cache\r\n" \
    "Pragma: no-cache\r\n" \
    "Connection: close\r\n" \
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n"

#define CORS_RESPONSE \
    "HTTP/1.1 200 OK\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Content-Type\r\n" \
    "Content-Length: 0\r\n\r\n"

typedef struct {
    int client_fd;
    http_server_t *server;
} client_ctx_t;

static void *client_thread_proc(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int fd = ctx->client_fd;
    http_server_t *server = ctx->server;
    free(ctx);

    // Set TCP_NODELAY
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char req_buf[1024];
    ssize_t bytes_read = recv(fd, req_buf, sizeof(req_buf) - 1, 0);
    if (bytes_read <= 0) {
        close(fd);
        return NULL;
    }
    req_buf[bytes_read] = '\0';

    if (strstr(req_buf, "OPTIONS")) {
        send(fd, CORS_RESPONSE, strlen(CORS_RESPONSE), MSG_NOSIGNAL);
        close(fd);
        return NULL;
    }

    // Send HTTP header
    if (send(fd, HTTP_HEADER, strlen(HTTP_HEADER), MSG_NOSIGNAL) <= 0) {
        close(fd);
        return NULL;
    }

    uint8_t *frame_buf = malloc(MAX_FRAME_SIZE);
    if (!frame_buf) {
        close(fd);
        return NULL;
    }

    uint64_t last_seq = 0;
    char boundary_hdr[128];

    while (server->running) {
        size_t frame_size = 0;
        uint64_t seq = 0;

        int r = frame_ring_get_latest(server->ring, last_seq, frame_buf, MAX_FRAME_SIZE, &frame_size, &seq, 500);
        if (r == 0) {
            last_seq = seq;
            int hdr_len = snprintf(boundary_hdr, sizeof(boundary_hdr),
                                  "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", frame_size);

            if (send(fd, boundary_hdr, hdr_len, MSG_NOSIGNAL) <= 0) break;
            if (send(fd, frame_buf, frame_size, MSG_NOSIGNAL) <= 0) break;
            if (send(fd, "\r\n", 2, MSG_NOSIGNAL) <= 0) break;
        } else if (r == -2) {
            fprintf(stderr, "[http_server] Frame exceeded buffer limit\n");
            break;
        }
        // r == -1 is timeout, loop again if server->running
    }

    free(frame_buf);
    close(fd);
    return NULL;
}

static void *accept_thread_proc(void *arg) {
    http_server_t *server = (http_server_t *)arg;

    struct pollfd fds[1];
    fds[0].fd = server->server_fd;
    fds[0].events = POLLIN;

    while (server->running) {
        int r = poll(fds, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server->server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_fd >= 0) {
                client_ctx_t *ctx = malloc(sizeof(*ctx));
                if (ctx) {
                    ctx->client_fd = client_fd;
                    ctx->server = server;
                    pthread_t thread;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    if (pthread_create(&thread, &attr, client_thread_proc, ctx) != 0) {
                        free(ctx);
                        close(client_fd);
                    }
                    pthread_attr_destroy(&attr);
                } else {
                    close(client_fd);
                }
            }
        }
    }

    return NULL;
}

int http_server_init(http_server_t *server, int port, frame_ring_t *ring) {
    memset(server, 0, sizeof(*server));
    server->port = port > 0 ? port : 8080;
    server->server_fd = -1;
    server->ring = ring;

    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) {
        perror("socket creation failed");
        return -1;
    }

    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(server->port);

    if (bind(server->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[http_server] Failed to bind HTTP server to port %d: %s\n", server->port, strerror(errno));
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }

    if (listen(server->server_fd, 10) < 0) {
        perror("listen failed");
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }

    printf("[http_server] HTTP MJPEG stream server initialized on port %d\n", server->port);
    return 0;
}

int http_server_start(http_server_t *server) {
    if (server->server_fd < 0 || server->running) return -1;

    server->running = 1;
    if (pthread_create(&server->accept_thread, NULL, accept_thread_proc, server) != 0) {
        server->running = 0;
        return -1;
    }

    printf("[http_server] HTTP stream server listening on port %d\n", server->port);
    return 0;
}

void http_server_stop(http_server_t *server) {
    if (!server->running) return;

    server->running = 0;
    pthread_join(server->accept_thread, NULL);

    if (server->server_fd >= 0) {
        close(server->server_fd);
        server->server_fd = -1;
    }
    printf("[http_server] HTTP stream server stopped\n");
}
