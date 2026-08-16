#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#include "timelapse_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <x264.h>



// Natural string comparison for filenames (e.g. frame_2.jpg before frame_10.jpg)
static int natural_sort_cmp(const void *a, const void *b) {
    const char *s1 = *(const char **)a;
    const char *s2 = *(const char **)b;
    return strverscmp(s1, s2);
}

// Convert RGB24 to Planar YUV420P
static void rgb24_to_yuv420p(const uint8_t *rgb, int width, int height,
                             uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane) {
    int y_stride = width;
    int uv_stride = width / 2;

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int r = rgb[(j * width + i) * 3 + 0];
            int g = rgb[(j * width + i) * 3 + 1];
            int b = rgb[(j * width + i) * 3 + 2];

            // BT.601 standard integer conversion
            int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_plane[j * y_stride + i] = (uint8_t)(y < 0 ? 0 : (y > 255 ? 255 : y));

            if ((j % 2 == 0) && (i % 2 == 0)) {
                int r2 = r, g2 = g, b2 = b;
                int count = 1;

                if (i + 1 < width) {
                    r2 += rgb[(j * width + i + 1) * 3 + 0];
                    g2 += rgb[(j * width + i + 1) * 3 + 1];
                    b2 += rgb[(j * width + i + 1) * 3 + 2];
                    count++;
                }
                if (j + 1 < height) {
                    r2 += rgb[((j + 1) * width + i) * 3 + 0];
                    g2 += rgb[((j + 1) * width + i) * 3 + 1];
                    b2 += rgb[((j + 1) * width + i) * 3 + 2];
                    count++;
                }
                if (i + 1 < width && j + 1 < height) {
                    r2 += rgb[((j + 1) * width + i + 1) * 3 + 0];
                    g2 += rgb[((j + 1) * width + i + 1) * 3 + 1];
                    b2 += rgb[((j + 1) * width + i + 1) * 3 + 2];
                    count++;
                }

                r2 /= count;
                g2 /= count;
                b2 /= count;

                int u = ((-38 * r2 - 74 * g2 + 112 * b2 + 128) >> 8) + 128;
                int v = ((112 * r2 - 94 * g2 - 18 * b2 + 128) >> 8) + 128;

                u_plane[(j / 2) * uv_stride + (i / 2)] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
                v_plane[(j / 2) * uv_stride + (i / 2)] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }
}

// MinimP4 file write callback (return 0 on success, non-zero on error)
static int mp4_write_callback(int64_t offset, const void *buffer, size_t size, void *token) {
    FILE *f = (FILE *)token;
    if (fseeko(f, offset, SEEK_SET) != 0) return 1;
    return (fwrite(buffer, 1, size, f) == size) ? 0 : 1;
}

int timelapse_encode_directory(const char *image_dir, const char *output_mp4, int fps, int width, int height,
                               timelapse_progress_cb cb, void *user_data) {
    if (!image_dir || !output_mp4) return -1;
    if (fps <= 0) fps = 15;

    DIR *dir = opendir(image_dir);
    if (!dir) {
        fprintf(stderr, "[timelapse] Failed to open image directory '%s'\n", image_dir);
        return -1;
    }

    // Collect all JPEG files
    char **file_list = NULL;
    size_t file_count = 0;
    size_t file_cap = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (strcasecmp(name + len - 4, ".jpg") == 0 || strcasecmp(name + len - 5, ".jpeg") == 0) {
            if (file_count >= file_cap) {
                file_cap = (file_cap == 0) ? 128 : file_cap * 2;
                file_list = realloc(file_list, file_cap * sizeof(char *));
            }
            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", image_dir, name);
            file_list[file_count++] = strdup(fullpath);
        }
    }
    closedir(dir);

    if (file_count == 0) {
        fprintf(stderr, "[timelapse] No JPEG files found in '%s'\n", image_dir);
        free(file_list);
        return -2;
    }

    // Sort files in natural numerical order
    qsort(file_list, file_count, sizeof(char *), natural_sort_cmp);
    printf("[timelapse] Found %zu frames in '%s'\n", file_count, image_dir);

    // Read first image to determine geometry if not provided
    int img_w = 0, img_h = 0, img_comp = 0;
    if (width <= 0 || height <= 0) {
        if (!stbi_info(file_list[0], &img_w, &img_h, &img_comp)) {
            fprintf(stderr, "[timelapse] Failed to inspect first frame '%s'\n", file_list[0]);
            for (size_t i = 0; i < file_count; i++) free(file_list[i]);
            free(file_list);
            return -3;
        }
        width = img_w;
        height = img_h;
    }

    // Width and height must be even for YUV420P
    width &= ~1;
    height &= ~1;

    // Ensure output directory exists
    char out_path_copy[512];
    snprintf(out_path_copy, sizeof(out_path_copy), "%s", output_mp4);
    char *slash = strrchr(out_path_copy, '/');
    if (slash) {
        *slash = '\0';
        char mkdir_cmd[600];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", out_path_copy);
        system(mkdir_cmd);
    }

    FILE *out_f = fopen(output_mp4, "wb+");
    if (!out_f) {
        fprintf(stderr, "[timelapse] Failed to create output video '%s'\n", output_mp4);
        for (size_t i = 0; i < file_count; i++) free(file_list[i]);
        free(file_list);
        return -4;
    }

    // Initialize MP4 Muxer
    MP4E_mux_t *mux = MP4E_open(0, 0, out_f, mp4_write_callback);
    if (!mux) {
        fprintf(stderr, "[timelapse] Failed to initialize MP4 muxer\n");
        fclose(out_f);
        for (size_t i = 0; i < file_count; i++) free(file_list[i]);
        free(file_list);
        return -5;
    }

    mp4_h26x_writer_t mp4_writer;
    if (mp4_h26x_write_init(&mp4_writer, mux, width, height, 0) != MP4E_STATUS_OK) {
        fprintf(stderr, "[timelapse] Failed to initialize H.264 MP4 writer\n");
        MP4E_close(mux);
        fclose(out_f);
        for (size_t i = 0; i < file_count; i++) free(file_list[i]);
        free(file_list);
        return -6;
    }

    // Initialize x264 parameters
    x264_param_t param;
    x264_param_default_preset(&param, "ultrafast", "zerolatency");
    param.i_width = width;
    param.i_height = height;
    param.i_fps_num = fps;
    param.i_fps_den = 1;
    param.i_csp = X264_CSP_I420;
    param.i_keyint_max = fps * 2; // Keyframe every 2 seconds
    param.i_keyint_min = fps;
    param.b_vfr_input = 0;
    param.b_repeat_headers = 1; // Repeat SPS/PPS on keyframes
    param.b_annexb = 1;

    // Ultra low memory settings (single-threaded, no lookahead buffers)
    param.i_threads = 1;
    param.i_lookahead_threads = 0;
    param.rc.i_lookahead = 0;
    param.i_bframe = 0;
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = 26.0; // Quality factor (CRF 26 is balanced for timelapse)

    x264_param_apply_profile(&param, "baseline");

    x264_t *encoder = x264_encoder_open(&param);
    if (!encoder) {
        fprintf(stderr, "[timelapse] Failed to open x264 encoder\n");
        mp4_h26x_write_close(&mp4_writer);
        MP4E_close(mux);
        fclose(out_f);
        for (size_t i = 0; i < file_count; i++) free(file_list[i]);
        free(file_list);
        return -7;
    }

    x264_picture_t pic_in, pic_out;
    x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);

    printf("[timelapse] Encoding %zu frames to '%s' (%dx%d @ %d fps)...\n",
           file_count, output_mp4, width, height, fps);

    uint32_t timescale = 90000;
    uint32_t frame_duration_90k = timescale / fps;

    for (size_t i = 0; i < file_count; i++) {
        int w = 0, h = 0, channels = 0;
        uint8_t *rgb_data = stbi_load(file_list[i], &w, &h, &channels, 3);
        if (!rgb_data) {
            fprintf(stderr, "[timelapse] Warning: Skipping unreadable frame '%s'\n", file_list[i]);
            continue;
        }

        if (w == width && h == height) {
            rgb24_to_yuv420p(rgb_data, width, height,
                             pic_in.img.plane[0], pic_in.img.plane[1], pic_in.img.plane[2]);
        } else {
            // If image resolution differs, center/crop into width x height buffer
            memset(pic_in.img.plane[0], 0x10, width * height);
            memset(pic_in.img.plane[1], 0x80, (width / 2) * (height / 2));
            memset(pic_in.img.plane[2], 0x80, (width / 2) * (height / 2));
            int copy_w = (w < width) ? w : width;
            int copy_h = (h < height) ? h : height;
            rgb24_to_yuv420p(rgb_data, copy_w, copy_h,
                             pic_in.img.plane[0], pic_in.img.plane[1], pic_in.img.plane[2]);
        }
        stbi_image_free(rgb_data);

        pic_in.i_pts = i;

        x264_nal_t *nals;
        int i_nals = 0;
        int frame_size = x264_encoder_encode(encoder, &nals, &i_nals, &pic_in, &pic_out);
        if (frame_size > 0) {
            for (int n = 0; n < i_nals; n++) {
                mp4_h26x_write_nal(&mp4_writer, nals[n].p_payload, nals[n].i_payload, frame_duration_90k);
            }
        }

        if (cb) cb((int)i + 1, (int)file_count, user_data);
    }

    // Flush encoder
    while (x264_encoder_delayed_frames(encoder) > 0) {
        x264_nal_t *nals;
        int i_nals = 0;
        int frame_size = x264_encoder_encode(encoder, &nals, &i_nals, NULL, &pic_out);
        if (frame_size > 0) {
            for (int n = 0; n < i_nals; n++) {
                mp4_h26x_write_nal(&mp4_writer, nals[n].p_payload, nals[n].i_payload, frame_duration_90k);
            }
        }
    }

    x264_picture_clean(&pic_in);
    x264_encoder_close(encoder);
    mp4_h26x_write_close(&mp4_writer);
    MP4E_close(mux);

    long final_size = ftell(out_f);
    fclose(out_f);

    for (size_t i = 0; i < file_count; i++) free(file_list[i]);
    free(file_list);

    printf("[timelapse] Successfully created '%s' (%ld bytes)\n", output_mp4, final_size);
    return 0;
}

typedef struct {
    char image_dir[256];
    char output_mp4[256];
    int fps;
    timelapse_progress_cb prog_cb;
    timelapse_finish_cb finish_cb;
    void *user_data;
} async_encode_args_t;

static void *async_encode_worker(void *arg) {
    async_encode_args_t *args = (async_encode_args_t *)arg;
    int res = timelapse_encode_directory(args->image_dir, args->output_mp4, args->fps, 0, 0,
                                         args->prog_cb, args->user_data);
    long sz = 0;
    if (res == 0) {
        FILE *f = fopen(args->output_mp4, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
        }
    }
    if (args->finish_cb) {
        args->finish_cb(args->output_mp4, sz, (res == 0), args->user_data);
    }
    free(args);
    return NULL;
}

int timelapse_encode_directory_async(const char *image_dir, const char *output_mp4, int fps,
                                     timelapse_progress_cb prog_cb,
                                     timelapse_finish_cb finish_cb,
                                     void *user_data) {
    if (!image_dir || !output_mp4) return -1;

    async_encode_args_t *args = malloc(sizeof(*args));
    if (!args) return -1;

    snprintf(args->image_dir, sizeof(args->image_dir), "%s", image_dir);
    snprintf(args->output_mp4, sizeof(args->output_mp4), "%s", output_mp4);
    args->fps = fps > 0 ? fps : 15;
    args->prog_cb = prog_cb;
    args->finish_cb = finish_cb;
    args->user_data = user_data;

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, async_encode_worker, args) != 0) {
        free(args);
        pthread_attr_destroy(&attr);
        return -1;
    }

    pthread_attr_destroy(&attr);
    return 0;
}
