#include "uds_ipc.h"
#include "timelapse_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <dirent.h>
#include <poll.h>

typedef struct {
    int client_fd;
    uds_ipc_t *ipc;
} ipc_client_ctx_t;

typedef struct {
    uds_ipc_t *ipc;
    char target_dir[256];
    char out_video[256];
} composite_callback_ctx_t;

static void extract_json_field(const char *json, const char *key, char *out_val, size_t out_max) {
    out_val[0] = '\0';
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char *pos = strstr(json, search_key);
    if (!pos) return;

    pos += strlen(search_key);
    while (*pos == ' ' || *pos == '\t' || *pos == ':') pos++;

    if (*pos == '"') {
        pos++;
        const char *end = strchr(pos, '"');
        if (end) {
            size_t len = end - pos;
            if (len >= out_max) len = out_max - 1;
            memcpy(out_val, pos, len);
            out_val[len] = '\0';
        }
    } else {
        size_t len = 0;
        while (pos[len] && pos[len] != ',' && pos[len] != '}' && pos[len] != ']' && pos[len] != ' ' && pos[len] != '\n' && pos[len] != '\r') {
            len++;
        }
        if (len >= out_max) len = out_max - 1;
        memcpy(out_val, pos, len);
        out_val[len] = '\0';
    }
}

void uds_ipc_broadcast(uds_ipc_t *ipc, const char *msg) {
    if (!ipc || !msg) return;

    size_t len = strlen(msg);
    pthread_mutex_lock(&ipc->client_lock);
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        if (ipc->client_fds[i] >= 0) {
            send(ipc->client_fds[i], msg, len, MSG_NOSIGNAL);
        }
    }
    pthread_mutex_unlock(&ipc->client_lock);
}

static void save_timelapse_snapshot(frame_ring_t *ring, const char *filepath) {
    if (!filepath || !filepath[0]) filepath = "/opt/usr/video/snapshot.jpg";

    uint8_t *frame_buf = malloc(MAX_FRAME_SIZE);
    if (!frame_buf) return;

    size_t frame_size = 0;
    uint64_t seq = 0;
    if (frame_ring_get_latest(ring, 0, frame_buf, MAX_FRAME_SIZE, &frame_size, &seq, 200) == 0) {
        char path_copy[256];
        snprintf(path_copy, sizeof(path_copy), "%s", filepath);
        char *slash = strrchr(path_copy, '/');
        if (slash) {
            *slash = '\0';
            char mkdir_cmd[300];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", path_copy);
            system(mkdir_cmd);
        }

        FILE *f = fopen(filepath, "wb");
        if (f) {
            fwrite(frame_buf, 1, frame_size, f);
            fclose(f);
            printf("[uds_ipc] Saved timelapse snapshot to %s (%zu bytes)\n", filepath, frame_size);
        } else {
            fprintf(stderr, "[uds_ipc] Failed to open %s for snapshot write: %s\n", filepath, strerror(errno));
        }
    }

    free(frame_buf);
}

static void on_composite_progress(int current, int total, void *user_data) {
    composite_callback_ctx_t *ctx = (composite_callback_ctx_t *)user_data;
    if (!ctx || !ctx->ipc || total <= 0) return;

    double pct = ((double)current * 100.0) / (double)total;
    char report[256];
    snprintf(report, sizeof(report),
             "{\"id\":-1,\"method\":\"status_report\",\"result\":{\"level\":\"info\",\"message\":\"Time-Lapse Composite Video Process\",\"process\":%.2f}}\n",
             pct);
    uds_ipc_broadcast(ctx->ipc, report);
}

static void on_composite_finish(const char *output_mp4, long file_size, int success, void *user_data) {
    composite_callback_ctx_t *ctx = (composite_callback_ctx_t *)user_data;
    if (!ctx || !ctx->ipc) return;

    if (success) {
        char path_buf[256];
        snprintf(path_buf, sizeof(path_buf), "%s", output_mp4);
        char *base = basename(path_buf);

        printf("[uds_ipc] Timelapse video ready: '%s' (%ld bytes)\n", output_mp4, file_size);

        // Broadcast finish event so GUI adds video to gallery
        char finish_report[512];
        snprintf(finish_report, sizeof(finish_report),
                 "{\"id\":-1,\"method\":\"status_report\",\"result\":{\"level\":\"info\",\"message\":\"Time-Lapse Composite Video Finish\",\"video_name\":\"%s\",\"video_path\":\"%s\",\"video_size\":%ld}}\n",
                 base, output_mp4, file_size);
        uds_ipc_broadcast(ctx->ipc, finish_report);

        // Clean up temporary files
        unlink("/tmp/pic_link");
        if (ctx->target_dir[0] && strstr(ctx->target_dir, "/opt/usr/picture/")) {
            char rm_cmd[300];
            snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", ctx->target_dir);
            system(rm_cmd);
        }
    } else {
        fprintf(stderr, "[uds_ipc] Timelapse composite failed for '%s'\n", output_mp4);
        char fail_report[256];
        snprintf(fail_report, sizeof(fail_report),
                 "{\"id\":-1,\"method\":\"status_report\",\"result\":{\"level\":\"error\",\"message\":\"Time-Lapse Composite Video Failed\"}}\n");
        uds_ipc_broadcast(ctx->ipc, fail_report);
    }

    free(ctx);
}

static void process_ipc_message(uds_ipc_t *ipc, int client_fd, const char *msg) {
    char id[128] = {0};
    char method[128] = {0};
    char status_param[64] = {0};
    char mode[64] = {0};
    char filename[128] = {0};
    char video_name[128] = {0};
    char index_str[32] = {0};
    char filepath[256] = {0};
    char pic_dir[256] = {0};
    char fps_str[32] = {0};

    extract_json_field(msg, "id", id, sizeof(id));
    extract_json_field(msg, "method", method, sizeof(method));
    extract_json_field(msg, "status", status_param, sizeof(status_param));
    extract_json_field(msg, "mode", mode, sizeof(mode));
    extract_json_field(msg, "filename", filename, sizeof(filename));
    extract_json_field(msg, "video_name", video_name, sizeof(video_name));
    extract_json_field(msg, "index", index_str, sizeof(index_str));
    extract_json_field(msg, "filepath", filepath, sizeof(filepath));
    if (!filepath[0]) {
        extract_json_field(msg, "video_path", filepath, sizeof(filepath));
    }
    extract_json_field(msg, "pic_dir_path", pic_dir, sizeof(pic_dir));
    if (!pic_dir[0]) {
        extract_json_field(msg, "dir", pic_dir, sizeof(pic_dir));
    }
    extract_json_field(msg, "fps", fps_str, sizeof(fps_str));
    int fps = fps_str[0] ? atoi(fps_str) : 15;

    if (method[0] != '\0') {
        printf("[uds_ipc] Received IPC message: method='%s', status='%s', mode='%s', filename='%s', index='%s', id='%s'\n",
               method, status_param[0] ? status_param : "none", mode[0] ? mode : "none",
               filename[0] ? filename : "none", index_str[0] ? index_str : "none", id[0] ? id : "null");
    }

    char status_str[64] = "ok";

    int is_begin = (strcasecmp(status_param, "start") == 0 || strcasecmp(status_param, "begin") == 0 ||
                    strcasecmp(mode, "start") == 0         || strcasecmp(mode, "begin") == 0);
    int is_capture = (strcasecmp(status_param, "capture") == 0 || strcasecmp(mode, "capture") == 0);
    int is_stop = (strcasecmp(status_param, "stop") == 0 || strcasecmp(mode, "stop") == 0);
    int is_continue = (strcasecmp(status_param, "continue") == 0 || strcasecmp(status_param, "resume") == 0 ||
                       strcasecmp(mode, "continue") == 0         || strcasecmp(mode, "resume") == 0);

    // Handle "time_lapse" / "TimeLapseControl" command
    if (strcasecmp(method, "time_lapse") == 0 || strcasecmp(method, "TimeLapseControl") == 0) {
        if (is_begin) {
            time_t now = time(NULL);
            struct tm *tm_now = localtime(&now);
            char tstamp[32];
            strftime(tstamp, sizeof(tstamp), "%Y%m%d%H%M%S", tm_now);

            if (!filename[0]) {
                snprintf(ipc->current_lapse_filename, sizeof(ipc->current_lapse_filename), "timelapse_%s", tstamp);
            } else {
                snprintf(ipc->current_lapse_filename, sizeof(ipc->current_lapse_filename), "%s%s", filename, tstamp);
            }
            snprintf(ipc->current_lapse_dir, sizeof(ipc->current_lapse_dir), "/opt/usr/picture/%s.temp", ipc->current_lapse_filename);
            ipc->current_frame_index = 0;

            // Ensure directories and symlink exist
            char mkdir_cmd[400];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\" \"/opt/usr/video\"", ipc->current_lapse_dir);
            system(mkdir_cmd);

            unlink("/tmp/pic_link");
            symlink(ipc->current_lapse_dir, "/tmp/pic_link");

            printf("[uds_ipc] Timelapse session started: dir='%s', symlink='/tmp/pic_link'\n", ipc->current_lapse_dir);
            snprintf(status_str, sizeof(status_str), "TimeLapseStart");
        } else if (is_capture) {
            int frame_idx = 0;
            if (index_str[0]) {
                frame_idx = atoi(index_str);
                if (frame_idx >= ipc->current_frame_index) {
                    ipc->current_frame_index = frame_idx + 1;
                }
            } else {
                frame_idx = ipc->current_frame_index++;
            }
            char snap_file[300];
            if (filepath[0]) {
                snprintf(snap_file, sizeof(snap_file), "%s", filepath);
            } else if (ipc->current_lapse_dir[0]) {
                snprintf(snap_file, sizeof(snap_file), "%s/%04d.jpeg", ipc->current_lapse_dir, frame_idx);
            } else {
                snprintf(snap_file, sizeof(snap_file), "/opt/usr/picture/%04d.jpeg", frame_idx);
            }
            save_timelapse_snapshot(ipc->ring, snap_file);
            snprintf(status_str, sizeof(status_str), "Capture");
        } else if (is_continue) {
            printf("[uds_ipc] Timelapse session continued\n");
            snprintf(status_str, sizeof(status_str), "Continue");
        } else if (is_stop) {
            // Strip .temp suffix and rename directory so GUI sees it as unsynthesized video
            char final_dir[256];
            snprintf(final_dir, sizeof(final_dir), "/opt/usr/picture/%s", ipc->current_lapse_filename);
            rename(ipc->current_lapse_dir, final_dir);
            snprintf(ipc->current_lapse_dir, sizeof(ipc->current_lapse_dir), "%s", final_dir);
            unlink("/tmp/pic_link");
            symlink(ipc->current_lapse_dir, "/tmp/pic_link");

            int frame_count = ipc->current_frame_index;
            if (frame_count <= 0) {
                DIR *d = opendir(ipc->current_lapse_dir);
                if (d) {
                    struct dirent *ent;
                    while ((ent = readdir(d)) != NULL) {
                        if (strstr(ent->d_name, ".jpeg") || strstr(ent->d_name, ".jpg")) {
                            frame_count++;
                        }
                    }
                    closedir(d);
                }
            }

            int duration = (frame_count > 0 ? (frame_count / 15) : 0);
            if (duration == 0 && frame_count > 0) duration = 1;

            printf("[uds_ipc] Timelapse session stopped (captured %d frames in '%s')\n",
                   frame_count, ipc->current_lapse_dir);

            char response[512];
            int len = snprintf(response, sizeof(response),
                               "{\"id\":%s,\"method\":\"time_lapse\",\"result\":\"ok\",\"status\":\"TimeLapseStop\",\"video_duration\":%d,\"video_name\":\"%s\",\"video_path\":\"%s\"}\n",
                               id[0] ? id : "null", duration, ipc->current_lapse_filename, ipc->current_lapse_dir);
            send(client_fd, response, len, MSG_NOSIGNAL);

            if (ipc->auto_composite && frame_count > 0 && ipc->current_lapse_filename[0]) {
                composite_callback_ctx_t *ctx = malloc(sizeof(*ctx));
                if (ctx) {
                    ctx->ipc = ipc;
                    snprintf(ctx->target_dir, sizeof(ctx->target_dir), "%s", ipc->current_lapse_dir);
                    snprintf(ctx->out_video, sizeof(ctx->out_video), "/opt/usr/video/%s.mp4", ipc->current_lapse_filename);

                    char out_copy[256];
                    snprintf(out_copy, sizeof(out_copy), "%s", ctx->out_video);
                    char *base_name = basename(out_copy);

                    printf("[uds_ipc] Auto-encoding timelapse on stop: '%s' -> '%s' (nice 19)\n",
                           ctx->target_dir, ctx->out_video);

                    char start_report[512];
                    snprintf(start_report, sizeof(start_report),
                             "{\"id\":-1,\"method\":\"status_report\",\"result\":{\"level\":\"info\",\"message\":\"Time-Lapse Composite Video Start\",\"video_name\":\"%s\"}}\n",
                             base_name);
                    uds_ipc_broadcast(ipc, start_report);

                    timelapse_encode_directory_async(ctx->target_dir, ctx->out_video, 15,
                                                     on_composite_progress, on_composite_finish, ctx);
                }
            }

            return;
        }
    } else if (strcasecmp(method, "composite_video") == 0 || strcasecmp(method, "TimeLapseComposite") == 0) {
        composite_callback_ctx_t *ctx = malloc(sizeof(*ctx));
        if (ctx) {
            ctx->ipc = ipc;

            if (pic_dir[0]) {
                snprintf(ctx->target_dir, sizeof(ctx->target_dir), "%s", pic_dir);
            } else if (video_name[0]) {
                snprintf(ctx->target_dir, sizeof(ctx->target_dir), "/opt/usr/picture/%s", video_name);
                struct stat st;
                if (stat(ctx->target_dir, &st) != 0) {
                    char temp_check[256];
                    snprintf(temp_check, sizeof(temp_check), "/opt/usr/picture/%s.temp", video_name);
                    if (stat(temp_check, &st) == 0) {
                        snprintf(ctx->target_dir, sizeof(ctx->target_dir), "%s", temp_check);
                    }
                }
            } else if (filename[0]) {
                snprintf(ctx->target_dir, sizeof(ctx->target_dir), "/opt/usr/picture/%s", filename);
            } else if (ipc->current_lapse_dir[0]) {
                snprintf(ctx->target_dir, sizeof(ctx->target_dir), "%s", ipc->current_lapse_dir);
            } else {
                snprintf(ctx->target_dir, sizeof(ctx->target_dir), "/tmp/pic_link");
            }

            if (filepath[0]) {
                snprintf(ctx->out_video, sizeof(ctx->out_video), "%s", filepath);
            } else if (video_name[0]) {
                snprintf(ctx->out_video, sizeof(ctx->out_video), "/opt/usr/video/%s", video_name);
            } else if (filename[0]) {
                snprintf(ctx->out_video, sizeof(ctx->out_video), "/opt/usr/video/%s.mp4", filename);
            } else if (ipc->current_lapse_filename[0]) {
                snprintf(ctx->out_video, sizeof(ctx->out_video), "/opt/usr/video/%s.mp4", ipc->current_lapse_filename);
            } else {
                snprintf(ctx->out_video, sizeof(ctx->out_video), "/opt/usr/video/timelapse.mp4");
            }

            if (!strstr(ctx->out_video, ".mp4")) {
                strncat(ctx->out_video, ".mp4", sizeof(ctx->out_video) - strlen(ctx->out_video) - 1);
            }

            char out_copy[256];
            snprintf(out_copy, sizeof(out_copy), "%s", ctx->out_video);
            char *base_name = basename(out_copy);

            struct stat st_video;
            if (stat(ctx->out_video, &st_video) == 0 && st_video.st_size > 0) {
                // Video is already synthesized on disk (e.g. user exporting an existing MP4 to USB/storage)
                printf("[uds_ipc] Video '%s' already exists (%lld bytes), reporting ready for export\n",
                       ctx->out_video, (long long)st_video.st_size);

                char start_report[512];
                snprintf(start_report, sizeof(start_report),
                         "{\"id\":-1,\"method\":\"status_report\",\"result\":{\"level\":\"info\",\"message\":\"Time-Lapse Composite Video Start\",\"video_name\":\"%s\"}}\n",
                         base_name);
                uds_ipc_broadcast(ipc, start_report);

                char finish_report[512];
                snprintf(finish_report, sizeof(finish_report),
                         "{\"id\":-1,\"method\":\"status_report\",\"result\":{\"level\":\"info\",\"message\":\"Time-Lapse Composite Video Finish\",\"video_name\":\"%s\",\"video_path\":\"%s\",\"video_size\":%lld}}\n",
                         base_name, ctx->out_video, (long long)st_video.st_size);
                uds_ipc_broadcast(ipc, finish_report);

                free(ctx);
            } else {
                printf("[uds_ipc] Triggering timelapse video composite: '%s' -> '%s' @ %d fps\n",
                       ctx->target_dir, ctx->out_video, fps);

                char start_report[512];
                snprintf(start_report, sizeof(start_report),
                         "{\"id\":-1,\"method\":\"status_report\",\"result\":{\"level\":\"info\",\"message\":\"Time-Lapse Composite Video Start\",\"video_name\":\"%s\"}}\n",
                         base_name);
                uds_ipc_broadcast(ipc, start_report);

                timelapse_encode_directory_async(ctx->target_dir, ctx->out_video, fps,
                                                 on_composite_progress, on_composite_finish, ctx);
            }
        }
        snprintf(status_str, sizeof(status_str), "Composite_video");
    } else if (strcasecmp(method, "TimeLapseSavePicture") == 0) {
        if (!filepath[0]) snprintf(filepath, sizeof(filepath), "/opt/usr/picture/snapshot.jpg");
        save_timelapse_snapshot(ipc->ring, filepath);
        snprintf(status_str, sizeof(status_str), "Capture");
    } else if (strcasecmp(method, "TimeLapseStart") == 0) {
        if (filename[0]) {
            snprintf(ipc->current_lapse_dir, sizeof(ipc->current_lapse_dir), "/opt/usr/picture/%s", filename);
        } else {
            snprintf(ipc->current_lapse_dir, sizeof(ipc->current_lapse_dir), "/opt/usr/picture/timelapse");
        }
        char mkdir_cmd[300];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\" \"/opt/usr/video\"", ipc->current_lapse_dir);
        system(mkdir_cmd);
        unlink("/tmp/pic_link");
        symlink(ipc->current_lapse_dir, "/tmp/pic_link");
        ipc->current_frame_index = 0;
        printf("[uds_ipc] TimeLapseStart: dir='%s'\n", ipc->current_lapse_dir);
        snprintf(status_str, sizeof(status_str), "TimeLapseStart");
    } else if (strcasecmp(method, "TimeLapseStop") == 0) {
        printf("[uds_ipc] TimeLapseStop received\n");
        snprintf(status_str, sizeof(status_str), "Stop");
    }

    char response[512];
    int len;
    if (id[0] != '\0' && id[0] != 'n') {
        len = snprintf(response, sizeof(response),
                       "{\"id\":%s,\"method\":\"%s\",\"result\":\"ok\",\"status\":\"%s\"}\n",
                       id, method[0] ? method : "start", status_str);
    } else {
        len = snprintf(response, sizeof(response),
                       "{\"id\":null,\"method\":\"%s\",\"result\":\"ok\",\"status\":\"%s\"}\n",
                       method[0] ? method : "start", status_str);
    }

    send(client_fd, response, len, MSG_NOSIGNAL);
}

static void *client_ipc_thread_proc(void *arg) {
    ipc_client_ctx_t *ctx = (ipc_client_ctx_t *)arg;
    int client_fd = ctx->client_fd;
    uds_ipc_t *ipc = ctx->ipc;
    free(ctx);

    // Register client fd for broadcasts
    pthread_mutex_lock(&ipc->client_lock);
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        if (ipc->client_fds[i] < 0) {
            ipc->client_fds[i] = client_fd;
            break;
        }
    }
    pthread_mutex_unlock(&ipc->client_lock);

    printf("[uds_ipc] Client connected on UDS (fd=%d)\n", client_fd);

    char buf[4096];
    struct pollfd fds[1];
    fds[0].fd = client_fd;
    fds[0].events = POLLIN;

    while (ipc->running) {
        int r = poll(fds, 1, 1000); // 1s timeout to check ipc->running
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                break;
            }

            buf[n] = '\0';
            char *line = strtok(buf, "\n\r");
            while (line) {
                if (strlen(line) > 0) {
                    process_ipc_message(ipc, client_fd, line);
                }
                line = strtok(NULL, "\n\r");
            }
        }
    }

    printf("[uds_ipc] Client disconnected (fd=%d)\n", client_fd);

    // Unregister client fd
    pthread_mutex_lock(&ipc->client_lock);
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        if (ipc->client_fds[i] == client_fd) {
            ipc->client_fds[i] = -1;
            break;
        }
    }
    pthread_mutex_unlock(&ipc->client_lock);

    close(client_fd);
    return NULL;
}

static void *uds_accept_thread_proc(void *arg) {
    uds_ipc_t *ipc = (uds_ipc_t *)arg;

    struct pollfd fds[1];
    fds[0].fd = ipc->server_fd;
    fds[0].events = POLLIN;

    while (ipc->running) {
        int r = poll(fds, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        if (fds[0].revents & POLLIN) {
            int client_fd = accept(ipc->server_fd, NULL, NULL);
            if (client_fd >= 0) {
                ipc_client_ctx_t *ctx = malloc(sizeof(*ctx));
                if (ctx) {
                    ctx->client_fd = client_fd;
                    ctx->ipc = ipc;
                    pthread_t thread;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    if (pthread_create(&thread, &attr, client_ipc_thread_proc, ctx) != 0) {
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

int uds_ipc_init(uds_ipc_t *ipc, const char *path, frame_ring_t *ring, int auto_composite) {
    memset(ipc, 0, sizeof(*ipc));
    snprintf(ipc->socket_path, sizeof(ipc->socket_path), "%s", path ? path : "/tmp/aicamera_uds");
    ipc->server_fd = -1;
    ipc->ring = ring;
    ipc->auto_composite = auto_composite;

    pthread_mutex_init(&ipc->client_lock, NULL);
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        ipc->client_fds[i] = -1;
    }

    unlink(ipc->socket_path);

    ipc->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc->server_fd < 0) {
        perror("[uds_ipc] Unix domain socket creation failed");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->socket_path);

    if (bind(ipc->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[uds_ipc] Failed to bind UDS socket at %s: %s\n", ipc->socket_path, strerror(errno));
        close(ipc->server_fd);
        ipc->server_fd = -1;
        return -1;
    }

    chmod(ipc->socket_path, 0777);

    if (listen(ipc->server_fd, 10) < 0) {
        perror("[uds_ipc] listen failed");
        close(ipc->server_fd);
        ipc->server_fd = -1;
        return -1;
    }

    printf("[uds_ipc] UDS IPC server initialized at %s (auto_composite=%d)\n",
           ipc->socket_path, ipc->auto_composite);
    return 0;
}

int uds_ipc_start(uds_ipc_t *ipc) {
    if (ipc->server_fd < 0 || ipc->running) return -1;

    ipc->running = 1;
    if (pthread_create(&ipc->thread, NULL, uds_accept_thread_proc, ipc) != 0) {
        ipc->running = 0;
        return -1;
    }

    printf("[uds_ipc] UDS IPC server listening at %s\n", ipc->socket_path);
    return 0;
}

void uds_ipc_stop(uds_ipc_t *ipc) {
    if (!ipc->running) return;

    ipc->running = 0;
    pthread_join(ipc->thread, NULL);

    if (ipc->server_fd >= 0) {
        close(ipc->server_fd);
        ipc->server_fd = -1;
    }
    unlink(ipc->socket_path);

    pthread_mutex_lock(&ipc->client_lock);
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        if (ipc->client_fds[i] >= 0) {
            close(ipc->client_fds[i]);
            ipc->client_fds[i] = -1;
        }
    }
    pthread_mutex_unlock(&ipc->client_lock);
    pthread_mutex_destroy(&ipc->client_lock);

    printf("[uds_ipc] UDS IPC server stopped\n");
}

int uds_ipc_sync_all_timelapses(const char *socket_path) {
    printf("[sync] Scanning /opt/usr/picture for unencoded timelapses...\n");

    // Scan /opt/usr/picture/ for unencoded image directories and encode them
    DIR *d = opendir("/opt/usr/picture");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char pic_dir[512];
            snprintf(pic_dir, sizeof(pic_dir), "/opt/usr/picture/%s", ent->d_name);
            struct stat st;
            if (stat(pic_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                char clean_name[256];
                snprintf(clean_name, sizeof(clean_name), "%s", ent->d_name);
                char *temp_sfx = strstr(clean_name, ".temp");
                if (temp_sfx) *temp_sfx = '\0';

                char out_mp4[512];
                snprintf(out_mp4, sizeof(out_mp4), "/opt/usr/video/%s.mp4", clean_name);

                struct stat st_v;
                if (stat(out_mp4, &st_v) != 0 || st_v.st_size == 0) {
                    printf("[sync] Encoding timelapse directory: '%s' -> '%s'\n", pic_dir, out_mp4);
                    int res = timelapse_encode_directory(pic_dir, out_mp4, 15, 0, 0, NULL, NULL);
                    if (res == 0) {
                        printf("[sync] Successfully encoded: '%s'\n", out_mp4);
                        char rm_cmd[600];
                        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", pic_dir);
                        system(rm_cmd);
                    } else {
                        fprintf(stderr, "[sync] Failed to encode: '%s'\n", pic_dir);
                    }
                } else {
                    printf("[sync] Video '%s' already exists, cleaning up redundant picture directory '%s'\n",
                           out_mp4, pic_dir);
                    char rm_cmd[600];
                    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", pic_dir);
                    system(rm_cmd);
                }
            }
        }
        closedir(d);
    }

    printf("[sync] Connecting to UDS socket to notify elegoo_printer...\n");

    // Connect to socket to notify elegoo_printer of all videos in /opt/usr/video
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path ? socket_path : "/tmp/aicamera_uds");

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            DIR *vd = opendir("/opt/usr/video");
            if (vd) {
                struct dirent *vent;
                while ((vent = readdir(vd)) != NULL) {
                    if (strstr(vent->d_name, ".mp4")) {
                        char vpath[512];
                        snprintf(vpath, sizeof(vpath), "/opt/usr/video/%s", vent->d_name);
                        struct stat vst;
                        if (stat(vpath, &vst) == 0 && vst.st_size > 0) {
                            char comp_cmd[1024];
                            snprintf(comp_cmd, sizeof(comp_cmd),
                                     "{\"id\":8,\"method\":\"composite_video\",\"params\":{\"video_name\":\"%s\"}}\n",
                                     vent->d_name);
                            send(sock, comp_cmd, strlen(comp_cmd), MSG_NOSIGNAL);
                            printf("[sync] Sent update to elegoo_printer for '%s' (%lld bytes)\n",
                                   vent->d_name, (long long)vst.st_size);
                            usleep(50000); // 50ms pause between syncs
                        }
                    }
                }
                closedir(vd);
            }
        } else {
            printf("[sync] Warning: Could not connect to UDS socket at %s (is ai_camera daemon running?)\n", addr.sun_path);
        }
        close(sock);
    }

    printf("[sync] Timelapse synchronization complete.\n");
    return 0;
}
