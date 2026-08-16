#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include "frame_ring.h"
#include "v4l2_camera.h"
#include "http_server.h"
#include "uds_ipc.h"

static volatile int keep_running = 1;

static void sig_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        printf("\n[main] Signal %d received, shutting down...\n", signo);
        keep_running = 0;
    }
}

#include "timelapse_encoder.h"

static void print_usage(const char *prog_name) {
    printf("ai_camera - replacement for eleegoo stock camera service\n\n");
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -d, --device PATH          V4L2 camera device path (default: /dev/video0)\n");
    printf("  -w, --width INT            Capture width (default: 1280)\n");
    printf("  -h, --height INT           Capture height (default: 720)\n");
    printf("  -r, --fps INT              Target capture / timelapse framerate (e.g. 15, 30)\n");
    printf("  -q, --quality INT          JPEG compression quality 1-100\n");
    printf("  -p, --port INT             HTTP stream port (default: 8080)\n");
    printf("  -s, --socket PATH          UDS IPC socket path (default: /tmp/aicamera_uds)\n");
    printf("  -f, --format STR           Format: H264 or MJPEG (default: MJPEG)\n");
    printf("  -S, --snapshot PATH        (for testing) Capture a single frame to PATH and exit\n");
    printf("  -E, --encode-timelapse DIR (for testing) Encode directory of JPEGs to MP4 and exit\n");
    printf("  -o, --output PATH          Output MP4 file path for timelapse encoding\n");
    printf("      --help                 Display this message and exit\n");
}

int main(int argc, char **argv) {
    char device_path[64] = "/dev/video0";
    int width = 1280;
    int height = 720;
    int fps = 0;
    int quality = 0;
    int http_port = 8080;
    char socket_path[128] = "/tmp/aicamera_uds";
    char snapshot_path[256] = {0};
    char encode_dir[256] = {0};
    char output_mp4[256] = "/opt/usr/video/timelapse.mp4";
    uint32_t format = V4L2_PIX_FMT_MJPEG;

    static struct option long_options[] = {
        {"device", required_argument, 0, 'd'},
        {"width", required_argument, 0, 'w'},
        {"height", required_argument, 0, 'h'},
        {"fps", required_argument, 0, 'r'},
        {"quality", required_argument, 0, 'q'},
        {"port", required_argument, 0, 'p'},
        {"socket", required_argument, 0, 's'},
        {"format", required_argument, 0, 'f'},
        {"snapshot", required_argument, 0, 'S'},
        {"encode-timelapse", required_argument, 0, 'E'},
        {"output", required_argument, 0, 'o'},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:w:h:r:q:p:s:f:S:E:o:?", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd': snprintf(device_path, sizeof(device_path), "%s", optarg); break;
            case 'w': width = atoi(optarg); break;
            case 'h': height = atoi(optarg); break;
            case 'r': fps = atoi(optarg); break;
            case 'q': quality = atoi(optarg); break;
            case 'p': http_port = atoi(optarg); break;
            case 's': snprintf(socket_path, sizeof(socket_path), "%s", optarg); break;
            case 'S': snprintf(snapshot_path, sizeof(snapshot_path), "%s", optarg); break;
            case 'E': snprintf(encode_dir, sizeof(encode_dir), "%s", optarg); break;
            case 'o': snprintf(output_mp4, sizeof(output_mp4), "%s", optarg); break;
            case 'f':
                if (strcasecmp(optarg, "MJPEG") == 0 || strcasecmp(optarg, "JPG") == 0) {
                    format = V4L2_PIX_FMT_MJPEG;
                } else if (strcasecmp(optarg, "H264") == 0 || strcasecmp(optarg, "264") == 0) {
                    format = V4L2_PIX_FMT_H264;
                }
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return (opt == '?') ? 0 : 1;
        }
    }

    // Direct standalone timelapse encoding mode (-E / --encode-timelapse)
    if (encode_dir[0] != '\0') {
        int encode_fps = fps > 0 ? fps : 15;
        printf("[TEST MODE] Encoding timelapse from '%s' to '%s' (%d fps)...\n",
               encode_dir, output_mp4, encode_fps);
        int res = timelapse_encode_directory(encode_dir, output_mp4, encode_fps, width, height, NULL, NULL);
        if (res == 0) {
            printf("[TEST MODE] Timelapse encoding completed successfully: '%s'\n", output_mp4);
            return 0;
        } else {
            fprintf(stderr, "[TEST MODE] Timelapse encoding failed with error code %d\n", res);
            return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    frame_ring_t ring;
    if (frame_ring_init(&ring) != 0) {
        fprintf(stderr, "Failed to initialize frame ring buffer\n");
        return 1;
    }

    v4l2_camera_t camera;
    if (v4l2_camera_init(&camera, device_path, width, height, fps, quality, format, &ring) != 0) {
        fprintf(stderr, "Error: Failed to initialize camera at %s\n", device_path);
        frame_ring_cleanup(&ring);
        return 1;
    }

    v4l2_camera_start(&camera);

    // If Test Snapshot mode requested (-S / --snapshot)
    if (snapshot_path[0] != '\0') {
        printf("[TEST MODE] Capturing snapshot to '%s'...\n", snapshot_path);
        uint8_t *frame_buf = malloc(MAX_FRAME_SIZE);
        size_t frame_size = 0;
        uint64_t seq = 0;
        int r = frame_ring_get_latest(&ring, 0, frame_buf, MAX_FRAME_SIZE, &frame_size, &seq, 3000);
        if (r == 0 && frame_size > 0) {
            FILE *f = fopen(snapshot_path, "wb");
            if (f) {
                fwrite(frame_buf, 1, frame_size, f);
                fclose(f);
                printf("[TEST MODE] Snapshot successfully saved to '%s' (%zu bytes, seq %llu)\n", snapshot_path, frame_size, (unsigned long long)seq);
            } else {
                fprintf(stderr, "[TEST MODE] Failed to open '%s' for writing\n", snapshot_path);
            }
        } else {
            fprintf(stderr, "[TEST MODE] Failed to acquire frame from camera within timeout\n");
        }
        free(frame_buf);
        v4l2_camera_cleanup(&camera);
        frame_ring_cleanup(&ring);
        return 0;
    }

    printf("Starting camera service\n");
    printf("Device: %s (%dx%d, format: %s)\n", device_path, camera.width, camera.height,
           camera.pixelformat == V4L2_PIX_FMT_H264 ? "H264" : "MJPEG");
    printf("HTTP Port: %d\n", http_port);
    printf("UDS Socket: %s\n", socket_path);

    http_server_t http;
    if (http_server_init(&http, http_port, &ring) == 0) {
        http_server_start(&http);
    }

    uds_ipc_t uds;
    if (uds_ipc_init(&uds, socket_path, &ring) == 0) {
        uds_ipc_start(&uds);
    }

    while (keep_running) {
        sleep(1);
    }

    printf("Shutting down camera service...\n");
    uds_ipc_stop(&uds);
    http_server_stop(&http);
    v4l2_camera_cleanup(&camera);
    frame_ring_cleanup(&ring);

    printf("Shutdown complete.\n");
    return 0;
}
