#ifndef V4L2_CAMERA_H
#define V4L2_CAMERA_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "frame_ring.h"

typedef struct {
    char device_path[64];
    int width;
    int height;
    int fps;
    int quality;
    uint32_t pixelformat; // V4L2_PIX_FMT_MJPEG or V4L2_PIX_FMT_H264
    int fd;
    
    // MMAP buffers
    struct {
        void *start;
        size_t length;
    } *buffers;
    unsigned int n_buffers;

    frame_ring_t *ring;
    pthread_t thread;
    volatile int running;
} v4l2_camera_t;

int v4l2_camera_init(v4l2_camera_t *cam, const char *dev_path, int width, int height, int fps, int quality, uint32_t fmt, frame_ring_t *ring);
int v4l2_camera_start(v4l2_camera_t *cam);
void v4l2_camera_stop(v4l2_camera_t *cam);
void v4l2_camera_cleanup(v4l2_camera_t *cam);

#endif // V4L2_CAMERA_H
