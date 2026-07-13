#include "../include/args_parser.h"
#include "../include/defs.h"
#include "../include/egl.h"
#include "../include/window/window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>
#include <libgen.h>
#include <sys/stat.h>

#ifndef GSR_VERSION
#define GSR_VERSION "unknown"
#endif

static const ArgEnum video_codec_enums[] = {
    { .name = "auto",              .value = GSR_VIDEO_CODEC_AUTO              },
    { .name = "h264",              .value = GSR_VIDEO_CODEC_H264              },
    { .name = "h265",              .value = GSR_VIDEO_CODEC_HEVC              },
    { .name = "hevc",              .value = GSR_VIDEO_CODEC_HEVC              },
    { .name = "hevc_hdr",          .value = GSR_VIDEO_CODEC_HEVC_HDR          },
    { .name = "hevc_10bit",        .value = GSR_VIDEO_CODEC_HEVC_10BIT        },
    { .name = "av1",               .value = GSR_VIDEO_CODEC_AV1               },
    { .name = "av1_hdr",           .value = GSR_VIDEO_CODEC_AV1_HDR           },
    { .name = "av1_10bit",         .value = GSR_VIDEO_CODEC_AV1_10BIT         },
    { .name = "vp8",               .value = GSR_VIDEO_CODEC_VP8               },
    { .name = "vp9",               .value = GSR_VIDEO_CODEC_VP9               },
    { .name = "h264_vulkan",       .value = GSR_VIDEO_CODEC_H264_VULKAN       },
    { .name = "hevc_vulkan",       .value = GSR_VIDEO_CODEC_HEVC_VULKAN       },
    { .name = "hevc_hdr_vulkan",   .value = GSR_VIDEO_CODEC_HEVC_HDR_VULKAN   },
    { .name = "hevc_10bit_vulkan", .value = GSR_VIDEO_CODEC_HEVC_10BIT_VULKAN },
    { .name = "av1_vulkan",        .value = GSR_VIDEO_CODEC_AV1_VULKAN        },
    { .name = "av1_hdr_vulkan",    .value = GSR_VIDEO_CODEC_AV1_HDR_VULKAN    },
    { .name = "av1_10bit_vulkan",  .value = GSR_VIDEO_CODEC_AV1_10BIT_VULKAN  },
};

static const ArgEnum audio_codec_enums[] = {
    { .name = "opus", .value = GSR_AUDIO_CODEC_OPUS },
    { .name = "aac",  .value = GSR_AUDIO_CODEC_AAC  },
    { .name = "flac", .value = GSR_AUDIO_CODEC_FLAC },
};

static const ArgEnum video_encoder_enums[] = {
    { .name = "gpu", .value = GSR_VIDEO_ENCODER_HW_GPU },
    { .name = "cpu", .value = GSR_VIDEO_ENCODER_HW_CPU },
};

static const ArgEnum pixel_format_enums[] = {
    { .name = "yuv420", .value = GSR_PIXEL_FORMAT_YUV420 },
    { .name = "yuv444", .value = GSR_PIXEL_FORMAT_YUV444 },
};

static const ArgEnum framerate_mode_enums[] = {
    { .name = "vfr",     .value = GSR_FRAMERATE_MODE_VARIABLE },
    { .name = "cfr",     .value = GSR_FRAMERATE_MODE_CONSTANT },
    { .name = "content", .value = GSR_FRAMERATE_MODE_CONTENT  },
};

static const ArgEnum bitrate_mode_enums[] = {
    { .name = "auto", .value = GSR_BITRATE_MODE_AUTO },
    { .name = "qp",   .value = GSR_BITRATE_MODE_QP   },
    { .name = "cbr",  .value = GSR_BITRATE_MODE_CBR  },
    { .name = "vbr",  .value = GSR_BITRATE_MODE_VBR  },
};

static const ArgEnum color_range_enums[] = {
    { .name = "limited", .value = GSR_COLOR_RANGE_LIMITED },
    { .name = "full",    .value = GSR_COLOR_RANGE_FULL    },
};

static const ArgEnum tune_enums[] = {
    { .name = "performance", .value = GSR_TUNE_PERFORMANCE },
    { .name = "quality",     .value = GSR_TUNE_QUALITY     },
};

static const ArgEnum replay_storage_enums[] = {
    { .name = "ram",  .value = GSR_REPLAY_STORAGE_RAM  },
    { .name = "disk", .value = GSR_REPLAY_STORAGE_DISK },
};

static void arg_deinit(Arg *arg) {
    if(arg->values) {
        free(arg->values);
        arg->values = NULL;
    }
}

static bool arg_append_value(Arg *arg, const char *value) {
    if(arg->num_values + 1 >= arg->capacity_num_values) {
        const int new_capacity_num_values = arg->capacity_num_values == 0 ? 4 : arg->capacity_num_values*2;
        void *new_data = realloc(arg->values, new_capacity_num_values * sizeof(const char*));
        if(!new_data)
            return false;

        arg->values = new_data;
        arg->capacity_num_values = new_capacity_num_values;
    }

    arg->values[arg->num_values] = value;
    ++arg->num_values;
    return true;
}

static bool arg_get_enum_value_by_name(const Arg *arg, const char *name, int *enum_value) {
    assert(arg->type == ARG_TYPE_ENUM);
    assert(arg->enum_values);
    for(int i = 0; i < arg->num_enum_values; ++i) {
        if(strcmp(arg->enum_values[i].name, name) == 0) {
            *enum_value = arg->enum_values[i].value;
            return true;
        }
    }
    return false;
}

static void arg_print_expected_enum_names(const Arg *arg) {
    assert(arg->type == ARG_TYPE_ENUM);
    assert(arg->enum_values);
    for(int i = 0; i < arg->num_enum_values; ++i) {
        if(i > 0) {
            if(i == arg->num_enum_values -1)
                fprintf(stderr, " or ");
            else
                fprintf(stderr, ", ");
        }
        fprintf(stderr, "'%s'", arg->enum_values[i].name);
    }
}

static Arg* args_get_by_key(Arg *args, int num_args, const char *key) {
    for(int i = 0; i < num_args; ++i) {
        if(strcmp(args[i].key, key) == 0)
            return &args[i];
    }
    return NULL;
}

static const char* args_get_value_by_key(Arg *args, int num_args, const char *key) {
    for(int i = 0; i < num_args; ++i) {
        if(strcmp(args[i].key, key) == 0) {
            if(args[i].num_values == 0)
                return NULL;
            else
                return args[i].values[0];
        }
    }
    return NULL;
}

static bool args_get_boolean_by_key(Arg *args, int num_args, const char *key, bool default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_BOOLEAN);
        return arg->typed_value.boolean;
    }
}

static int args_get_enum_by_key(Arg *args, int num_args, const char *key, int default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_ENUM);
        return arg->typed_value.enum_value;
    }
}

static int64_t args_get_i64_by_key(Arg *args, int num_args, const char *key, int64_t default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_I64);
        return arg->typed_value.i64_value;
    }
}

static double args_get_double_by_key(Arg *args, int num_args, const char *key, double default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_DOUBLE);
        return arg->typed_value.d_value;
    }
}

static void usage_header(void) {
    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
    const char *program_name = inside_flatpak ? "flatpak run --command=gpu-screen-recorder com.dec05eba.gpu_screen_recorder" : "gpu-screen-recorder";
    printf("usage: %s -w <window_id|monitor|focused|portal|region|v4l2_device_path> [-c <container_format>] [-s WxH] [-region WxH+X+Y] [-f <fps>] [-a <audio_input>] "
           "[-q <quality>] [-r <replay_buffer_size_sec>] [-replay-storage ram|disk] [-restart-replay-on-save yes|no] "
           "[-k h264|hevc|av1|vp8|vp9|hevc_hdr|av1_hdr|hevc_10bit|av1_10bit] [-ac aac|opus|flac] [-ab <bitrate>] [-fm cfr|vfr|content] "
           "[-bm auto|qp|vbr|cbr] [-cr limited|full] [-tune performance|quality] [-df yes|no] [-sc <script_path>] [-p <plugin_path>] "
           "[-cursor yes|no] [-keyint <value>] [-restore-portal-session yes|no] [-portal-session-token-filepath filepath] [-encoder gpu|cpu] "
           "[-fallback-cpu-encoding yes|no] [-o <output_file>] [-ro <output_directory>] [-ffmpeg-opts <options>] [--list-capture-options [card_path]] "
           "[--list-monitors] [--list-audio-devices] [--list-application-audio] [--list-v4l2-devices] [-write-first-frame-ts yes|no] [-low-power yes|no] "
           "[-v yes|no] [-gl-debug yes|no] [-exclude-metadata yes|no] [--version] [-h|--help]\n", program_name);
    fflush(stdout);
}

static void usage_full(void) {
    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
    usage_header();
    printf("\n");
    printf("NOTES:\n");
    if(inside_flatpak)
        printf("  Run \"man /var/lib/flatpak/app/com.dec05eba.gpu_screen_recorder/current/active/files/share/man/man1/gpu-screen-recorder.1\" to open the man page for GPU Screen Recorder to see an explanation for each option and examples\n");
    else
        printf("  Run \"man gpu-screen-recorder.1\" to open the man page for GPU Screen Recorder to see an explanation for each option and examples\n");
    fflush(stdout);
}

static void usage(void) {
    usage_header();
}

// TODO: Does this match all livestreaming cases?
static bool is_livestream_path(const char *str) {
    const int len = strlen(str);
    if((len >= 7 && memcmp(str, "http://", 7) == 0) || (len >= 8 && memcmp(str, "https://", 8) == 0))
        return true;
    else if((len >= 7 && memcmp(str, "rtmp://", 7) == 0) || (len >= 8 && memcmp(str, "rtmps://", 8) == 0))
        return true;
    else if((len >= 7 && memcmp(str, "rtsp://", 7) == 0))
        return true;
    else if((len >= 6 && memcmp(str, "srt://", 6) == 0))
        return true;
    else if((len >= 6 && memcmp(str, "tcp://", 6) == 0))
        return true;
    else if((len >= 6 && memcmp(str, "udp://", 6) == 0))
        return true;
    else
        return false;
}

static bool file_is_pipe_or_char_device(const char *filepath) {
    struct stat st;
    if(stat(filepath, &st) == -1)
        return false;
    return S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode);
}

static bool args_parser_set_values(args_parser *self) {
    self->video_encoder = (gsr_video_encoder_hardware)args_get_enum_by_key(self->args, NUM_ARGS, "-encoder", GSR_VIDEO_ENCODER_HW_GPU);
    self->pixel_format = (gsr_pixel_format)args_get_enum_by_key(self->args, NUM_ARGS, "-pixfmt", GSR_PIXEL_FORMAT_YUV420);
    self->framerate_mode = (gsr_framerate_mode)args_get_enum_by_key(self->args, NUM_ARGS, "-fm", GSR_FRAMERATE_MODE_VARIABLE);
    self->color_range = (gsr_color_range)args_get_enum_by_key(self->args, NUM_ARGS, "-cr", GSR_COLOR_RANGE_LIMITED);
    self->tune = (gsr_tune)args_get_enum_by_key(self->args, NUM_ARGS, "-tune", GSR_TUNE_PERFORMANCE);
    self->video_codec = (gsr_video_codec)args_get_enum_by_key(self->args, NUM_ARGS, "-k", GSR_VIDEO_CODEC_AUTO);
    self->audio_codec = (gsr_audio_codec)args_get_enum_by_key(self->args, NUM_ARGS, "-ac", GSR_AUDIO_CODEC_OPUS);
    self->bitrate_mode = (gsr_bitrate_mode)args_get_enum_by_key(self->args, NUM_ARGS, "-bm", GSR_BITRATE_MODE_AUTO);
    self->replay_storage = (gsr_replay_storage)args_get_enum_by_key(self->args, NUM_ARGS, "-replay-storage", GSR_REPLAY_STORAGE_RAM);

    self->capture_source = args_get_value_by_key(self->args, NUM_ARGS, "-w");
    self->verbose = args_get_boolean_by_key(self->args, NUM_ARGS, "-v", true);
    self->gl_debug = args_get_boolean_by_key(self->args, NUM_ARGS, "-gl-debug", false);
    self->record_cursor = args_get_boolean_by_key(self->args, NUM_ARGS, "-cursor", true);
    self->date_folders = args_get_boolean_by_key(self->args, NUM_ARGS, "-df", false);
    self->restore_portal_session = args_get_boolean_by_key(self->args, NUM_ARGS, "-restore-portal-session", false);
    self->restart_replay_on_save = args_get_boolean_by_key(self->args, NUM_ARGS, "-restart-replay-on-save", false);
    const bool overclock = args_get_boolean_by_key(self->args, NUM_ARGS, "-oc", false);
    self->fallback_cpu_encoding = args_get_boolean_by_key(self->args, NUM_ARGS, "-fallback-cpu-encoding", false);
    self->write_first_frame_ts = args_get_boolean_by_key(self->args, NUM_ARGS, "-write-first-frame-ts", false);
    self->low_power = args_get_boolean_by_key(self->args, NUM_ARGS, "-low-power", false);
    self->exclude_metadata = args_get_boolean_by_key(self->args, NUM_ARGS, "-exclude-metadata", false);

    self->audio_bitrate = args_get_i64_by_key(self->args, NUM_ARGS, "-ab", 0);
    self->audio_bitrate *= 1000LL;

    self->keyint = args_get_double_by_key(self->args, NUM_ARGS, "-keyint", 2.0);

    if(overclock) {
        fprintf(stderr, "gsr info: the overclock option (-oc) is deprecated and no longer has any effect as it's no longer needed (on GPUs that are 12 years old or younger)\n");
    }

    if(self->audio_codec == GSR_AUDIO_CODEC_FLAC) {
        fprintf(stderr, "gsr warning: flac audio codec is temporary disabled, using opus audio codec instead\n");
        self->audio_codec = GSR_AUDIO_CODEC_OPUS;
    }

    self->portal_session_token_filepath = args_get_value_by_key(self->args, NUM_ARGS, "-portal-session-token-filepath");
    if(self->portal_session_token_filepath) {
        int len = strlen(self->portal_session_token_filepath);
        if(len > 0 && self->portal_session_token_filepath[len - 1] == '/') {
            fprintf(stderr, "gsr error: -portal-session-token-filepath should be a path to a file but it ends with a /: %s\n", self->portal_session_token_filepath);
            return false;
        }
    }

    self->recording_saved_script = args_get_value_by_key(self->args, NUM_ARGS, "-sc");
    if(self->recording_saved_script) {
        struct stat buf;
        if(stat(self->recording_saved_script, &buf) == -1 || !S_ISREG(buf.st_mode)) {
            fprintf(stderr, "gsr error: Script \"%s\" either doesn't exist or it's not a file\n", self->recording_saved_script);
            usage();
            return false;
        }

        if(!(buf.st_mode & S_IXUSR)) {
            fprintf(stderr, "gsr error: Script \"%s\" is not executable\n", self->recording_saved_script);
            usage();
            return false;
        }
    }

    const char *quality_str = args_get_value_by_key(self->args, NUM_ARGS, "-q");
    self->video_quality = GSR_VIDEO_QUALITY_VERY_HIGH;
    self->video_bitrate = 0;

    if(self->bitrate_mode == GSR_BITRATE_MODE_CBR) {
        if(!quality_str) {
            fprintf(stderr, "gsr error: option '-q' is required when using '-bm cbr' option\n");
            usage();
            return false;
        }

        if(sscanf(quality_str, "%" PRIi64, &self->video_bitrate) != 1) {
            fprintf(stderr, "gsr error: -q argument \"%s\" is not an integer value. When using '-bm cbr' option '-q' is expected to be an integer value\n", quality_str);
            usage();
            return false;
        }

        if(self->video_bitrate < 0) {
            fprintf(stderr, "gsr error: -q is expected to be 0 or larger, got %" PRIi64 "\n", self->video_bitrate);
            usage();
            return false;
        }

        self->video_bitrate *= 1000LL;
    } else {
        if(!quality_str)
            quality_str = "very_high";

        if(strcmp(quality_str, "medium") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_MEDIUM;
        } else if(strcmp(quality_str, "high") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_HIGH;
        } else if(strcmp(quality_str, "very_high") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_VERY_HIGH;
        } else if(strcmp(quality_str, "ultra") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_ULTRA;
        } else {
            fprintf(stderr, "gsr error: -q should either be 'medium', 'high', 'very_high' or 'ultra', got: '%s'\n", quality_str);
            usage();
            return false;
        }
    }

    self->output_resolution = (vec2i){0, 0};

    const char *output_resolution_str = args_get_value_by_key(self->args, NUM_ARGS, "-s");
    if(output_resolution_str) {
        if(sscanf(output_resolution_str, "%dx%d", &self->output_resolution.x, &self->output_resolution.y) != 2) {
            fprintf(stderr, "gsr error: invalid value for option -s '%s', expected a value in format WxH\n", output_resolution_str);
            usage();
            return false;
        }

        if(self->output_resolution.x < 0 || self->output_resolution.y < 0) {
            fprintf(stderr, "gsr error: invalid value for option -s '%s', expected width and height to be greater or equal to 0\n", output_resolution_str);
            usage();
            return false;
        }
    }

    self->region_size = (vec2i){0, 0};
    self->region_position = (vec2i){0, 0};
    const char *region_str = args_get_value_by_key(self->args, NUM_ARGS, "-region");
    if(region_str) {
        if(sscanf(region_str, "%dx%d+%d+%d", &self->region_size.x, &self->region_size.y, &self->region_position.x, &self->region_position.y) != 4) {
            fprintf(stderr, "gsr error: invalid value for option -region '%s', expected a value in format WxH+X+Y\n", region_str);
            usage();
            return false;
        }

        if(self->region_size.x < 0 || self->region_size.y < 0) {
            fprintf(stderr, "gsr error: invalid value for option -region '%s', expected width and height to be greater or equal to 0\n", region_str);
            usage();
            return false;
        }
    }

    self->fps = args_get_i64_by_key(self->args, NUM_ARGS, "-f", 60);
    self->replay_buffer_size_secs = args_get_i64_by_key(self->args, NUM_ARGS, "-r", -1);
    if(self->replay_buffer_size_secs != -1)
        self->replay_buffer_size_secs += (int64_t)(self->keyint + 0.5); // Add a few seconds to account of lost packets because of non-keyframe packets skipped

    self->container_format = args_get_value_by_key(self->args, NUM_ARGS, "-c");
    if(self->container_format && strcmp(self->container_format, "mkv") == 0)
        self->container_format = "matroska";

    self->is_replaying = self->replay_buffer_size_secs != -1;
    self->is_livestream = false;
    self->filename = args_get_value_by_key(self->args, NUM_ARGS, "-o");
    if(self->filename) {
        self->is_livestream = is_livestream_path(self->filename);
        if(self->is_livestream) {
            if(self->is_replaying) {
                fprintf(stderr, "gsr error: replay mode is not applicable to live streaming\n");
                return false;
            }
        } else {
            if(!self->is_replaying) {
                char directory_buf[PATH_MAX];
                snprintf(directory_buf, sizeof(directory_buf), "%s", self->filename);
                char *directory = dirname(directory_buf);
                if(strcmp(directory, ".") != 0 && strcmp(directory, "/") != 0) {
                    if(create_directory_recursive(directory) != 0) {
                        fprintf(stderr, "gsr error: failed to create directory for output file: %s\n", self->filename);
                        return false;
                    }
                }
            } else {
                if(!self->container_format) {
                    fprintf(stderr, "gsr error: option -c is required when using option -r\n");
                    usage();
                    return false;
                }

                struct stat buf;
                if(stat(self->filename, &buf) != -1 && !S_ISDIR(buf.st_mode)) {
                    fprintf(stderr, "gsr error: File \"%s\" exists but it's not a directory\n", self->filename);
                    usage();
                    return false;
                }
            }
        }
    } else {
        if(!self->is_replaying) {
            self->filename = "/dev/stdout";
        } else {
            fprintf(stderr, "gsr error: Option -o is required when using option -r\n");
            usage();
            return false;
        }

        if(!self->container_format) {
            fprintf(stderr, "gsr error: option -c is required when not using option -o\n");
            usage();
            return false;
        }
    }

    self->is_output_piped = file_is_pipe_or_char_device(self->filename);
    self->low_latency_recording = self->is_livestream || self->is_output_piped;
    if(self->write_first_frame_ts && (self->is_livestream || self->is_output_piped)) {
        fprintf(stderr, "gsr warning: -write-first-frame-ts is ignored for livestreaming or when output is piped\n");
        self->write_first_frame_ts = false;
    }

    self->replay_recording_directory = args_get_value_by_key(self->args, NUM_ARGS, "-ro");

    if(self->is_livestream && self->recording_saved_script) {
        fprintf(stderr, "gsr warning: live stream detected, -sc script is ignored\n");
        self->recording_saved_script = NULL;
    }

    self->ffmpeg_opts = args_get_value_by_key(self->args, NUM_ARGS, "-ffmpeg-opts");
    self->ffmpeg_video_opts = args_get_value_by_key(self->args, NUM_ARGS, "-ffmpeg-video-opts");
    self->ffmpeg_audio_opts = args_get_value_by_key(self->args, NUM_ARGS, "-ffmpeg-audio-opts");

    return true;
}

bool args_parser_parse(args_parser *self, int argc, char **argv, const args_handlers *arg_handlers, void *userdata) {
    assert(arg_handlers);
    memset(self, 0, sizeof(*self));

    if(argc <= 1) {
        usage_full();
        return false;
    }

    if(argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage_full();
        return false;
    }

    if(argc == 2 && strcmp(argv[1], "--info") == 0) {
        arg_handlers->info(userdata);
        return true;
    }

    if(argc == 2 && strcmp(argv[1], "--list-audio-devices") == 0) {
        arg_handlers->list_audio_devices(userdata);
        return true;
    }

    if(argc == 2 && strcmp(argv[1], "--list-application-audio") == 0) {
        arg_handlers->list_application_audio(userdata);
        return true;
    }

    if(argc == 2 && strcmp(argv[1], "--list-v4l2-devices") == 0) {
        arg_handlers->list_v4l2_devices(userdata);
        return true;
    }

    if(strcmp(argv[1], "--list-capture-options") == 0) {
        if(argc == 2) {
            arg_handlers->list_capture_options(NULL, userdata);
            return true;
        } else if(argc == 3 || argc == 4) {
            const char *card_path = argv[2];
            arg_handlers->list_capture_options(card_path, userdata);
            return true;
        } else {
            fprintf(stderr, "gsr error: expected --list-capture-options to be called with either no extra arguments or 1 extra argument (card path)\n");
            return false;
        }
    }

    if(strcmp(argv[1], "--list-monitors") == 0) {
        arg_handlers->list_monitors(userdata);
        return true;
    }

    if(argc == 2 && strcmp(argv[1], "--version") == 0) {
        arg_handlers->version(userdata);
        return true;
    }

    int arg_index = 0;
    self->args[arg_index++] = (Arg){ .key = "-w",                             .optional = false, .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-c",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-f",                             .optional = true,  .list = false, .type = ARG_TYPE_I64, .integer_value_min = 1, .integer_value_max = 1000 };
    self->args[arg_index++] = (Arg){ .key = "-s",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-region",                        .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-a",                             .optional = true,  .list = true,  .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-q",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-o",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-ro",                            .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-r",                             .optional = true,  .list = false, .type = ARG_TYPE_I64, .integer_value_min = 2, .integer_value_max = 86400 };
    self->args[arg_index++] = (Arg){ .key = "-restart-replay-on-save",        .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-k",                             .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = video_codec_enums, .num_enum_values = sizeof(video_codec_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-ac",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = audio_codec_enums, .num_enum_values = sizeof(audio_codec_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-ab",                            .optional = true,  .list = false, .type = ARG_TYPE_I64, .integer_value_min = 0, .integer_value_max = 50000 };
    self->args[arg_index++] = (Arg){ .key = "-oc",                            .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-fm",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = framerate_mode_enums, .num_enum_values = sizeof(framerate_mode_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-bm",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = bitrate_mode_enums, .num_enum_values = sizeof(bitrate_mode_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-pixfmt",                        .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = pixel_format_enums, .num_enum_values = sizeof(pixel_format_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-v",                             .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-gl-debug",                      .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-df",                            .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-sc",                            .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-cr",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = color_range_enums, .num_enum_values = sizeof(color_range_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-tune",                          .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = tune_enums, .num_enum_values = sizeof(tune_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-cursor",                        .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-keyint",                        .optional = true,  .list = false, .type = ARG_TYPE_DOUBLE, .integer_value_min = 0, .integer_value_max = 500 };
    self->args[arg_index++] = (Arg){ .key = "-restore-portal-session",        .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-portal-session-token-filepath", .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-encoder",                       .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = video_encoder_enums, .num_enum_values = sizeof(video_encoder_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-fallback-cpu-encoding",         .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-replay-storage",                .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = replay_storage_enums, .num_enum_values = sizeof(replay_storage_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-p",                             .optional = true,  .list = true,  .type = ARG_TYPE_STRING };
    self->args[arg_index++] = (Arg){ .key = "-ffmpeg-opts",                   .optional = true,  .list = false, .type = ARG_TYPE_STRING };
    self->args[arg_index++] = (Arg){ .key = "-ffmpeg-video-opts",             .optional = true,  .list = false, .type = ARG_TYPE_STRING };
    self->args[arg_index++] = (Arg){ .key = "-ffmpeg-audio-opts",             .optional = true,  .list = false, .type = ARG_TYPE_STRING };
    self->args[arg_index++] = (Arg){ .key = "-write-first-frame-ts",          .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-low-power",                     .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-exclude-metadata",              .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    assert(arg_index == NUM_ARGS);

    for(int i = 1; i < argc; i += 2) {
        const char *arg_name = argv[i];
        Arg *arg = args_get_by_key(self->args, NUM_ARGS, arg_name);
        if(!arg) {
            fprintf(stderr, "gsr error: invalid argument '%s'\n", arg_name);
            usage();
            return false;
        }

        if(arg->num_values > 0 && !arg->list) {
            fprintf(stderr, "gsr error: expected argument '%s' to only be specified once\n", arg_name);
            usage();
            return false;
        }

        if(i + 1 >= argc) {
            fprintf(stderr, "gsr error: missing value for argument '%s'\n", arg_name);
            usage();
            return false;
        }

        const char *arg_value = argv[i + 1];
        switch(arg->type) {
            case ARG_TYPE_STRING: {
                break;
            }
            case ARG_TYPE_BOOLEAN: {
                if(strcmp(arg_value, "yes") == 0) {
                    arg->typed_value.boolean = true;
                } else if(strcmp(arg_value, "no") == 0) {
                    arg->typed_value.boolean = false;
                } else {
                    fprintf(stderr, "gsr error: %s should either be 'yes' or 'no', got: '%s'\n", arg_name, arg_value);
                    usage();
                    return false;
                }
                break;
            }
            case ARG_TYPE_ENUM: {
                if(!arg_get_enum_value_by_name(arg, arg_value, &arg->typed_value.enum_value)) {
                    fprintf(stderr, "gsr error: %s should either be ", arg_name);
                    arg_print_expected_enum_names(arg);
                    fprintf(stderr, ", got: '%s'\n", arg_value);
                    usage();
                    return false;
                }
                break;
            }
            case ARG_TYPE_I64: {
                if(sscanf(arg_value, "%" PRIi64, &arg->typed_value.i64_value) != 1) {
                    fprintf(stderr, "gsr error: %s argument \"%s\" is not an integer\n", arg_name, arg_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.i64_value < arg->integer_value_min) {
                    fprintf(stderr, "gsr error: %s argument is expected to be larger than %" PRIi64 ", got %" PRIi64 "\n", arg_name, arg->integer_value_min, arg->typed_value.i64_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.i64_value > arg->integer_value_max) {
                    fprintf(stderr, "gsr error: %s argument is expected to be less than %" PRIi64 ", got %" PRIi64 "\n", arg_name, arg->integer_value_max, arg->typed_value.i64_value);
                    usage();
                    return false;
                }
                break;
            }
            case ARG_TYPE_DOUBLE: {
                if(sscanf(arg_value, "%lf", &arg->typed_value.d_value) != 1) {
                    fprintf(stderr, "gsr error: %s argument \"%s\" is not an floating-point number\n", arg_name, arg_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.d_value < arg->integer_value_min) {
                    fprintf(stderr, "gsr error: %s argument is expected to be larger than %" PRIi64 ", got %lf\n", arg_name, arg->integer_value_min, arg->typed_value.d_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.d_value > arg->integer_value_max) {
                    fprintf(stderr, "gsr error: %s argument is expected to be less than %" PRIi64 ", got %lf\n", arg_name, arg->integer_value_max, arg->typed_value.d_value);
                    usage();
                    return false;
                }
                break;
            }
        }

        if(!arg_append_value(arg, arg_value)) {
            fprintf(stderr, "gsr error: failed to append argument, out of memory\n");
            return false;
        }
    }

    for(int i = 0; i < NUM_ARGS; ++i) {
        const Arg *arg = &self->args[i];
        if(!arg->optional && arg->num_values == 0) {
            fprintf(stderr, "gsr error: missing argument '%s'\n", arg->key);
            usage();
            return false;
        }
    }

    return args_parser_set_values(self);
}

void args_parser_deinit(args_parser *self) {
    for(int i = 0; i < NUM_ARGS; ++i) {
        arg_deinit(&self->args[i]);
    }
}

bool args_parser_validate_with_gl_info(args_parser *self, gsr_egl *egl) {
    const bool wayland = gsr_window_get_display_server(egl->window) == GSR_DISPLAY_SERVER_WAYLAND;

    if(self->bitrate_mode == (gsr_bitrate_mode)GSR_BITRATE_MODE_AUTO) {
        // QP is broken on steam deck, see https://github.com/ValveSoftware/SteamOS/issues/1609
        self->bitrate_mode = egl->gpu_info.is_steam_deck ? GSR_BITRATE_MODE_VBR : GSR_BITRATE_MODE_QP;
    }

    if(egl->gpu_info.is_steam_deck && self->bitrate_mode == GSR_BITRATE_MODE_QP) {
        fprintf(stderr, "gsr warning: qp bitrate mode is not supported on Steam Deck because of Steam Deck driver bugs. Using vbr instead\n");
        self->bitrate_mode = GSR_BITRATE_MODE_VBR;
    }

    if(self->video_encoder == GSR_VIDEO_ENCODER_HW_CPU && self->bitrate_mode == GSR_BITRATE_MODE_VBR) {
        fprintf(stderr, "gsr warning: bitrate mode has been forcefully set to qp because software encoding option doesn't support vbr option\n");
        self->bitrate_mode = GSR_BITRATE_MODE_QP;
    }

    if(egl->gpu_info.is_steam_deck) {
        fprintf(stderr, "gsr warning: steam deck has multiple driver issues. One of them has been reported here: https://github.com/ValveSoftware/SteamOS/issues/1609\n"
            "If you have issues with GPU Screen Recorder on steam deck that you don't have on a desktop computer then report the issue to Valve and/or AMD.\n");
    }

    self->very_old_gpu = false;
    if(egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA && egl->gpu_info.gpu_version != 0 && egl->gpu_info.gpu_version < 900) {
        fprintf(stderr, "gsr info: your gpu appears to be very old (older than maxwell architecture). Switching to lower preset\n");
        self->very_old_gpu = true;
    }

    if(video_codec_is_hdr(self->video_codec) && !wayland) {
        fprintf(stderr, "gsr error: hdr video codec option %s is not available on X11\n", video_codec_to_string(self->video_codec));
        usage();
        return false;
    }

    return true;
}

void args_parser_print_usage(void) {
    usage();
}

Arg* args_parser_get_arg(args_parser *self, const char *arg_name) {
    return args_get_by_key(self->args, NUM_ARGS, arg_name);
}
