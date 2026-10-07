#ifndef GSR_ENCODER_H
#define GSR_ENCODER_H

#include "../replay_buffer/replay_buffer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define GSR_MAX_RECORDING_DESTINATIONS 128

typedef struct AVCodecContext AVCodecContext;
typedef struct AVFormatContext AVFormatContext;
typedef struct AVStream AVStream;

typedef void (*gsr_video_packet_callback)(void *userdata, const uint8_t *data, int size, bool is_keyframe, int64_t pts);
typedef void (*gsr_audio_packet_callback)(void *userdata, const uint8_t *data, int size, int64_t pts, int stream_index);

typedef struct {
    size_t id;
    AVCodecContext *codec_context;
    AVFormatContext *format_context;
    AVStream *stream;
    int64_t start_pts;
    int64_t first_pts;
    bool has_received_keyframe;
    char *first_frame_ts_filepath;
    bool first_frame_ts_written;
} gsr_encoder_recording_destination;

typedef struct {
    gsr_replay_buffer *replay_buffer;
    int64_t first_pts;

    pthread_mutex_t file_write_mutex;
    bool file_write_mutex_created;

    pthread_mutex_t replay_mutex;
    bool replay_mutex_created;

    gsr_encoder_recording_destination recording_destinations[GSR_MAX_RECORDING_DESTINATIONS];
    size_t num_recording_destinations;
    size_t recording_destination_id_counter;
    
    gsr_video_packet_callback on_video_packet;
    void *on_video_packet_userdata;

    gsr_audio_packet_callback on_audio_packet;
    void *on_audio_packet_userdata;
} gsr_encoder;

bool gsr_encoder_init(gsr_encoder *self, gsr_replay_storage replay_storage, size_t replay_buffer_num_packets, double replay_buffer_time, const char *replay_directory);
void gsr_encoder_deinit(gsr_encoder *self);

void gsr_encoder_receive_packets(gsr_encoder *self, AVCodecContext *codec_context, int64_t pts, int stream_index);
/* Returns the id to the recording destination, or -1 on error */
size_t gsr_encoder_add_recording_destination(gsr_encoder *self, AVCodecContext *codec_context, AVFormatContext *format_context, AVStream *stream, int64_t start_pts);
bool gsr_encoder_remove_recording_destination(gsr_encoder *self, size_t id);
bool gsr_encoder_set_recording_destination_first_frame_ts_filepath(gsr_encoder *self, size_t id, const char *filepath);

void gsr_encoder_set_video_packet_callback(gsr_encoder *self, gsr_video_packet_callback cb, void *userdata);
void gsr_encoder_set_audio_packet_callback(gsr_encoder *self, gsr_audio_packet_callback cb, void *userdata);

#endif /* GSR_ENCODER_H */
