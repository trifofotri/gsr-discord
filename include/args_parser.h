#ifndef GSR_ARGS_PARSER_H
#define GSR_ARGS_PARSER_H

#include <stdbool.h>
#include <stdint.h>
#include "defs.h"
#include "vec2.h"

typedef struct gsr_egl gsr_egl;

#define NUM_ARGS 38

typedef enum {
    GSR_CAPTURE_SOURCE_TYPE_WINDOW,
    GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW,
    GSR_CAPTURE_SOURCE_TYPE_MONITOR,
    GSR_CAPTURE_SOURCE_TYPE_REGION,
    GSR_CAPTURE_SOURCE_TYPE_PORTAL,
    GSR_CAPTURE_SOURCE_TYPE_V4L2
} CaptureSourceType;

typedef enum {
    ARG_TYPE_STRING,
    ARG_TYPE_BOOLEAN,
    ARG_TYPE_ENUM,
    ARG_TYPE_I64,
    ARG_TYPE_DOUBLE,
} ArgType;

typedef struct {
    const char *name;
    int value;
} ArgEnum;

typedef struct {
    ArgType type;
    const char **values;
    int capacity_num_values;
    int num_values;

    const char *key;
    bool optional;
    bool list;

    const ArgEnum *enum_values;
    int num_enum_values;

    int64_t integer_value_min;
    int64_t integer_value_max;

    union {
        bool boolean;
        int enum_value;
        int64_t i64_value;
        double d_value;
    } typed_value;
} Arg;

typedef struct {
    void (*version)(void *userdata);
    void (*info)(void *userdata);
    void (*list_audio_devices)(void *userdata);
    void (*list_application_audio)(void *userdata);
    void (*list_v4l2_devices)(void *userdata);
    void (*list_capture_options)(const char *card_path, void *userdata);
    void (*list_monitors)(void *userdata);
} args_handlers;

typedef struct {
    Arg args[NUM_ARGS];

    gsr_video_encoder_hardware video_encoder;
    gsr_pixel_format pixel_format;
    gsr_framerate_mode framerate_mode;
    gsr_color_range color_range;
    gsr_tune tune;
    gsr_video_codec video_codec;
    gsr_audio_codec audio_codec;
    gsr_bitrate_mode bitrate_mode;
    gsr_video_quality video_quality;
    gsr_replay_storage replay_storage;

    const char *capture_source;
    const char *container_format;
    const char *filename;
    const char *replay_recording_directory;
    const char *portal_session_token_filepath;
    const char *recording_saved_script;
    const char *ffmpeg_opts;
    const char *ffmpeg_video_opts;
    const char *ffmpeg_audio_opts;
    bool verbose;
    bool gl_debug;
    bool fallback_cpu_encoding;
    bool low_power;
    bool exclude_metadata;
    bool record_cursor;
    bool date_folders;
    bool restore_portal_session;
    bool restart_replay_on_save;
    bool write_first_frame_ts;
    bool is_replaying;
    bool is_livestream;
    bool is_output_piped;
    bool low_latency_recording;
    bool very_old_gpu;
    int64_t video_bitrate;
    int64_t audio_bitrate;
    int64_t fps;
    int64_t replay_buffer_size_secs;
    double keyint;
    vec2i output_resolution;
    vec2i region_size;
    vec2i region_position;
} args_parser;

/* |argv| is stored as a reference */
bool args_parser_parse(args_parser *self, int argc, char **argv, const args_handlers *args_handlers, void *userdata);
void args_parser_deinit(args_parser *self);

bool args_parser_validate_with_gl_info(args_parser *self, gsr_egl *egl);
void args_parser_print_usage(void);
Arg* args_parser_get_arg(args_parser *self, const char *arg_name);

#endif /* GSR_ARGS_PARSER_H */
