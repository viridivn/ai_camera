#ifndef TIMELAPSE_ENCODER_H
#define TIMELAPSE_ENCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*timelapse_progress_cb)(int current_frame, int total_frames, void *user_data);
typedef void (*timelapse_finish_cb)(const char *output_mp4, long file_size, int success, void *user_data);

/**
 * Encodes all JPEG images in `image_dir` into an H.264 MP4 video file at `output_mp4`.
 */
int timelapse_encode_directory(const char *image_dir, const char *output_mp4, int fps, int width, int height,
                               timelapse_progress_cb prog_cb, void *user_data);

/**
 * Starts encoding in a detached background thread with progress & finish callbacks.
 */
int timelapse_encode_directory_async(const char *image_dir, const char *output_mp4, int fps,
                                     timelapse_progress_cb prog_cb,
                                     timelapse_finish_cb finish_cb,
                                     void *user_data);

#ifdef __cplusplus
}
#endif

#endif // TIMELAPSE_ENCODER_H
