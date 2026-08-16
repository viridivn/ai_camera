#include "v4l2_camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

static int xioctl(int fd, int request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (-1 == r && EINTR == errno);
    return r;
}

static void *camera_thread_proc(void *arg) {
    v4l2_camera_t *cam = (v4l2_camera_t *)arg;

    struct pollfd fds[1];
    fds[0].fd = cam->fd;
    fds[0].events = POLLIN;

    while (cam->running) {
        int r = poll(fds, 1, 1000); // 1 sec timeout
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll error");
            break;
        }
        if (r == 0) continue;

        if (fds[0].revents & POLLIN) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;
                perror("VIDIOC_DQBUF error");
                break;
            }

            if (buf.index < cam->n_buffers && buf.bytesused > 0) {
                frame_ring_push(cam->ring, cam->buffers[buf.index].start, buf.bytesused);
            }

            if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
                perror("VIDIOC_QBUF error");
                break;
            }
        }
    }

    return NULL;
}

int v4l2_camera_init(v4l2_camera_t *cam, const char *dev_path, int width, int height, int fps, int quality, uint32_t fmt, frame_ring_t *ring) {
    memset(cam, 0, sizeof(*cam));
    snprintf(cam->device_path, sizeof(cam->device_path), "%s", dev_path ? dev_path : "/dev/video0");
    cam->width = width > 0 ? width : 1280;
    cam->height = height > 0 ? height : 720;
    cam->fps = fps > 0 ? fps : 0;
    cam->quality = quality > 0 ? quality : 0;
    cam->pixelformat = fmt ? fmt : V4L2_PIX_FMT_MJPEG;
    cam->fd = -1;
    cam->ring = ring;

    cam->fd = open(cam->device_path, O_RDWR | O_NONBLOCK, 0);
    if (cam->fd < 0) {
        fprintf(stderr, "Cannot open video device '%s': %s\n", cam->device_path, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap;
    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "%s is not a V4L2 device\n", cam->device_path);
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support video capture streaming\n", cam->device_path);
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    // Set Format
    struct v4l2_format vfmt;
    memset(&vfmt, 0, sizeof(vfmt));
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vfmt.fmt.pix.width = cam->width;
    vfmt.fmt.pix.height = cam->height;
    vfmt.fmt.pix.pixelformat = cam->pixelformat;
    vfmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &vfmt) < 0) {
        // If requested format failed, try MJPEG fallback if H.264 was requested
        if (cam->pixelformat == V4L2_PIX_FMT_H264) {
            fprintf(stderr, "H.264 not accepted by %s, falling back to MJPEG\n", cam->device_path);
            cam->pixelformat = V4L2_PIX_FMT_MJPEG;
            vfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
            if (xioctl(cam->fd, VIDIOC_S_FMT, &vfmt) < 0) {
                fprintf(stderr, "Failed to set MJPEG format on %s: %s\n", cam->device_path, strerror(errno));
                close(cam->fd);
                cam->fd = -1;
                return -1;
            }
        } else {
            fprintf(stderr, "Failed to set format on %s: %s\n", cam->device_path, strerror(errno));
            close(cam->fd);
            cam->fd = -1;
            return -1;
        }
    }

    cam->width = vfmt.fmt.pix.width;
    cam->height = vfmt.fmt.pix.height;
    cam->pixelformat = vfmt.fmt.pix.pixelformat;

    // Set FPS if specified
    if (cam->fps > 0) {
        struct v4l2_streamparm parm;
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = cam->fps;
        if (xioctl(cam->fd, VIDIOC_S_PARM, &parm) == 0) {
            printf("[v4l2_camera] Target framerate set to %d fps\n", cam->fps);
        }
    }

    // Set Quality if specified
    if (cam->quality > 0) {
        struct v4l2_control ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        ctrl.value = cam->quality;
        if (xioctl(cam->fd, VIDIOC_S_CTRL, &ctrl) == 0) {
            printf("[v4l2_camera] JPEG compression quality set to %d%%\n", cam->quality);
        }
    }

    char fourcc[5] = {0};
    memcpy(fourcc, &cam->pixelformat, 4);
    printf("[v4l2_camera] Negotiated format: %s (%dx%d)\n", fourcc, cam->width, cam->height);

    // Request MMAP buffers
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        fprintf(stderr, "VIDIOC_REQBUFS error on %s\n", cam->device_path);
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    cam->buffers = calloc(req.count, sizeof(*cam->buffers));
    if (!cam->buffers) {
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    for (cam->n_buffers = 0; cam->n_buffers < req.count; cam->n_buffers++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = cam->n_buffers;

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QUERYBUF error\n");
            v4l2_camera_cleanup(cam);
            return -1;
        }

        cam->buffers[cam->n_buffers].length = buf.length;
        cam->buffers[cam->n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, buf.m.offset);
        if (cam->buffers[cam->n_buffers].start == MAP_FAILED) {
            fprintf(stderr, "mmap error\n");
            v4l2_camera_cleanup(cam);
            return -1;
        }
    }

    return 0;
}

int v4l2_camera_start(v4l2_camera_t *cam) {
    if (cam->fd < 0 || cam->running) return -1;

    for (unsigned int i = 0; i < cam->n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QBUF error\n");
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "VIDIOC_STREAMON error: %s\n", strerror(errno));
        return -1;
    }

    cam->running = 1;
    if (pthread_create(&cam->thread, NULL, camera_thread_proc, cam) != 0) {
        cam->running = 0;
        xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
        return -1;
    }

    printf("[v4l2_camera] Camera streaming started on %s\n", cam->device_path);
    return 0;
}

void v4l2_camera_stop(v4l2_camera_t *cam) {
    if (!cam->running) return;

    cam->running = 0;
    pthread_join(cam->thread, NULL);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    printf("[v4l2_camera] Camera streaming stopped\n");
}

void v4l2_camera_cleanup(v4l2_camera_t *cam) {
    v4l2_camera_stop(cam);

    if (cam->buffers) {
        for (unsigned int i = 0; i < cam->n_buffers; i++) {
            if (cam->buffers[i].start && cam->buffers[i].start != MAP_FAILED) {
                munmap(cam->buffers[i].start, cam->buffers[i].length);
            }
        }
        free(cam->buffers);
        cam->buffers = NULL;
    }

    if (cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }
}
