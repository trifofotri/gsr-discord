extern "C" {
#include "../include/capture/nvfbc.h"
#include "../include/capture/xcomposite.h"
#include "../include/capture/ximage.h"
#include "../include/capture/kms.h"
#include "../include/capture/v4l2.h"
#ifdef GSR_PORTAL
#include "../include/capture/portal.h"
#include "../include/dbus.h"
#endif
#ifdef GSR_APP_AUDIO
#include "../include/pipewire_audio.h"
#endif
#include "../include/encoder/encoder.h"
#include "../include/encoder/video/nvenc.h"
#include "../include/encoder/video/vaapi.h"
#include "../include/encoder/video/vulkan.h"
#include "../include/encoder/video/software.h"
#include "../include/codec_query/nvenc.h"
#include "../include/codec_query/vaapi.h"
#include "../include/codec_query/vulkan.h"
#include "../include/window/x11.h"
#include "../include/window/wayland.h"
#include "../include/egl.h"
#include "../include/utils.h"
#include "../include/damage.h"
#include "../include/color_conversion.h"
#include "../include/image_writer.h"
#include "../include/args_parser.h"
#include "../include/plugins.h"
#include "../kms/client/kms_client.h"
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <mutex>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <libgen.h>
#include <malloc.h>

#include "../include/sound.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <future>

#ifndef GSR_VERSION
#define GSR_VERSION "unknown"
#endif

// TODO: If options are not supported then they are returned (allocated) in the options. This should be free'd.

// TODO: Remove LIBAVUTIL_VERSION_MAJOR checks in the future when ubuntu, pop os LTS etc update ffmpeg to >= 5.0

static const int AUDIO_SAMPLE_RATE = 48000;

static const int VIDEO_STREAM_INDEX = 0;

static thread_local char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

typedef struct {
    const gsr_window *window;
} MonitorOutputCallbackUserdata;

static void monitor_output_callback_print(const gsr_monitor *monitor, void *userdata) {
    const MonitorOutputCallbackUserdata *options = (MonitorOutputCallbackUserdata*)userdata;
    vec2i monitor_position = monitor->pos;
    vec2i monitor_size = monitor->size;
    if(gsr_window_get_display_server(options->window) == GSR_DISPLAY_SERVER_WAYLAND) {
        gsr_monitor_rotation monitor_rotation = GSR_MONITOR_ROT_0;
        drm_monitor_get_display_server_data(options->window, monitor, &monitor_rotation, &monitor_position);
        if(monitor_rotation == GSR_MONITOR_ROT_90 || monitor_rotation == GSR_MONITOR_ROT_270)
            std::swap(monitor_size.x, monitor_size.y);
    }
    fprintf(stderr, "  \"%.*s\"    (%dx%d+%d+%d)\n", monitor->name_len, monitor->name, monitor_size.x, monitor_size.y, monitor_position.x, monitor_position.y);
}

typedef struct {
    char *output_name;
} FirstOutputCallback;

static void get_first_output_callback(const gsr_monitor *monitor, void *userdata) {
    FirstOutputCallback *data = (FirstOutputCallback*)userdata;
    if(!data->output_name)
        data->output_name = strdup(monitor->name);
}

typedef struct {
    gsr_window *window;
    vec2i position;
    char *output_name;
    vec2i monitor_pos;
    vec2i monitor_size;
    double monitor_scale_inverted;
} MonitorByPositionCallback;

static void get_monitor_by_position_callback(const gsr_monitor *monitor, void *userdata) {
    MonitorByPositionCallback *data = (MonitorByPositionCallback*)userdata;

    const vec2i monitor_position = monitor->logical_pos;
    const vec2i monitor_size = monitor->size;
    const vec2i monitor_logical_size = monitor->logical_size;

    if(!data->output_name && data->position.x >= monitor_position.x && data->position.x <= monitor_position.x + monitor_logical_size.x
        && data->position.y >= monitor_position.y && data->position.y <= monitor_position.y + monitor_logical_size.y)
    {
        data->output_name = strdup(monitor->name);
        data->monitor_pos = monitor_position;
        data->monitor_size = monitor_size;
        data->monitor_scale_inverted = (double)monitor_size.x / (double)monitor_logical_size.x;
    }
}

static char* av_error_to_string(int err) {
    if(av_strerror(err, av_error_buffer, sizeof(av_error_buffer)) < 0)
        strcpy(av_error_buffer, "Unknown error");
    return av_error_buffer;
}

static int x11_error_handler(Display*, XErrorEvent*) {
    return 0;
}

static int x11_io_error_handler(Display*) {
    return 0;
}

static AVCodecID audio_codec_get_id(gsr_audio_codec audio_codec) {
    switch(audio_codec) {
        case GSR_AUDIO_CODEC_AAC:  return AV_CODEC_ID_AAC;
        case GSR_AUDIO_CODEC_OPUS: return AV_CODEC_ID_OPUS;
        case GSR_AUDIO_CODEC_FLAC: return AV_CODEC_ID_FLAC;
    }
    assert(false);
    return AV_CODEC_ID_AAC;
}

static AVSampleFormat audio_codec_get_sample_format(AVCodecContext *audio_codec_context, gsr_audio_codec audio_codec, const AVCodec *codec, bool mix_audio) {
    (void)audio_codec_context;
    switch(audio_codec) {
        case GSR_AUDIO_CODEC_AAC: {
            return AV_SAMPLE_FMT_FLTP;
        }
        case GSR_AUDIO_CODEC_OPUS: {
            bool supports_s16 = false;
            bool supports_flt = false;

            #if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 15, 0)
            for(size_t i = 0; codec->sample_fmts && codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
                if(codec->sample_fmts[i] == AV_SAMPLE_FMT_S16) {
                    supports_s16 = true;
                } else if(codec->sample_fmts[i] == AV_SAMPLE_FMT_FLT) {
                    supports_flt = true;
                }
            }
            #else
            const enum AVSampleFormat *sample_fmts = NULL;
            if(avcodec_get_supported_config(audio_codec_context, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void**)&sample_fmts, NULL) >= 0) {
                if(sample_fmts) {
                    for(size_t i = 0; sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
                        if(sample_fmts[i] == AV_SAMPLE_FMT_S16) {
                            supports_s16 = true;
                        } else if(sample_fmts[i] == AV_SAMPLE_FMT_FLT) {
                            supports_flt = true;
                        }
                    }
                } else {
                    // What a dumb API. It returns NULL if all formats are supported
                    supports_s16 = true;
                    supports_flt = true;
                }
            }
            #endif

            // Amix only works with float audio
            if(mix_audio)
                supports_s16 = false;

            if(!supports_s16 && !supports_flt) {
                fprintf(stderr, "gsr warning: opus audio codec is chosen but your ffmpeg version does not support s16/flt sample format and performance might be slightly worse.\n");
                fprintf(stderr, "  You can either rebuild ffmpeg with libopus instead of the built-in opus, use the flatpak version of gpu screen recorder or record with aac audio codec instead (-ac aac).\n");
                fprintf(stderr, "  Falling back to fltp audio sample format instead.\n");
            }

            if(supports_s16)
                return AV_SAMPLE_FMT_S16;
            else if(supports_flt)
                return AV_SAMPLE_FMT_FLT;
            else
                return AV_SAMPLE_FMT_FLTP;
        }
        case GSR_AUDIO_CODEC_FLAC: {
            return AV_SAMPLE_FMT_S32;
        }
    }
    assert(false);
    return AV_SAMPLE_FMT_FLTP;
}

static int64_t audio_codec_get_get_bitrate(gsr_audio_codec audio_codec) {
    switch(audio_codec) {
        case GSR_AUDIO_CODEC_AAC:  return 160000;
        case GSR_AUDIO_CODEC_OPUS: return 128000;
        case GSR_AUDIO_CODEC_FLAC: return 128000;
    }
    assert(false);
    return 128000;
}

static AudioFormat audio_codec_context_get_audio_format(const AVCodecContext *audio_codec_context) {
    switch(audio_codec_context->sample_fmt) {
        case AV_SAMPLE_FMT_FLT:   return F32;
        case AV_SAMPLE_FMT_FLTP:  return S32;
        case AV_SAMPLE_FMT_S16:   return S16;
        case AV_SAMPLE_FMT_S32:   return S32;
        default:                  return S16;
    }
}

static AVSampleFormat audio_format_to_sample_format(const AudioFormat audio_format) {
    switch(audio_format) {
        case S16:   return AV_SAMPLE_FMT_S16;
        case S32:   return AV_SAMPLE_FMT_S32;
        case F32:   return AV_SAMPLE_FMT_FLT;
    }
    assert(false);
    return AV_SAMPLE_FMT_S16;
}

static AVCodecContext* create_audio_codec_context(int fps, gsr_audio_codec audio_codec, bool mix_audio, int64_t audio_bitrate) {
    (void)fps;
    const AVCodec *codec = avcodec_find_encoder(audio_codec_get_id(audio_codec));
    if (!codec) {
        fprintf(stderr, "gsr error: Could not find %s audio encoder\n", audio_codec_get_name(audio_codec));
        _exit(1);
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    assert(codec->type == AVMEDIA_TYPE_AUDIO);
    codec_context->codec_id = codec->id;
    codec_context->sample_fmt = audio_codec_get_sample_format(codec_context, audio_codec, codec, mix_audio);
    codec_context->bit_rate = audio_bitrate == 0 ? audio_codec_get_get_bitrate(audio_codec) : audio_bitrate;
    codec_context->sample_rate = AUDIO_SAMPLE_RATE;
    if(audio_codec == GSR_AUDIO_CODEC_AAC) {
#if LIBAVCODEC_VERSION_MAJOR < 62
        codec_context->profile = FF_PROFILE_AAC_LOW;
#else
        codec_context->profile = AV_PROFILE_AAC_LOW;
#endif
    }
#if LIBAVCODEC_VERSION_MAJOR < 60
    codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    codec_context->channels = 2;
#else
    av_channel_layout_default(&codec_context->ch_layout, 2);
#endif

    codec_context->time_base.num = 1;
    codec_context->time_base.den = codec_context->sample_rate;
    codec_context->thread_count = 1;
    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static int vbr_get_quality_parameter(AVCodecContext *codec_context, gsr_video_quality video_quality, bool hdr) {
    // 8 bit / 10 bit = 80%
    const float qp_multiply = hdr ? 8.0f/10.0f : 1.0f;
    if(codec_context->codec_id == AV_CODEC_ID_AV1) {
        switch(video_quality) {
            case GSR_VIDEO_QUALITY_MEDIUM:
                return 160 * qp_multiply;
            case GSR_VIDEO_QUALITY_HIGH:
                return 130 * qp_multiply;
            case GSR_VIDEO_QUALITY_VERY_HIGH:
                return 110 * qp_multiply;
            case GSR_VIDEO_QUALITY_ULTRA:
                return 90 * qp_multiply;
        }
    } else if(codec_context->codec_id == AV_CODEC_ID_H264) {
        switch(video_quality) {
            case GSR_VIDEO_QUALITY_MEDIUM:
                return 35 * qp_multiply;
            case GSR_VIDEO_QUALITY_HIGH:
                return 30 * qp_multiply;
            case GSR_VIDEO_QUALITY_VERY_HIGH:
                return 25 * qp_multiply;
            case GSR_VIDEO_QUALITY_ULTRA:
                return 22 * qp_multiply;
        }
    } else if(codec_context->codec_id == AV_CODEC_ID_HEVC) {
        switch(video_quality) {
            case GSR_VIDEO_QUALITY_MEDIUM:
                return 35 * qp_multiply;
            case GSR_VIDEO_QUALITY_HIGH:
                return 30 * qp_multiply;
            case GSR_VIDEO_QUALITY_VERY_HIGH:
                return 25 * qp_multiply;
            case GSR_VIDEO_QUALITY_ULTRA:
                return 22 * qp_multiply;
        }
    } else if(codec_context->codec_id == AV_CODEC_ID_VP8 || codec_context->codec_id == AV_CODEC_ID_VP9) {
        switch(video_quality) {
            case GSR_VIDEO_QUALITY_MEDIUM:
                return 35 * qp_multiply;
            case GSR_VIDEO_QUALITY_HIGH:
                return 30 * qp_multiply;
            case GSR_VIDEO_QUALITY_VERY_HIGH:
                return 25 * qp_multiply;
            case GSR_VIDEO_QUALITY_ULTRA:
                return 22 * qp_multiply;
        }
    }
    assert(false);
    return 22 * qp_multiply;
}

static AVCodecContext *create_video_codec_context(AVPixelFormat pix_fmt, const AVCodec *codec, const gsr_egl &egl, const args_parser &arg_parser, int width, int height) {
    const bool use_software_video_encoder = arg_parser.video_encoder == GSR_VIDEO_ENCODER_HW_CPU;
    const bool hdr = video_codec_is_hdr(arg_parser.video_codec);
    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    //double fps_ratio = (double)fps / 30.0;

    assert(codec->type == AVMEDIA_TYPE_VIDEO);
    codec_context->codec_id = codec->id;
    codec_context->width = width;
    codec_context->height = height;
    // Timebase: This is the fundamental unit of time (in seconds) in terms
    // of which frame timestamps are represented. For fixed-fps content,
    // timebase should be 1/framerate and timestamp increments should be
    // identical to 1
    codec_context->time_base.num = 1;
    codec_context->time_base.den = arg_parser.framerate_mode == GSR_FRAMERATE_MODE_CONSTANT ? arg_parser.fps : AV_TIME_BASE;
    codec_context->framerate.num = arg_parser.fps;
    codec_context->framerate.den = 1;
    codec_context->sample_aspect_ratio.num = 0;
    codec_context->sample_aspect_ratio.den = 0;
    if(arg_parser.low_latency_recording) {
        codec_context->flags |= (AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY);
        codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
        //codec_context->gop_size = std::numeric_limits<int>::max();
        //codec_context->keyint_min = std::numeric_limits<int>::max();
        codec_context->gop_size = arg_parser.fps * arg_parser.keyint;
    } else {
        // High values reduce file size but increases time it takes to seek
        codec_context->gop_size = arg_parser.fps * arg_parser.keyint;
    }
    codec_context->max_b_frames = 0;
    codec_context->pix_fmt = pix_fmt;
    codec_context->color_range = arg_parser.color_range == GSR_COLOR_RANGE_LIMITED ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
    if(hdr) {
        codec_context->color_primaries = AVCOL_PRI_BT2020;
        codec_context->color_trc = AVCOL_TRC_SMPTE2084;
        codec_context->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
        codec_context->color_primaries = AVCOL_PRI_BT709;
        codec_context->color_trc = AVCOL_TRC_BT709;
        codec_context->colorspace = AVCOL_SPC_BT709;
    }
    //codec_context->chroma_sample_location = AVCHROMA_LOC_CENTER;
    // Can't use this because it's fucking broken in ffmpeg 8 or new mesa. It produces garbage output
    //if(codec->id == AV_CODEC_ID_HEVC)
    //    codec_context->codec_tag = MKTAG('h', 'v', 'c', '1'); // QuickTime on MacOS requires this or the video wont be playable

    if(arg_parser.bitrate_mode == GSR_BITRATE_MODE_CBR) {
        codec_context->bit_rate = arg_parser.video_bitrate;
        codec_context->rc_max_rate = codec_context->bit_rate;
        //codec_context->rc_min_rate = codec_context->bit_rate;
        codec_context->rc_buffer_size = codec_context->bit_rate;//codec_context->bit_rate / 10;
        codec_context->rc_initial_buffer_occupancy = 0;//codec_context->bit_rate;//codec_context->bit_rate * 1000;
    } else if(arg_parser.bitrate_mode == GSR_BITRATE_MODE_VBR) {
        const int quality = vbr_get_quality_parameter(codec_context, arg_parser.video_quality, hdr);
        switch(arg_parser.video_quality) {
            case GSR_VIDEO_QUALITY_MEDIUM:
                codec_context->qmin = quality;
                codec_context->qmax = quality;
                codec_context->bit_rate = 100000;//4500000 + (codec_context->width * codec_context->height)*0.75;
                break;
            case GSR_VIDEO_QUALITY_HIGH:
                codec_context->qmin = quality;
                codec_context->qmax = quality;
                codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
                break;
            case GSR_VIDEO_QUALITY_VERY_HIGH:
                codec_context->qmin = quality;
                codec_context->qmax = quality;
                codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
                break;
            case GSR_VIDEO_QUALITY_ULTRA:
                codec_context->qmin = quality;
                codec_context->qmax = quality;
                codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
                break;
        }

        codec_context->rc_max_rate = codec_context->bit_rate;
        //codec_context->rc_min_rate = codec_context->bit_rate;
        codec_context->rc_buffer_size = codec_context->bit_rate;//codec_context->bit_rate / 10;
        codec_context->rc_initial_buffer_occupancy = codec_context->bit_rate;//codec_context->bit_rate * 1000;
    } else {
        //codec_context->rc_buffer_size = 50000 * 1000;
    }
    //codec_context->profile = FF_PROFILE_H264_MAIN;
    if (codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
        codec_context->mb_decision = 2;

    if(!use_software_video_encoder && (egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA || video_codec_is_vulkan(arg_parser.video_codec)) && arg_parser.bitrate_mode != GSR_BITRATE_MODE_CBR) {
        // 8 bit / 10 bit = 80%, and increase it even more
        const float quality_multiply = hdr ? (8.0f/10.0f * 0.7f) : 1.0f;
        if(codec_context->codec_id == AV_CODEC_ID_AV1 || codec_context->codec_id == AV_CODEC_ID_H264 || codec_context->codec_id == AV_CODEC_ID_HEVC) {
            switch(arg_parser.video_quality) {
                case GSR_VIDEO_QUALITY_MEDIUM:
                    codec_context->global_quality = 130 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_HIGH:
                    codec_context->global_quality = 110 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_VERY_HIGH:
                    codec_context->global_quality = 95 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_ULTRA:
                    codec_context->global_quality = 85 * quality_multiply;
                    break;
            }
        } else if(codec_context->codec_id == AV_CODEC_ID_VP8) {
            switch(arg_parser.video_quality) {
                case GSR_VIDEO_QUALITY_MEDIUM:
                    codec_context->global_quality = 35 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_HIGH:
                    codec_context->global_quality = 30 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_VERY_HIGH:
                    codec_context->global_quality = 25 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_ULTRA:
                    codec_context->global_quality = 10 * quality_multiply;
                    break;
            }
        } else if(codec_context->codec_id == AV_CODEC_ID_VP9) {
            switch(arg_parser.video_quality) {
                case GSR_VIDEO_QUALITY_MEDIUM:
                    codec_context->global_quality = 35 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_HIGH:
                    codec_context->global_quality = 30 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_VERY_HIGH:
                    codec_context->global_quality = 25 * quality_multiply;
                    break;
                case GSR_VIDEO_QUALITY_ULTRA:
                    codec_context->global_quality = 10 * quality_multiply;
                    break;
            }
        }
    }

    av_opt_set_int(codec_context->priv_data, "b_ref_mode", 0, 0);
    //av_opt_set_int(codec_context->priv_data, "cbr", true, 0);

    if(egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA || video_codec_is_vulkan(arg_parser.video_codec)) {
        // TODO: More options, better options
        //codec_context->bit_rate = codec_context->width * codec_context->height;
        switch(arg_parser.bitrate_mode) {
            case GSR_BITRATE_MODE_QP: {
                if(video_codec_is_vulkan(arg_parser.video_codec))
                    av_opt_set(codec_context->priv_data, "rc_mode", "cqp", 0);
                else if(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA)
                    av_opt_set(codec_context->priv_data, "rc", "constqp", 0);
                else
                    av_opt_set(codec_context->priv_data, "rc_mode", "CQP", 0);
                break;
            }
            case GSR_BITRATE_MODE_VBR: {
                if(video_codec_is_vulkan(arg_parser.video_codec))
                    av_opt_set(codec_context->priv_data, "rc_mode", "vbr", 0);
                else if(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA)
                    av_opt_set(codec_context->priv_data, "rc", "vbr", 0);
                else
                    av_opt_set(codec_context->priv_data, "rc_mode", "VBR", 0);
                break;
            }
            case GSR_BITRATE_MODE_CBR: {
                if(video_codec_is_vulkan(arg_parser.video_codec))
                    av_opt_set(codec_context->priv_data, "rc_mode", "cbr", 0);
                else if(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA)
                    av_opt_set(codec_context->priv_data, "rc", "cbr", 0);
                else
                    av_opt_set(codec_context->priv_data, "rc_mode", "CBR", 0);
                break;
            }
        }
        //codec_context->global_quality = 4;
        //codec_context->compression_level = 2;
    }

    //av_opt_set(codec_context->priv_data, "bsf", "hevc_metadata=colour_primaries=9:transfer_characteristics=16:matrix_coefficients=9", 0);

    if(arg_parser.tune == GSR_TUNE_QUALITY)
        codec_context->max_b_frames = 2;

    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static void open_audio(AVCodecContext *audio_codec_context, const char *ffmpeg_audio_opts) {
    AVDictionary *options = nullptr;
    av_dict_set(&options, "strict", "experimental", 0);

    if(ffmpeg_audio_opts)
        av_dict_parse_string(&options, ffmpeg_audio_opts, "=", ";", 0);

    int ret;
    ret = avcodec_open2(audio_codec_context, audio_codec_context->codec, &options);
    if(ret < 0) {
        fprintf(stderr, "failed to open codec, reason: %s\n", av_error_to_string(ret));
        _exit(1);
    }
}

static AVFrame* create_audio_frame(AVCodecContext *audio_codec_context) {
    AVFrame *frame = av_frame_alloc();
    if(!frame) {
        fprintf(stderr, "failed to allocate audio frame\n");
        _exit(1);
    }

    frame->sample_rate = audio_codec_context->sample_rate;
    frame->nb_samples = audio_codec_context->frame_size;
    frame->format = audio_codec_context->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR < 60
    frame->channels = audio_codec_context->channels;
    frame->channel_layout = audio_codec_context->channel_layout;
#else
    av_channel_layout_copy(&frame->ch_layout, &audio_codec_context->ch_layout);
#endif

    int ret = av_frame_get_buffer(frame, 0);
    if(ret < 0) {
        fprintf(stderr, "failed to allocate audio data buffers, reason: %s\n", av_error_to_string(ret));
        _exit(1);
    }

    return frame;
}

static void dict_set_profile(AVCodecContext *codec_context, gsr_gpu_vendor vendor, gsr_color_depth color_depth, gsr_video_codec video_codec, AVDictionary **options) {
    #if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 17, 100)
    if(codec_context->codec_id == AV_CODEC_ID_H264) {
        // TODO: Only for vaapi
        //if(color_depth == GSR_COLOR_DEPTH_10_BITS)
        //    av_dict_set(options, "profile", "high10", 0);
        //else
        av_dict_set(options, "profile", "high", 0);
    } else if(codec_context->codec_id == AV_CODEC_ID_AV1) {
        if(vendor == GSR_GPU_VENDOR_NVIDIA) {
            if(color_depth == GSR_COLOR_DEPTH_10_BITS)
                av_dict_set_int(options, "highbitdepth", 1, 0);
        } else {
            av_dict_set(options, "profile", "main", 0); // TODO: use professional instead?
        }
    } else if(codec_context->codec_id == AV_CODEC_ID_HEVC) {
        if(color_depth == GSR_COLOR_DEPTH_10_BITS)
            av_dict_set(options, "profile", "main10", 0);
        else
            av_dict_set(options, "profile", "main", 0);
    }
    #else
    const bool use_nvidia_values = vendor == GSR_GPU_VENDOR_NVIDIA && !video_codec_is_vulkan(video_codec);
    if(codec_context->codec_id == AV_CODEC_ID_H264) {
        // TODO: Only for vaapi
        //if(color_depth == GSR_COLOR_DEPTH_10_BITS)
        //    av_dict_set_int(options, "profile", AV_PROFILE_H264_HIGH_10, 0);
        //else
        av_dict_set_int(options, "profile", use_nvidia_values ? 2 : AV_PROFILE_H264_HIGH, 0);
    } else if(codec_context->codec_id == AV_CODEC_ID_AV1) {
        if(use_nvidia_values) {
            if(color_depth == GSR_COLOR_DEPTH_10_BITS)
                av_dict_set_int(options, "highbitdepth", 1, 0);
        } else {
            av_dict_set_int(options, "profile", AV_PROFILE_AV1_MAIN, 0); // TODO: use professional instead?
        }
    } else if(codec_context->codec_id == AV_CODEC_ID_HEVC) {
        if(color_depth == GSR_COLOR_DEPTH_10_BITS)
            av_dict_set_int(options, "profile", use_nvidia_values ? 1 : AV_PROFILE_HEVC_MAIN_10, 0);
        else
            av_dict_set_int(options, "profile", use_nvidia_values ? 0 : AV_PROFILE_HEVC_MAIN, 0);
    }
    #endif
}

static void video_software_set_qp(AVCodecContext *codec_context, gsr_video_quality video_quality, bool hdr, AVDictionary **options) {
    // 8 bit / 10 bit = 80%
    const float qp_multiply = hdr ? 8.0f/10.0f : 1.0f;
    switch(video_quality) {
        case GSR_VIDEO_QUALITY_MEDIUM:
            av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
            break;
        case GSR_VIDEO_QUALITY_HIGH:
            av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
            break;
        case GSR_VIDEO_QUALITY_VERY_HIGH:
            av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
            break;
        case GSR_VIDEO_QUALITY_ULTRA:
            av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
            break;
    }
}

static void open_video_software(AVCodecContext *codec_context, const args_parser &arg_parser) {
    const bool hdr = video_codec_is_hdr(arg_parser.video_codec);
    AVDictionary *options = nullptr;

    if(arg_parser.bitrate_mode == GSR_BITRATE_MODE_QP)
        video_software_set_qp(codec_context, arg_parser.video_quality, hdr, &options);

    av_dict_set(&options, "preset", "veryfast", 0);
    av_dict_set(&options, "tune", "film", 0);
    av_dict_set_int(&options, "forced-idr", 1, 0);

    if(codec_context->codec_id == AV_CODEC_ID_H264) {
        av_dict_set(&options, "coder", "cabac", 0); // TODO: cavlc is faster than cabac but worse compression. Which to use?
    }

    av_dict_set(&options, "strict", "experimental", 0);

    if(arg_parser.ffmpeg_video_opts)
        av_dict_parse_string(&options, arg_parser.ffmpeg_video_opts, "=", ";", 0);

    int ret = avcodec_open2(codec_context, codec_context->codec, &options);
    if (ret < 0) {
        fprintf(stderr, "gsr error: Could not open video codec: %s\n", av_error_to_string(ret));
        _exit(1);
    }
}

static void video_set_rc(gsr_video_codec video_codec, gsr_gpu_vendor vendor, gsr_bitrate_mode bitrate_mode, AVDictionary **options) {
    switch(bitrate_mode) {
        case GSR_BITRATE_MODE_QP: {
            if(video_codec_is_vulkan(video_codec))
                av_dict_set(options, "rc_mode", "cqp", 0);
            else if(vendor == GSR_GPU_VENDOR_NVIDIA)
                av_dict_set(options, "rc", "constqp", 0);
            else
                av_dict_set(options, "rc_mode", "CQP", 0);
            break;
        }
        case GSR_BITRATE_MODE_VBR: {
            if(video_codec_is_vulkan(video_codec))
                av_dict_set(options, "rc_mode", "vbr", 0);
            else if(vendor == GSR_GPU_VENDOR_NVIDIA)
                av_dict_set(options, "rc", "vbr", 0);
            else
                av_dict_set(options, "rc_mode", "VBR", 0);
            break;
        }
        case GSR_BITRATE_MODE_CBR: {
            if(video_codec_is_vulkan(video_codec))
                av_dict_set(options, "rc_mode", "cbr", 0);
            else if(vendor == GSR_GPU_VENDOR_NVIDIA)
                av_dict_set(options, "rc", "cbr", 0);
            else
                av_dict_set(options, "rc_mode", "CBR", 0);
            break;
        }
    }
}

static void video_hardware_set_qp(AVCodecContext *codec_context, gsr_video_quality video_quality, gsr_gpu_vendor vendor, bool hdr, AVDictionary **options) {
    // 8 bit / 10 bit = 80%
    const float qp_multiply = hdr ? 8.0f/10.0f : 1.0f;
    switch(video_quality) {
        case GSR_VIDEO_QUALITY_MEDIUM:
            av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
            break;
        case GSR_VIDEO_QUALITY_HIGH:
            av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
            break;
        case GSR_VIDEO_QUALITY_VERY_HIGH:
            av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
            break;
        case GSR_VIDEO_QUALITY_ULTRA:
            av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
            break;
    }
}

static void open_video_hardware(AVCodecContext *codec_context, bool low_power, const gsr_egl &egl, const args_parser &arg_parser) {
    const gsr_color_depth color_depth = video_codec_to_bit_depth(arg_parser.video_codec);
    const bool hdr = video_codec_is_hdr(arg_parser.video_codec);
    AVDictionary *options = nullptr;

    if(arg_parser.bitrate_mode == GSR_BITRATE_MODE_QP)
        video_hardware_set_qp(codec_context, arg_parser.video_quality, egl.gpu_info.vendor, hdr, &options);

    video_set_rc(arg_parser.video_codec, egl.gpu_info.vendor, arg_parser.bitrate_mode, &options);

    // TODO: Enable multipass

    dict_set_profile(codec_context, egl.gpu_info.vendor, color_depth, arg_parser.video_codec, &options);

    if(video_codec_is_vulkan(arg_parser.video_codec)) {
        av_dict_set_int(&options, "async_depth", 3, 0);
        av_dict_set(&options, "tune", "ll", 0); // Low latency
        av_dict_set(&options, "usage", arg_parser.is_livestream ? "stream" : "record", 0);
        av_dict_set(&options, "content", "rendered", 0); // Game or 3D content

        if(codec_context->codec_id == AV_CODEC_ID_H264) {
            // Removed because it causes stutter in games for some people
            //av_dict_set_int(&options, "quality", 5, 0); // quality preset
        } else if(codec_context->codec_id == AV_CODEC_ID_AV1) {
            av_dict_set(&options, "tier", "main", 0);
        } else if(codec_context->codec_id == AV_CODEC_ID_HEVC) {
            if(hdr)
                av_dict_set(&options, "sei", "hdr", 0);
        }
    } else if(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA) {
        // TODO: These dont seem to be necessary
        // av_dict_set_int(&options, "zerolatency", 1, 0);
        // if(codec_context->codec_id == AV_CODEC_ID_AV1) {
        //     av_dict_set(&options, "tune", "ll", 0);
        // } else if(codec_context->codec_id == AV_CODEC_ID_H264 || codec_context->codec_id == AV_CODEC_ID_HEVC) {
        //     av_dict_set(&options, "preset", "llhq", 0);
        //     av_dict_set(&options, "tune", "ll", 0);
        // }
        av_dict_set(&options, "tune", "ll", 0);
        av_dict_set_int(&options, "forced-idr", 1, 0);

        switch(arg_parser.tune) {
            case GSR_TUNE_PERFORMANCE:
                //av_dict_set(&options, "multipass", "qres", 0);
                break;
            case GSR_TUNE_QUALITY:
                av_dict_set(&options, "multipass", "fullres", 0);
                av_dict_set(&options, "preset", "p6", 0);
                av_dict_set_int(&options, "rc-lookahead", 0, 0);
                break;
        }

        if(codec_context->codec_id == AV_CODEC_ID_H264) {
            // TODO: h264 10bit?
            // TODO:
            // switch(pixel_format) {
            //     case GSR_PIXEL_FORMAT_YUV420:
            //         av_dict_set_int(&options, "profile", AV_PROFILE_H264_HIGH, 0);
            //         break;
            //     case GSR_PIXEL_FORMAT_YUV444:
            //         av_dict_set_int(&options, "profile", AV_PROFILE_H264_HIGH_444, 0);
            //         break;
            // }
        } else if(codec_context->codec_id == AV_CODEC_ID_AV1) {
            switch(arg_parser.pixel_format) {
                case GSR_PIXEL_FORMAT_YUV420:
                    av_dict_set(&options, "rgb_mode", "yuv420", 0);
                    break;
                case GSR_PIXEL_FORMAT_YUV444:
                    av_dict_set(&options, "rgb_mode", "yuv444", 0);
                    break;
            }
        } else if(codec_context->codec_id == AV_CODEC_ID_HEVC) {
            //av_dict_set(&options, "pix_fmt", "yuv420p16le", 0);
        }
    } else {
        // TODO: More quality options
        if(low_power)
            av_dict_set_int(&options, "low_power", 1, 0);
        // Improves performance but increases vram.
        // TODO: Might need a different async_depth for optimal performance on different amd/intel gpus
        av_dict_set_int(&options, "async_depth", 3, 0);

        if(codec_context->codec_id == AV_CODEC_ID_H264) {
            // Removed because it causes stutter in games for some people
            //av_dict_set_int(&options, "quality", 5, 0); // quality preset
        } else if(codec_context->codec_id == AV_CODEC_ID_AV1) {
            av_dict_set(&options, "tier", "main", 0);
        } else if(codec_context->codec_id == AV_CODEC_ID_HEVC) {
            if(hdr)
                av_dict_set(&options, "sei", "hdr", 0);
        }

        // TODO: vp8/vp9 10bit
    }

    if(codec_context->codec_id == AV_CODEC_ID_H264) {
        av_dict_set(&options, "coder", "cabac", 0); // TODO: cavlc is faster than cabac but worse compression. Which to use?
    }

    av_dict_set(&options, "strict", "experimental", 0);

    if(arg_parser.ffmpeg_video_opts)
        av_dict_parse_string(&options, arg_parser.ffmpeg_video_opts, "=", ";", 0);

    int ret = avcodec_open2(codec_context, codec_context->codec, &options);
    if (ret < 0) {
        fprintf(stderr, "gsr error: Could not open video codec: %s\n", av_error_to_string(ret));
        _exit(1);
    }
}

static const int save_replay_seconds_full = -1;

static sig_atomic_t running = 1;
static sig_atomic_t toggle_pause = 0;
static sig_atomic_t toggle_replay_recording = 0;
static sig_atomic_t save_replay_seconds = 0;

static void stop_handler(int) {
    running = 0;
}

static void toggle_pause_handler(int) {
    toggle_pause = 1;
}

static void toggle_replay_recording_handler(int) {
    toggle_replay_recording = 1;
}

static void save_replay_handler(int) {
    save_replay_seconds = save_replay_seconds_full;
}

static void save_replay_10_seconds_handler(int) {
    save_replay_seconds = 10;
}

static void save_replay_30_seconds_handler(int) {
    save_replay_seconds = 30;
}

static void save_replay_1_minute_handler(int) {
    save_replay_seconds = 60;
}

static void save_replay_5_minutes_handler(int) {
    save_replay_seconds = 60*5;
}

static void save_replay_10_minutes_handler(int) {
    save_replay_seconds = 60*10;
}

static void save_replay_30_minutes_handler(int) {
    save_replay_seconds = 60*30;
}

static std::string get_date_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
    return str;
}

static std::string get_date_only_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d", t);
    return str;
}

static std::string get_time_only_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%H-%M-%S", t);
    return str;
}

static AVStream* create_stream(AVFormatContext *av_format_context, AVCodecContext *codec_context) {
    AVStream *stream = avformat_new_stream(av_format_context, nullptr);
    if (!stream) {
        fprintf(stderr, "gsr error: Could not allocate stream\n");
        _exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    stream->time_base = codec_context->time_base;
    stream->avg_frame_rate = codec_context->framerate;
    //stream->r_frame_rate = codec_context->framerate;
    return stream;
}

static void run_recording_saved_script_async(const char *script_file, const char *video_file, const char *type) {
    char script_file_full[PATH_MAX];
    script_file_full[0] = '\0';
    if(!realpath(script_file, script_file_full)) {
        fprintf(stderr, "gsr error: script file not found: %s\n", script_file);
        return;
    }

    const char *args[7];
    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;

    if(inside_flatpak) {
        args[0] = "flatpak-spawn";
        args[1] = "--host";
        args[2] = "--";
        args[3] = script_file_full;
        args[4] = video_file;
        args[5] = type;
        args[6] = NULL;
    } else {
        args[0] = script_file_full;
        args[1] = video_file;
        args[2] = type;
        args[3] = NULL;
    }

    pid_t pid = fork();
    if(pid == -1) {
        perror(script_file_full);
        return;
    } else if(pid == 0) { // child
        setsid();
        signal(SIGHUP, SIG_IGN);

        pid_t second_child = fork();
        if(second_child == 0) { // child
            execvp(args[0], (char* const*)args);
            perror(script_file_full);
            _exit(127);
        } else if(second_child != -1) { // parent
            _exit(0);
        }
    } else { // parent
        waitpid(pid, NULL, 0);
    }
}

static double audio_codec_get_desired_delay(gsr_audio_codec audio_codec, int fps) {
    const double fps_inv = 1.0 / (double)fps;
    const double base = 0.01 + 1.0/165.0;
    switch(audio_codec) {
        case GSR_AUDIO_CODEC_OPUS:
            return std::max(0.0, base - fps_inv);
        case GSR_AUDIO_CODEC_AAC:
            return std::max(0.0, (base + 0.008) * 2.0 - fps_inv);
        case GSR_AUDIO_CODEC_FLAC:
            // TODO: Test
            return std::max(0.0, base - fps_inv);
    }
    assert(false);
    return std::max(0.0, base - fps_inv);
}

struct AudioDeviceData {
    SoundDevice sound_device;
    AudioInput audio_input;
    AVFilterContext *src_filter_ctx = nullptr;
    AVFrame *frame = nullptr;
    std::thread thread; // TODO: Instead of having a thread for each track, have one thread for all threads and read the data with non-blocking read
};

// TODO: Cleanup
struct AudioTrack {
    std::string name;
    AVCodecContext *codec_context = nullptr;

    std::vector<AudioDeviceData> audio_devices;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    int stream_index = 0;
    int64_t pts = 0;
};

static bool add_hdr_metadata_to_video_stream(gsr_capture *cap, AVStream *video_stream) {
    size_t light_metadata_size = 0;
    size_t mastering_display_metadata_size = 0;
    AVContentLightMetadata *light_metadata = av_content_light_metadata_alloc(&light_metadata_size);
    #if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(59, 37, 100)
    AVMasteringDisplayMetadata *mastering_display_metadata = av_mastering_display_metadata_alloc();
    mastering_display_metadata_size = sizeof(*mastering_display_metadata);
    #else
    AVMasteringDisplayMetadata *mastering_display_metadata = av_mastering_display_metadata_alloc_size(&mastering_display_metadata_size);
    #endif

    if(!light_metadata || !mastering_display_metadata) {
        if(light_metadata)
            av_freep(&light_metadata);

        if(mastering_display_metadata)
            av_freep(&mastering_display_metadata);

        return false;
    }

    if(!gsr_capture_set_hdr_metadata(cap, mastering_display_metadata, light_metadata)) {
        av_freep(&light_metadata);
        av_freep(&mastering_display_metadata);
        return false;
    }

    // TODO: More error checking

    #if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
    const bool content_light_level_added = av_stream_add_side_data(video_stream, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, (uint8_t*)light_metadata, light_metadata_size) == 0;
    #else
    const bool content_light_level_added = av_packet_side_data_add(&video_stream->codecpar->coded_side_data, &video_stream->codecpar->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, light_metadata, light_metadata_size, 0) != NULL;
    #endif

    #if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
    const bool mastering_display_metadata_added = av_stream_add_side_data(video_stream, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, (uint8_t*)mastering_display_metadata, mastering_display_metadata_size) == 0;
    #else
    const bool mastering_display_metadata_added = av_packet_side_data_add(&video_stream->codecpar->coded_side_data, &video_stream->codecpar->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, mastering_display_metadata, mastering_display_metadata_size, 0) != NULL;
    #endif

    if(!content_light_level_added)
        av_freep(&light_metadata);

    if(!mastering_display_metadata_added)
        av_freep(&mastering_display_metadata);

    // Return true even on failure because we dont want to retry adding hdr metadata on failure
    return true;
}

static void set_format_context_options(AVFormatContext *av_format_context) {
    if(LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 6, 101)) {
        bool hybrid_fragmented_available = false;
        av_opt_set(av_format_context->priv_data, "use_editlist", "1", 0);
        const AVOption *opt = av_opt_find(av_format_context->priv_data, "movflags", NULL, 0, 0);
        if (opt && opt->unit) {
            const AVOption *flag = av_opt_find(av_format_context->priv_data, "hybrid_fragmented", opt->unit, 0, 0);
            if (flag) {
                hybrid_fragmented_available = true;
                av_opt_set(av_format_context->priv_data, "movflags", "+hybrid_fragmented", 0);
            }
        }

        if(!hybrid_fragmented_available)
            fprintf(stderr, "gsr warning: hybrid_fragmented movflags is not available in your FFmpeg version. If you experience stutter in the mp4 file then update your FFmpeg to at least version 8 or record to a mkv file\n");
    } else {
        fprintf(stderr, "gsr warning: your FFmpeg version is known to be buggy (it doesn't have working hybrid_fragmented movflags). If you experience stutter in the mp4 file then update your FFmpeg to at least version 8 or record to a mkv file\n");
    }
}

struct RecordingStartAudio {
    const AudioTrack *audio_track;
    AVStream *stream;
};

struct RecordingStartResult {
    AVFormatContext *av_format_context = nullptr;
    AVStream *video_stream = nullptr;
    std::vector<RecordingStartAudio> audio_inputs;
};

typedef enum {
    VVEC2I_TYPE_PIXELS,
    VVEC2I_TYPE_SCALAR
} vvec2i_type;

typedef struct {
    int x, y;
    vvec2i_type x_type;
    vvec2i_type y_type;
} vvec2i;

struct CaptureSource {
    std::string name;
    CaptureSourceType type = GSR_CAPTURE_SOURCE_TYPE_WINDOW;
    gsr_capture_alignment halign = GSR_CAPTURE_ALIGN_CENTER;
    gsr_capture_alignment valign = GSR_CAPTURE_ALIGN_CENTER;
    gsr_capture_v4l2_pixfmt v4l2_pixfmt = GSR_CAPTURE_V4L2_PIXFMT_AUTO;
    uint32_t flip = GSR_FLIP_NONE;
    vvec2i pos = {0, 0, VVEC2I_TYPE_PIXELS, VVEC2I_TYPE_PIXELS};
    vvec2i size = {100, 100, VVEC2I_TYPE_SCALAR, VVEC2I_TYPE_SCALAR};
    vec2i region_pos = {0, 0};
    vec2i region_size = {0, 0};
    bool region_set = false;
    int64_t window_id = 0;
    int camera_fps = 0;
    vec2i camera_resolution = {0, 0};
};

struct VideoSource {
    gsr_capture *capture;
    gsr_capture_metadata metadata;
    CaptureSource *capture_source;
};

static RecordingStartResult start_recording_create_streams(const char *filename, const args_parser &arg_parser, AVCodecContext *video_codec_context, const std::vector<AudioTrack> &audio_tracks, bool hdr, std::vector<VideoSource> &video_sources) {
    AVFormatContext *av_format_context;
    avformat_alloc_output_context2(&av_format_context, nullptr, arg_parser.container_format, filename);
    set_format_context_options(av_format_context);

    AVStream *video_stream = create_stream(av_format_context, video_codec_context);
    avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

    RecordingStartResult result;
    result.audio_inputs.reserve(audio_tracks.size());

    for(const AudioTrack &audio_track : audio_tracks) {
        AVStream *audio_stream = create_stream(av_format_context, audio_track.codec_context);
        if(!audio_track.name.empty())
            av_dict_set(&audio_stream->metadata, "title", audio_track.name.c_str(), 0);
        avcodec_parameters_from_context(audio_stream->codecpar, audio_track.codec_context);
        result.audio_inputs.push_back({&audio_track, audio_stream});
    }

    const int open_ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
    if(open_ret < 0) {
        fprintf(stderr, "gsr error: start: could not open '%s': %s\n", filename, av_error_to_string(open_ret));
        return result;
    }

    AVDictionary *options = nullptr;
    av_dict_set(&options, "strict", "experimental", 0);

    if(arg_parser.ffmpeg_opts)
        av_dict_parse_string(&options, arg_parser.ffmpeg_opts, "=", ";", 0);

    const int header_write_ret = avformat_write_header(av_format_context, &options);
    av_dict_free(&options);
    if(header_write_ret < 0) {
        fprintf(stderr, "gsr error: start: error occurred when writing header to output file: %s\n", av_error_to_string(header_write_ret));
        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);
        return result;
    }

    for(VideoSource &video_source : video_sources) {
        if(hdr && add_hdr_metadata_to_video_stream(video_source.capture, video_stream))
            break;
    }

    result.av_format_context = av_format_context;
    result.video_stream = video_stream;
    return result;
}

static bool stop_recording_close_streams(AVFormatContext *av_format_context) {
    bool trailer_written = true;
    if(av_write_trailer(av_format_context) != 0) {
        //fprintf(stderr, "gsr error: end: failed to write trailer\n");
        //trailer_written = false;
    }

    const bool closed = avio_close(av_format_context->pb) == 0;
    avformat_free_context(av_format_context);
    return trailer_written && closed;
}

static std::future<void> save_replay_thread;
static std::string save_replay_output_filepath;

static std::string create_new_recording_filepath_from_timestamp(std::string directory, const char *filename_prefix, const std::string &file_extension, bool date_folders) {
    std::string output_filepath;
    if(date_folders) {
        std::string output_folder = directory + '/' + get_date_only_str();
        if(create_directory_recursive(&output_folder[0]) != 0)
            fprintf(stderr, "gsr error: failed to create directory: %s\n", output_folder.c_str());
        output_filepath = output_folder + "/" + filename_prefix + "_" + get_time_only_str() + "." + file_extension;
    } else {
        if(create_directory_recursive(&directory[0]) != 0)
            fprintf(stderr, "gsr error: failed to create directory: %s\n", directory.c_str());
        output_filepath = directory + "/" + filename_prefix + "_" + get_date_str() + "." + file_extension;
    }
    return output_filepath;
}

static RecordingStartAudio* get_recording_start_item_by_stream_index(RecordingStartResult &result, int stream_index) {
    for(auto &audio_input : result.audio_inputs) {
        if(audio_input.stream->index == stream_index)
            return &audio_input;
    }
    return nullptr;
}

struct AudioPtsOffset {
    int64_t pts_offset = 0;
    int stream_index = 0;
};

static void save_replay_async(AVCodecContext *video_codec_context, int video_stream_index, const std::vector<AudioTrack> &audio_tracks, gsr_encoder *encoder, const args_parser &arg_parser, const std::string &file_extension, bool date_folders, bool hdr, std::vector<VideoSource> &video_sources, int current_save_replay_seconds) {
    if(save_replay_thread.valid())
        return;

    pthread_mutex_lock(&encoder->replay_mutex);
    gsr_replay_buffer *cloned_replay_buffer = gsr_replay_buffer_clone(encoder->replay_buffer);
    pthread_mutex_unlock(&encoder->replay_mutex);
    if(!cloned_replay_buffer) {
        // TODO: Return this error to mark the replay as failed
        fprintf(stderr, "gsr error: failed to save replay: failed to clone replay buffer\n");
        return;
    }

    const gsr_replay_buffer_iterator search_start_iterator = current_save_replay_seconds == save_replay_seconds_full ? gsr_replay_buffer_iterator{0, 0} : gsr_replay_buffer_find_packet_index_by_time_passed(cloned_replay_buffer, current_save_replay_seconds);
    const gsr_replay_buffer_iterator video_start_iterator = gsr_replay_buffer_find_keyframe(cloned_replay_buffer, search_start_iterator, video_stream_index, false);
    if(video_start_iterator.packet_index == (size_t)-1) {
        fprintf(stderr, "gsr error: failed to save replay: failed to find a video keyframe. perhaps replay was saved too fast, before anything has been recorded\n");
        pthread_mutex_lock(&encoder->replay_mutex);
        gsr_replay_buffer_destroy(cloned_replay_buffer);
        pthread_mutex_unlock(&encoder->replay_mutex);
        return;
    }

    const int64_t video_pts_offset = gsr_replay_buffer_iterator_get_packet(cloned_replay_buffer, video_start_iterator)->pts;

    std::vector<AudioPtsOffset> audio_pts_offsets;
    audio_pts_offsets.reserve(audio_tracks.size());
    for(const AudioTrack &audio_track : audio_tracks) {
        const gsr_replay_buffer_iterator audio_start_iterator = gsr_replay_buffer_find_keyframe(cloned_replay_buffer, video_start_iterator, audio_track.stream_index, false);
        const int64_t audio_pts_offset = audio_start_iterator.packet_index == (size_t)-1 ? 0 : gsr_replay_buffer_iterator_get_packet(cloned_replay_buffer, audio_start_iterator)->pts;
        audio_pts_offsets.push_back(AudioPtsOffset{audio_pts_offset, audio_track.stream_index});
    }

    std::string output_filepath = create_new_recording_filepath_from_timestamp(arg_parser.filename, "Replay", file_extension, date_folders);
    RecordingStartResult recording_start_result = start_recording_create_streams(output_filepath.c_str(), arg_parser, video_codec_context, audio_tracks, hdr, video_sources);
    if(!recording_start_result.av_format_context) {
        pthread_mutex_lock(&encoder->replay_mutex);
        gsr_replay_buffer_destroy(cloned_replay_buffer);
        pthread_mutex_unlock(&encoder->replay_mutex);
        return;
    }

    save_replay_output_filepath = std::move(output_filepath);

    save_replay_thread = std::async(std::launch::async, [video_stream_index, recording_start_result, video_start_iterator, video_pts_offset, audio_pts_offsets{std::move(audio_pts_offsets)}, video_codec_context, cloned_replay_buffer, encoder]() mutable {
        gsr_replay_buffer_iterator replay_iterator = video_start_iterator;
        for(;;) {
            AVPacket *replay_packet = gsr_replay_buffer_iterator_get_packet(cloned_replay_buffer, replay_iterator);
            uint8_t *replay_packet_data = NULL;
            if(replay_packet) {
                pthread_mutex_lock(&encoder->replay_mutex);
                replay_packet_data = gsr_replay_buffer_iterator_get_packet_data(cloned_replay_buffer, replay_iterator);
                pthread_mutex_unlock(&encoder->replay_mutex);
            }

            if(!replay_packet) {
                fprintf(stderr, "gsr error: save_replay_async: no replay packet\n");
                break;
            }

            if(!replay_packet->data && !replay_packet_data) {
                fprintf(stderr, "gsr error: save_replay_async: no replay packet data\n");
                break;
            }

            // TODO: Check if successful
            AVPacket av_packet;
            memset(&av_packet, 0, sizeof(av_packet));
            //av_packet_from_data(av_packet, replay_packet->data, replay_packet->size);
            av_packet.data = replay_packet->data ? replay_packet->data : replay_packet_data;
            av_packet.size = replay_packet->size;
            av_packet.stream_index = replay_packet->stream_index;
            av_packet.pts = replay_packet->pts;
            av_packet.dts = replay_packet->pts;
            av_packet.flags = replay_packet->flags;
            //av_packet.duration = replay_packet->duration;

            AVStream *stream = recording_start_result.video_stream;
            AVCodecContext *codec_context = video_codec_context;

            if(av_packet.stream_index == video_stream_index) {
                av_packet.pts -= video_pts_offset;
                av_packet.dts -= video_pts_offset;
            } else {
                RecordingStartAudio *recording_start_audio = get_recording_start_item_by_stream_index(recording_start_result, av_packet.stream_index);
                if(!recording_start_audio) {
                    fprintf(stderr, "gsr error: save_replay_async: failed to find audio stream by index: %d\n", av_packet.stream_index);
                    free(replay_packet_data);
                    continue;
                }

                const AudioTrack *audio_track = recording_start_audio->audio_track;
                stream = recording_start_audio->stream;
                codec_context = audio_track->codec_context;

                const AudioPtsOffset &audio_pts_offset = audio_pts_offsets[av_packet.stream_index - 1];
                assert(audio_pts_offset.stream_index == av_packet.stream_index);
                av_packet.pts -= audio_pts_offset.pts_offset;
                av_packet.dts -= audio_pts_offset.pts_offset;
            }

            //av_packet.stream_index = stream->index;
            av_packet_rescale_ts(&av_packet, codec_context->time_base, stream->time_base);

            const int ret = av_write_frame(recording_start_result.av_format_context, &av_packet);
            if(ret < 0)
                fprintf(stderr, "gsr error: Failed to write frame index %d to muxer, reason: %s (%d)\n", av_packet.stream_index, av_error_to_string(ret), ret);

            free(replay_packet_data);

            //av_packet_free(&av_packet);
            if(!gsr_replay_buffer_iterator_next(cloned_replay_buffer, &replay_iterator))
                break;
        }

        stop_recording_close_streams(recording_start_result.av_format_context);

        pthread_mutex_lock(&encoder->replay_mutex);
        gsr_replay_buffer_destroy(cloned_replay_buffer);
        pthread_mutex_unlock(&encoder->replay_mutex);
    });
}

static void split_string(const std::string &str, char delimiter, std::function<bool(const char*,size_t)> callback) {
    size_t index = 0;
    while(index < str.size()) {
        size_t end_index = str.find(delimiter, index);
        if(end_index == std::string::npos)
            end_index = str.size();

        if(!callback(&str[index], end_index - index))
            break;

        index = end_index + 1;
    }
}

static bool string_starts_with(const char *str, size_t str_size, const char *substr) {
    int len = strlen(substr);
    return (int)str_size >= len && memcmp(str, substr, len) == 0;
}

static bool string_starts_with(const std::string &str, const char *substr) {
    return string_starts_with(str.data(), str.size(), substr);
}

static bool string_ends_with(const char *str, const char *substr) {
    int str_len = strlen(str);
    int substr_len = strlen(substr);
    return str_len >= substr_len && memcmp(str + str_len - substr_len, substr, substr_len) == 0;
}

static const AudioDevice* get_audio_device_by_name(const std::vector<AudioDevice> &audio_devices, const char *name) {
    for(const auto &audio_device : audio_devices) {
        if(strcmp(audio_device.name.c_str(), name) == 0)
            return &audio_device;
    }
    return nullptr;
}

static MergedAudioInputs parse_audio_input_arg(const char *str) {
    MergedAudioInputs result;
    result.track_name = str;

    split_string(str, '|', [&](const char *sub, size_t size) {
        if(size == 0)
            return true;

        AudioInput audio_input;
        audio_input.name.assign(sub, size);

        if(string_starts_with(audio_input.name.c_str(), "app:")) {
            audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + 4);
            audio_input.type = AudioInputType::APPLICATION;
            audio_input.inverted = false;
            result.audio_inputs.push_back(std::move(audio_input));
            return true;
        } else if(string_starts_with(audio_input.name.c_str(), "app-inverse:")) {
            audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + 12);
            audio_input.type = AudioInputType::APPLICATION;
            audio_input.inverted = true;
            result.audio_inputs.push_back(std::move(audio_input));
            return true;
        } else if(string_starts_with(audio_input.name.c_str(), "device:")) {
            audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + 7);
            audio_input.type = AudioInputType::DEVICE;
            result.audio_inputs.push_back(std::move(audio_input));
            return true;
        } else {
            audio_input.type = AudioInputType::DEVICE;
            result.audio_inputs.push_back(std::move(audio_input));
            return true;
        }
    });

    return result;
}

static int init_filter_graph(AVCodecContext* audio_codec_context, AVFilterGraph** graph, AVFilterContext** sink, std::vector<AVFilterContext*>& src_filter_ctx, size_t num_sources) {
    char ch_layout[64];
    int err = 0;
    ch_layout[0] = '\0';

    // C89-style variable declaration to
    // avoid problems because of goto
    AVFilterGraph* filter_graph = nullptr;
    AVFilterContext* mix_ctx = nullptr;

    const AVFilter* mix_filter = nullptr;
    const AVFilter* abuffersink = nullptr;
    AVFilterContext* abuffersink_ctx = nullptr;
    char args[512] = { 0 };
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(7, 107, 100)
    bool normalize = false;
#endif

    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        fprintf(stderr, "Unable to create filter graph.\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    for(size_t i = 0; i < num_sources; ++i) {
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        if (!abuffer) {
            fprintf(stderr, "Could not find the abuffer filter.\n");
            err = AVERROR_FILTER_NOT_FOUND;
            goto fail;
        }

        AVFilterContext *abuffer_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, NULL);
        if (!abuffer_ctx) {
            fprintf(stderr, "Could not allocate the abuffer instance.\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }

        #if LIBAVCODEC_VERSION_MAJOR < 60
        av_get_channel_layout_string(ch_layout, sizeof(ch_layout), 0, AV_CH_LAYOUT_STEREO);
        #else
        av_channel_layout_describe(&audio_codec_context->ch_layout, ch_layout, sizeof(ch_layout));
        #endif
        av_opt_set    (abuffer_ctx, "channel_layout", ch_layout,                                               AV_OPT_SEARCH_CHILDREN);
        av_opt_set    (abuffer_ctx, "sample_fmt",     av_get_sample_fmt_name(audio_codec_context->sample_fmt), AV_OPT_SEARCH_CHILDREN);
        av_opt_set_q  (abuffer_ctx, "time_base",      audio_codec_context->time_base,                          AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(abuffer_ctx, "sample_rate",    audio_codec_context->sample_rate,                        AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(abuffer_ctx, "bit_rate",       audio_codec_context->bit_rate,                           AV_OPT_SEARCH_CHILDREN);

        err = avfilter_init_str(abuffer_ctx, NULL);
        if (err < 0) {
            fprintf(stderr, "Could not initialize the abuffer filter.\n");
            goto fail;
        }

        src_filter_ctx.push_back(abuffer_ctx);
    }

    mix_filter = avfilter_get_by_name("amix");
    if (!mix_filter) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
        err = AVERROR_FILTER_NOT_FOUND;
        goto fail;
    }

#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(7, 107, 100)
    snprintf(args, sizeof(args), "inputs=%d:normalize=%s", (int)num_sources, normalize ? "true" : "false");
#else
    snprintf(args, sizeof(args), "inputs=%d", (int)num_sources);
    fprintf(stderr, "gsr warning: your ffmpeg version doesn't support disabling normalizing of mixed audio. Volume might be lower than expected\n");
#endif

    err = avfilter_graph_create_filter(&mix_ctx, mix_filter, "amix", args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter\n");
        goto fail;
    }

    abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        fprintf(stderr, "Could not find the abuffersink filter.\n");
        err = AVERROR_FILTER_NOT_FOUND;
        goto fail;
    }

    abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
    if (!abuffersink_ctx) {
        fprintf(stderr, "Could not allocate the abuffersink instance.\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = avfilter_init_str(abuffersink_ctx, NULL);
    if (err < 0) {
        fprintf(stderr, "Could not initialize the abuffersink instance.\n");
        goto fail;
    }

    err = 0;
    for(size_t i = 0; i < src_filter_ctx.size(); ++i) {
        AVFilterContext *src_ctx = src_filter_ctx[i];
        if (err >= 0)
            err = avfilter_link(src_ctx, 0, mix_ctx, i);
    }
    if (err >= 0)
        err = avfilter_link(mix_ctx, 0, abuffersink_ctx, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
        goto fail;
    }

    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
        goto fail;
    }

    *graph = filter_graph;
    *sink = abuffersink_ctx;

    return 0;

fail:
    avfilter_graph_free(&filter_graph);
    src_filter_ctx.clear();  // possibly unnecessary?
    return err;
}

static gsr_video_encoder* create_video_encoder(gsr_egl *egl, const args_parser &arg_parser) {
    const gsr_color_depth color_depth = video_codec_to_bit_depth(arg_parser.video_codec);
    gsr_video_encoder *video_encoder = nullptr;

    if(arg_parser.video_encoder == GSR_VIDEO_ENCODER_HW_CPU) {
        gsr_video_encoder_software_params params;
        params.egl = egl;
        params.color_depth = color_depth;
        video_encoder = gsr_video_encoder_software_create(&params);
        return video_encoder;
    }

    if(video_codec_is_vulkan(arg_parser.video_codec)) {
        gsr_video_encoder_vulkan_params params;
        params.egl = egl;
        params.color_depth = color_depth;
        video_encoder = gsr_video_encoder_vulkan_create(&params);
        return video_encoder;
    }

    switch(egl->gpu_info.vendor) {
        case GSR_GPU_VENDOR_AMD:
        case GSR_GPU_VENDOR_INTEL:
        case GSR_GPU_VENDOR_BROADCOM: {
            gsr_video_encoder_vaapi_params params;
            params.egl = egl;
            params.color_depth = color_depth;
            video_encoder = gsr_video_encoder_vaapi_create(&params);
            break;
        }
        case GSR_GPU_VENDOR_NVIDIA: {
            gsr_video_encoder_nvenc_params params;
            params.egl = egl;
            params.color_depth = color_depth;
            video_encoder = gsr_video_encoder_nvenc_create(&params);
            break;
        }
    }

    return video_encoder;
}

static bool get_supported_video_codecs(gsr_egl *egl, gsr_video_codec video_codec, bool use_software_video_encoder, bool cleanup, gsr_supported_video_codecs *video_codecs) {
    memset(video_codecs, 0, sizeof(*video_codecs));

    if(use_software_video_encoder) {
        video_codecs->h264.supported = avcodec_find_encoder_by_name("libx264");
        video_codecs->h264.max_resolution = {4096, 2304};
        return true;
    }

    if(video_codec_is_vulkan(video_codec))
        return gsr_get_supported_video_codecs_vulkan(video_codecs, egl->card_path, &egl->vulkan_device_index, cleanup);

    switch(egl->gpu_info.vendor) {
        case GSR_GPU_VENDOR_AMD:
        case GSR_GPU_VENDOR_INTEL:
        case GSR_GPU_VENDOR_BROADCOM:
            return gsr_get_supported_video_codecs_vaapi(video_codecs, egl->card_path, cleanup);
        case GSR_GPU_VENDOR_NVIDIA:
            return gsr_get_supported_video_codecs_nvenc(video_codecs, cleanup);
    }

    return false;
}

static void xwayland_check_callback(const gsr_monitor *monitor, void *userdata) {
    bool *xwayland_found = (bool*)userdata;
    if(monitor->name_len >= 8 && strncmp(monitor->name, "XWAYLAND", 8) == 0)
        *xwayland_found = true;
    else if(memmem(monitor->name, monitor->name_len, "X11", 3))
        *xwayland_found = true;
}

static bool is_xwayland(Display *display) {
    int opcode, event, error;
    if(XQueryExtension(display, "XWAYLAND", &opcode, &event, &error))
        return true;

    bool xwayland_found = false;
    for_each_active_monitor_output_x11_not_cached(display, xwayland_check_callback, &xwayland_found);
    return xwayland_found;
}

static bool is_using_prime_run() {
    const char *prime_render_offload = getenv("__NV_PRIME_RENDER_OFFLOAD");
    return (prime_render_offload && strcmp(prime_render_offload, "1") == 0) || getenv("DRI_PRIME");
}

static void disable_prime_run() {
    unsetenv("__NV_PRIME_RENDER_OFFLOAD");
    unsetenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER");
    unsetenv("__GLX_VENDOR_LIBRARY_NAME");
    unsetenv("__VK_LAYER_NV_optimus");
    unsetenv("DRI_PRIME");
}

static gsr_window* gsr_window_create(Display *display, bool wayland) {
    if(wayland)
        return gsr_window_wayland_create();
    else
        return gsr_window_x11_create(display);
}

static void list_system_info(bool wayland) {
    printf("display_server|%s\n", wayland ? "wayland" : "x11");
    bool supports_app_audio = false;
#ifdef GSR_APP_AUDIO
    supports_app_audio = pulseaudio_server_is_pipewire();
    if(supports_app_audio) {
        gsr_pipewire_audio audio;
        if(gsr_pipewire_audio_init(&audio))
            gsr_pipewire_audio_deinit(&audio);
        else
            supports_app_audio = false;
    }
#endif
    printf("supports_app_audio|%s\n", supports_app_audio ? "yes" : "no");
}

static void list_gpu_info(gsr_egl *egl) {
    switch(egl->gpu_info.vendor) {
        case GSR_GPU_VENDOR_AMD:
            printf("vendor|amd\n");
            break;
        case GSR_GPU_VENDOR_INTEL:
            printf("vendor|intel\n");
            break;
        case GSR_GPU_VENDOR_NVIDIA:
            printf("vendor|nvidia\n");
            break;
        case GSR_GPU_VENDOR_BROADCOM:
            printf("vendor|broadcom\n");
            break;
    }
    printf("card_path|%s\n", egl->card_path);
}

static const AVCodec* get_ffmpeg_video_codec(gsr_video_codec video_codec, gsr_gpu_vendor vendor) {
    switch(video_codec) {
        case GSR_VIDEO_CODEC_H264:
            return avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "h264_nvenc" : "h264_vaapi");
        case GSR_VIDEO_CODEC_HEVC:
        case GSR_VIDEO_CODEC_HEVC_HDR:
        case GSR_VIDEO_CODEC_HEVC_10BIT:
            return avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "hevc_nvenc" : "hevc_vaapi");
        case GSR_VIDEO_CODEC_AV1:
        case GSR_VIDEO_CODEC_AV1_HDR:
        case GSR_VIDEO_CODEC_AV1_10BIT:
            return avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "av1_nvenc" : "av1_vaapi");
        case GSR_VIDEO_CODEC_VP8:
            return avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "vp8_nvenc" : "vp8_vaapi");
        case GSR_VIDEO_CODEC_VP9:
            return avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "vp9_nvenc" : "vp9_vaapi");
        case GSR_VIDEO_CODEC_H264_VULKAN:
            return avcodec_find_encoder_by_name("h264_vulkan");
        case GSR_VIDEO_CODEC_HEVC_VULKAN:
        case GSR_VIDEO_CODEC_HEVC_HDR_VULKAN:
        case GSR_VIDEO_CODEC_HEVC_10BIT_VULKAN:
            return avcodec_find_encoder_by_name("hevc_vulkan");
        case GSR_VIDEO_CODEC_AV1_VULKAN:
        case GSR_VIDEO_CODEC_AV1_HDR_VULKAN:
        case GSR_VIDEO_CODEC_AV1_10BIT_VULKAN:
            return avcodec_find_encoder_by_name("av1_vulkan");
    }
    return nullptr;
}

static void set_supported_video_codecs_ffmpeg(gsr_supported_video_codecs *supported_video_codecs, gsr_supported_video_codecs *supported_video_codecs_vulkan, gsr_gpu_vendor vendor) {
    if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_H264, vendor)) {
        supported_video_codecs->h264.supported = false;
    }

    if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_HEVC, vendor)) {
        supported_video_codecs->hevc.supported = false;
        supported_video_codecs->hevc_hdr.supported = false;
        supported_video_codecs->hevc_10bit.supported = false;
    }

    if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_AV1, vendor)) {
        supported_video_codecs->av1.supported = false;
        supported_video_codecs->av1_hdr.supported = false;
        supported_video_codecs->av1_10bit.supported = false;
    }

    if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_VP8, vendor)) {
        supported_video_codecs->vp8.supported = false;
    }

    if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_VP9, vendor)) {
        supported_video_codecs->vp9.supported = false;
    }

    if(supported_video_codecs_vulkan) {
        if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_H264_VULKAN, vendor)) {
            supported_video_codecs_vulkan->h264.supported = false;
        }

        if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_HEVC_VULKAN, vendor)) {
            supported_video_codecs_vulkan->hevc.supported = false;
            supported_video_codecs_vulkan->hevc_hdr.supported = false;
            supported_video_codecs_vulkan->hevc_10bit.supported = false;
        }

        if(!get_ffmpeg_video_codec(GSR_VIDEO_CODEC_AV1_VULKAN, vendor)) {
            supported_video_codecs_vulkan->av1.supported = false;
            supported_video_codecs_vulkan->av1_hdr.supported = false;
            supported_video_codecs_vulkan->av1_10bit.supported = false;
        }
    }
}

static void list_supported_video_codecs(gsr_egl *egl, bool wayland) {
    // Dont clean it up on purpose to increase shutdown speed
    gsr_supported_video_codecs supported_video_codecs;
    get_supported_video_codecs(egl, GSR_VIDEO_CODEC_H264, false, false, &supported_video_codecs);

    gsr_supported_video_codecs supported_video_codecs_vulkan;
    get_supported_video_codecs(egl, GSR_VIDEO_CODEC_H264_VULKAN, false, false, &supported_video_codecs_vulkan);

    set_supported_video_codecs_ffmpeg(&supported_video_codecs, &supported_video_codecs_vulkan, egl->gpu_info.vendor);

    if(supported_video_codecs.h264.supported)
        puts("h264");
    if(avcodec_find_encoder_by_name("libx264"))
        puts("h264_software");
    if(supported_video_codecs.hevc.supported)
        puts("hevc");
    if(supported_video_codecs.hevc_hdr.supported && wayland)
        puts("hevc_hdr");
    if(supported_video_codecs.hevc_10bit.supported)
        puts("hevc_10bit");
    if(supported_video_codecs.av1.supported)
        puts("av1");
    if(supported_video_codecs.av1_hdr.supported && wayland)
        puts("av1_hdr");
    if(supported_video_codecs.av1_10bit.supported)
        puts("av1_10bit");
    if(supported_video_codecs.vp8.supported)
        puts("vp8");
    if(supported_video_codecs.vp9.supported)
        puts("vp9");
    if(supported_video_codecs_vulkan.h264.supported)
       puts("h264_vulkan");
    if(supported_video_codecs_vulkan.hevc.supported)
       puts("hevc_vulkan");
    if(supported_video_codecs_vulkan.hevc_hdr.supported && wayland)
        puts("hevc_hdr_vulkan");
    if(supported_video_codecs_vulkan.hevc_10bit.supported)
        puts("hevc_10bit_vulkan");
    if(supported_video_codecs_vulkan.av1.supported)
        puts("av1_vulkan");
    if(supported_video_codecs_vulkan.av1_hdr.supported && wayland)
        puts("av1_hdr_vulkan");
    if(supported_video_codecs_vulkan.av1_10bit.supported)
        puts("av1_10bit_vulkan");
}

static bool monitor_capture_use_drm(const gsr_window *window, gsr_gpu_vendor vendor) {
    return gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_WAYLAND || vendor != GSR_GPU_VENDOR_NVIDIA;
}

typedef struct {
    const gsr_window *window;
    int num_monitors;
} capture_options_callback;

static void output_monitor_info(const gsr_monitor *monitor, void *userdata) {
    capture_options_callback *options = (capture_options_callback*)userdata;
    if(gsr_window_get_display_server(options->window) == GSR_DISPLAY_SERVER_WAYLAND) {
        vec2i monitor_size = monitor->size;
        gsr_monitor_rotation monitor_rotation = GSR_MONITOR_ROT_0;
        vec2i monitor_position = {0, 0};
        drm_monitor_get_display_server_data(options->window, monitor, &monitor_rotation, &monitor_position);
        if(monitor_rotation == GSR_MONITOR_ROT_90 || monitor_rotation == GSR_MONITOR_ROT_270)
            std::swap(monitor_size.x, monitor_size.y);
        printf("%.*s|%dx%d\n", monitor->name_len, monitor->name, monitor_size.x, monitor_size.y);
    } else {
        printf("%.*s|%dx%d\n", monitor->name_len, monitor->name, monitor->size.x, monitor->size.y);
    }
    ++options->num_monitors;
}

static void camera_query_callback(const char *path, const gsr_capture_v4l2_supported_setup *setup, void *userdata) {
    (void)userdata;
    printf("%s|%ux%u@%uhz|%s\n", path, setup->resolution.width, setup->resolution.height, gsr_capture_v4l2_framerate_to_number(setup->framerate), gsr_capture_v4l2_pixfmt_to_string(setup->pixfmt));
}

// Returns the number of monitors found
static int list_monitors(const gsr_window *window, const char *card_path) {
    capture_options_callback options;
    options.window = window;
    options.num_monitors = 0;

    const bool is_x11 = gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_X11;
    const gsr_connection_type connection_type = is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_DRM;
    for_each_active_monitor_output(window, card_path, connection_type, output_monitor_info, &options);

    return options.num_monitors;
}

static void list_supported_capture_options(const gsr_window *window, const char *card_path, bool do_list_monitors) {
    const bool wayland = gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_WAYLAND;
    if(!wayland) {
        puts("window");
        puts("focused");
    }

    int num_monitors = 0;
    if(do_list_monitors)
        num_monitors = list_monitors(window, card_path);

    if(num_monitors > 0)
        puts("region");

    gsr_capture_v4l2_list_devices(camera_query_callback, NULL);

#ifdef GSR_PORTAL
    // Desktop portal capture on x11 doesn't seem to be hardware accelerated
    if(!wayland)
        return;

    gsr_dbus dbus;
    if(!gsr_dbus_init(&dbus, NULL))
        return;

    char *session_handle = NULL;
    if(gsr_dbus_screencast_create_session(&dbus, &session_handle) == 0)
        puts("portal");

    gsr_dbus_deinit(&dbus);
#endif
}

static void version_command(void *userdata) {
    (void)userdata;
    puts(GSR_VERSION);
    fflush(stdout);
    _exit(0);
}

struct WindowingSetup {
    Display *dpy;
    gsr_window *window;
    gsr_egl egl;
    bool list_monitors;
};

static WindowingSetup setup_windowing(bool setup_egl) {
    WindowingSetup setup;
    memset(&setup, 0, sizeof(setup));

    bool wayland = false;
    setup.dpy = XOpenDisplay(nullptr);
    if (!setup.dpy) {
        wayland = true;
        fprintf(stderr, "gsr warning: failed to connect to the X server. Assuming wayland is running without Xwayland\n");
    }

    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);

    if(!wayland)
        wayland = is_xwayland(setup.dpy);

    if(!wayland && is_using_prime_run()) {
        // Disable prime-run and similar options as it doesn't work, the monitor to capture has to be run on the same device.
        // This is fine on wayland since nvidia uses drm interface there and the monitor query checks the monitors connected
        // to the drm device.
        fprintf(stderr, "gsr warning: use of prime-run on X11 is not supported. Disabling prime-run\n");
        disable_prime_run();
    }

    setup.window = gsr_window_create(setup.dpy, wayland);
    if(!setup.window) {
        fprintf(stderr, "gsr error: failed to create window\n");
        _exit(1);
    }

    setup.list_monitors = true;

    if(setup_egl) {
        if(!gsr_egl_load(&setup.egl, setup.window, false, false)) {
            fprintf(stderr, "gsr error: failed to load opengl\n");
            _exit(22);
        }

        setup.egl.card_path[0] = '\0';
        if(monitor_capture_use_drm(setup.window, setup.egl.gpu_info.vendor)) {
            // TODO: Allow specifying another card, and in other places
            if(!gsr_get_valid_card_path(&setup.egl, setup.egl.card_path, true)) {
                fprintf(stderr, "gsr error: no /dev/dri/cardX device found. Make sure that you have at least one monitor connected\n");
                setup.list_monitors = false;
            }
        } else {
            gsr_get_valid_card_path(&setup.egl, setup.egl.card_path, false);
        }
    }

    return setup;
}

static void info_command(void *userdata) {
    (void)userdata;
    WindowingSetup windowing_setup = setup_windowing(true);
    const bool wayland = gsr_window_get_display_server(windowing_setup.window) == GSR_DISPLAY_SERVER_WAYLAND;

    av_log_set_level(AV_LOG_FATAL);

    puts("section=system_info");
    list_system_info(wayland);
    if(windowing_setup.egl.gpu_info.is_steam_deck)
        puts("is_steam_deck|yes");
    else
        puts("is_steam_deck|no");
    printf("gsr_version|%s\n", GSR_VERSION);
    puts("section=gpu_info");
    list_gpu_info(&windowing_setup.egl);
    puts("section=video_codecs");
    list_supported_video_codecs(&windowing_setup.egl, wayland);
    puts("section=image_formats");
    puts("jpeg");
    puts("png");
    puts("section=capture_options");
    list_supported_capture_options(windowing_setup.window, windowing_setup.egl.card_path, windowing_setup.list_monitors);

    fflush(stdout);

    // Not needed as this will just slow down shutdown
    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);
    //if(dpy)
    //    XCloseDisplay(dpy);

    _exit(0);
}

static void list_audio_devices_command(void *userdata) {
    (void)userdata;
    const AudioDevices audio_devices = get_pulseaudio_inputs();

    if(!audio_devices.default_output.empty())
        puts("default_output|Default output");

    if(!audio_devices.default_input.empty())
        puts("default_input|Default input");

    for(const auto &audio_input : audio_devices.audio_inputs) {
        printf("%s|%s\n", audio_input.name.c_str(), audio_input.description.c_str());
    }

    fflush(stdout);
    _exit(0);
}

static bool app_audio_query_callback(const char *app_name, void*) {
    puts(app_name);
    return true;
}

static void list_application_audio_command(void *userdata) {
    (void)userdata;
#ifdef GSR_APP_AUDIO
    if(pulseaudio_server_is_pipewire()) {
        gsr_pipewire_audio audio;
        if(gsr_pipewire_audio_init(&audio)) {
            gsr_pipewire_audio_for_each_app(&audio, app_audio_query_callback, NULL);
            gsr_pipewire_audio_deinit(&audio);
        }
    }
#endif

    fflush(stdout);
    _exit(0);
}

static void list_v4l2_devices(void *userdata) {
    (void)userdata;
    gsr_capture_v4l2_list_devices(camera_query_callback, NULL);

    fflush(stdout);
    _exit(0);
}

// |card_path| can be NULL. If not NULL then |vendor| has to be valid
static void list_capture_options_command(const char *card_path, void *userdata) {
    (void)userdata;
    WindowingSetup windowing_setup = setup_windowing(card_path != nullptr);

    if(card_path)
        list_supported_capture_options(windowing_setup.window, card_path, true);
    else
        list_supported_capture_options(windowing_setup.window, windowing_setup.egl.card_path, windowing_setup.list_monitors);

    fflush(stdout);

    // Not needed as this will just slow down shutdown
    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);
    //if(dpy)
    //    XCloseDisplay(dpy);

    _exit(0);
}

static void list_monitors_command(void *userdata) {
    (void)userdata;
    WindowingSetup windowing_setup = setup_windowing(true);

    if(windowing_setup.list_monitors)
        list_monitors(windowing_setup.window, windowing_setup.egl.card_path);

    fflush(stdout);

    // Not needed as this will just slow down shutdown
    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);
    //if(dpy)
    //    XCloseDisplay(dpy);

    _exit(0);
}

static std::string validate_monitor_get_valid(const gsr_egl *egl, const char* window) {
    const bool is_x11 = gsr_window_get_display_server(egl->window) == GSR_DISPLAY_SERVER_X11;
    const gsr_connection_type connection_type = is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_DRM;
    const bool capture_use_drm = monitor_capture_use_drm(egl->window, egl->gpu_info.vendor);

    std::string capture_source_result = window;
    if(strcmp(capture_source_result.c_str(), "screen") == 0) {
        FirstOutputCallback data;
        data.output_name = NULL;
        for_each_active_monitor_output(egl->window, egl->card_path, connection_type, get_first_output_callback, &data);

        if(data.output_name) {
            capture_source_result = data.output_name;
            free(data.output_name);
        } else {
            fprintf(stderr, "gsr error: no usable output found\n");
            _exit(51);
        }
    } else if(capture_use_drm || (strcmp(capture_source_result.c_str(), "screen-direct") != 0 && strcmp(capture_source_result.c_str(), "screen-direct-force") != 0)) {
        gsr_monitor gmon;
        if(!get_monitor_by_name(egl, connection_type, capture_source_result.c_str(), &gmon)) {
            fprintf(stderr, "gsr error: display \"%s\" not found, expected one of:\n", capture_source_result.c_str());
            fprintf(stderr, "  \"screen\"\n");
            if(!capture_use_drm)
                fprintf(stderr, "  \"screen-direct\"\n");

            MonitorOutputCallbackUserdata userdata;
            userdata.window = egl->window;
            for_each_active_monitor_output(egl->window, egl->card_path, connection_type, monitor_output_callback_print, &userdata);
            _exit(51);
        }
    }
    return capture_source_result;
}

static std::string get_monitor_by_region_center(const gsr_egl *egl, vec2i region_position, vec2i region_size, vec2i *monitor_pos, vec2i *monitor_size, double *monitor_scale_inverted) {
    const bool is_x11 = gsr_window_get_display_server(egl->window) == GSR_DISPLAY_SERVER_X11;
    const gsr_connection_type connection_type = is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_WAYLAND;

    MonitorByPositionCallback data;
    data.window = egl->window;
    data.position = { region_position.x + region_size.x / 2, region_position.y + region_size.y / 2 };
    data.output_name = NULL;
    data.monitor_pos = {0, 0};
    data.monitor_size = {0, 0};
    data.monitor_scale_inverted = 1.0;
    for_each_active_monitor_output(egl->window, egl->card_path, connection_type, get_monitor_by_position_callback, &data);

    std::string result;
    if(data.output_name) {
        result = data.output_name;
        free(data.output_name);
    }
    *monitor_pos = data.monitor_pos;
    *monitor_size = data.monitor_size;
    *monitor_scale_inverted = data.monitor_scale_inverted;
    return result;
}

static gsr_kms_client kms_client;
static bool kms_client_initialized = false;
static gsr_kms_response kms_response;

static gsr_cursor x11_cursor;
static Display *x11_cursor_display = NULL;

static gsr_capture* create_monitor_capture(const args_parser &arg_parser, gsr_egl *egl, const CaptureSource &capture_source, bool prefer_ximage) {
    if(gsr_window_get_display_server(egl->window) == GSR_DISPLAY_SERVER_X11 && prefer_ximage) {
        gsr_capture_ximage_params ximage_params;
        memset(&ximage_params, 0, sizeof(ximage_params));
        ximage_params.egl = egl;
        ximage_params.cursor = &x11_cursor;
        ximage_params.display_to_capture = capture_source.name.c_str();
        ximage_params.record_cursor = arg_parser.record_cursor;
        ximage_params.output_resolution = arg_parser.output_resolution;
        ximage_params.region_size = capture_source.region_size;
        ximage_params.region_position = capture_source.region_pos;
        return gsr_capture_ximage_create(&ximage_params);
    }

    if(monitor_capture_use_drm(egl->window, egl->gpu_info.vendor)) {
        if(!kms_client_initialized) {
            kms_client_initialized = true;
            const int kms_init_res = gsr_kms_client_init(&kms_client, egl->card_path);
            if(kms_init_res != 0)
                _exit(kms_init_res < 0 ? 1 : kms_init_res);
        }

        gsr_capture_kms_params kms_params;
        memset(&kms_params, 0, sizeof(kms_params));
        kms_params.egl = egl;
        kms_params.x11_cursor = &x11_cursor;
        kms_params.kms_response = &kms_response;
        kms_params.display_to_capture = capture_source.name.c_str();
        kms_params.record_cursor = arg_parser.record_cursor;
        kms_params.hdr = video_codec_is_hdr(arg_parser.video_codec);
        kms_params.fps = arg_parser.fps;
        kms_params.output_resolution = arg_parser.output_resolution;
        kms_params.region_size = capture_source.region_size;
        kms_params.region_position = capture_source.region_pos;
        return gsr_capture_kms_create(&kms_params);
    } else {
        const char *capture_source_real = capture_source.name.c_str();
        const bool direct_capture = strcmp(capture_source.name.c_str(), "screen-direct") == 0 || strcmp(capture_source.name.c_str(), "screen-direct-force") == 0;
        if(direct_capture) {
            capture_source_real = "screen";
            fprintf(stderr, "gsr warning: %s capture option is not recommended unless you use G-SYNC as Nvidia has driver issues that can cause your system or games to freeze/crash.\n", capture_source.name.c_str());
        }

        gsr_capture_nvfbc_params nvfbc_params;
        memset(&nvfbc_params, 0, sizeof(nvfbc_params));
        nvfbc_params.egl = egl;
        nvfbc_params.display_to_capture = capture_source_real;
        nvfbc_params.fps = arg_parser.fps;
        nvfbc_params.direct_capture = direct_capture;
        nvfbc_params.record_cursor = arg_parser.record_cursor;
        nvfbc_params.output_resolution = arg_parser.output_resolution;
        nvfbc_params.region_size = capture_source.region_size;
        nvfbc_params.region_position = capture_source.region_pos;
        return gsr_capture_nvfbc_create(&nvfbc_params);
    }
}

static void monitor_output_callback_print_region(const gsr_monitor *monitor, void *userdata) {
    const vec2i monitor_position = monitor->logical_pos;
    const vec2i monitor_size = monitor->logical_size;
    fprintf(stderr, "  \"%.*s\"    (%dx%d+%d+%d)\n", monitor->name_len, monitor->name, monitor_size.x, monitor_size.y, monitor_position.x, monitor_position.y);
}

static std::string region_get_data(gsr_egl *egl, vec2i *region_size, vec2i *region_position) {
    vec2i monitor_pos = {0, 0};
    vec2i monitor_size = {0, 0};
    double monitor_scale_inverted = 1.0;
    std::string window = get_monitor_by_region_center(egl, *region_position, *region_size, &monitor_pos, &monitor_size, &monitor_scale_inverted);
    if(window.empty()) {
        const bool is_x11 = gsr_window_get_display_server(egl->window) == GSR_DISPLAY_SERVER_X11;
        const gsr_connection_type connection_type = is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_WAYLAND;
        fprintf(stderr, "gsr error: the region %dx%d+%d+%d doesn't match any monitor. Available monitors and their regions:\n", region_size->x, region_size->y, region_position->x, region_position->y);

        MonitorOutputCallbackUserdata userdata;
        userdata.window = egl->window;
        for_each_active_monitor_output(egl->window, egl->card_path, connection_type, monitor_output_callback_print_region, &userdata);
        _exit(51);
    }

    // Capture whole monitor when region size is set to 0x0
    if(region_size->x == 0 && region_size->y == 0) {
        region_position->x = 0;
        region_position->y = 0;
    } else {
        region_position->x -= monitor_pos.x;
        region_position->y -= monitor_pos.y;
        // Match drm plane coordinate space (1x scaling) to wayland coordinate space (which may have scaling set by user)
        region_position->x *= monitor_scale_inverted;
        region_position->y *= monitor_scale_inverted;

        region_size->x *= monitor_scale_inverted;
        region_size->y *= monitor_scale_inverted;
    }
    return window;
}

static gsr_capture* create_capture_impl(const args_parser &arg_parser, gsr_egl *egl, CaptureSource &capture_source, bool prefer_ximage) {
    bool follow_focused = false;
    const bool wayland = gsr_window_get_display_server(egl->window) == GSR_DISPLAY_SERVER_WAYLAND;

    gsr_capture *capture = nullptr;
    if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW) {
        if(wayland) {
            fprintf(stderr, "gsr error: GPU Screen Recorder window capture only works in a pure X11 session. Xwayland is not supported. You can record a monitor instead on wayland\n");
            _exit(2);
        }

        if(arg_parser.output_resolution.x <= 0 || arg_parser.output_resolution.y <= 0) {
            fprintf(stderr, "gsr error: invalid value for option -s '%dx%d' when using -w focused option. expected width and height to be greater than 0\n", arg_parser.output_resolution.x, arg_parser.output_resolution.y);
            args_parser_print_usage();
            _exit(1);
        }

        follow_focused = true;
    } else if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_PORTAL) {
#ifdef GSR_PORTAL
        // Desktop portal capture on x11 doesn't seem to be hardware accelerated
        if(!wayland) {
            fprintf(stderr, "gsr error: desktop portal capture is not supported on X11\n");
            _exit(1);
        }

        gsr_capture_portal_params portal_params;
        memset(&portal_params, 0, sizeof(portal_params));
        portal_params.egl = egl;
        portal_params.record_cursor = arg_parser.record_cursor;
        portal_params.restore_portal_session = arg_parser.restore_portal_session;
        portal_params.portal_session_token_filepath = arg_parser.portal_session_token_filepath;
        portal_params.output_resolution = arg_parser.output_resolution;
        capture = gsr_capture_portal_create(&portal_params);
        if(!capture)
            _exit(1);
#else
        fprintf(stderr, "gsr error: option '-w portal' used but GPU Screen Recorder was compiled without desktop portal support. Please recompile GPU Screen recorder with the -Dportal=true option\n");
        _exit(2);
#endif
    } else if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_REGION) {
        capture_source.name = region_get_data(egl, &capture_source.region_size, &capture_source.region_pos);
        capture = create_monitor_capture(arg_parser, egl, capture_source, prefer_ximage);
        if(!capture)
            _exit(1);
    } else if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_MONITOR) {
        capture_source.name = validate_monitor_get_valid(egl, capture_source.name.c_str());
        capture = create_monitor_capture(arg_parser, egl, capture_source, prefer_ximage);
        if(!capture)
            _exit(1);
    } else if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_V4L2) {
        gsr_capture_v4l2_params v4l2_params;
        memset(&v4l2_params, 0, sizeof(v4l2_params));
        v4l2_params.egl = egl;
        v4l2_params.output_resolution = arg_parser.output_resolution;
        v4l2_params.device_path = capture_source.name.c_str();
        v4l2_params.pixfmt = capture_source.v4l2_pixfmt;
        v4l2_params.camera_fps = capture_source.camera_fps;
        v4l2_params.camera_resolution.width = capture_source.camera_resolution.x;
        v4l2_params.camera_resolution.height = capture_source.camera_resolution.y;
        capture = gsr_capture_v4l2_create(&v4l2_params);
        if(!capture)
            _exit(1);
    } else {
        if(wayland) {
            fprintf(stderr, "gsr error: GPU Screen Recorder window capture only works in a pure X11 session. Xwayland is not supported. You can record a monitor instead on wayland or use -w portal option which supports window capture if your wayland compositor supports window capture\n");
            _exit(2);
        }
    }

    if(!capture) {
        gsr_capture_xcomposite_params xcomposite_params;
        memset(&xcomposite_params, 0, sizeof(xcomposite_params));
        xcomposite_params.egl = egl;
        xcomposite_params.cursor = &x11_cursor;
        xcomposite_params.window = capture_source.window_id;
        xcomposite_params.follow_focused = follow_focused;
        xcomposite_params.record_cursor = arg_parser.record_cursor;
        xcomposite_params.output_resolution = arg_parser.output_resolution;
        capture = gsr_capture_xcomposite_create(&xcomposite_params);
        if(!capture)
            _exit(1);
    }

    return capture;
}

static gsr_color_range image_format_to_color_range(gsr_image_format image_format, int image_quality) {
    switch(image_format) {
        case GSR_IMAGE_FORMAT_JPEG: return image_quality >= 91 ? GSR_COLOR_RANGE_FULL : GSR_COLOR_RANGE_LIMITED;
        case GSR_IMAGE_FORMAT_PNG:  return GSR_COLOR_RANGE_FULL;
    }
    assert(false);
    return GSR_COLOR_RANGE_FULL;
}

static int video_quality_to_image_quality_value(gsr_video_quality video_quality) {
    switch(video_quality) {
        case GSR_VIDEO_QUALITY_MEDIUM:
            return 75;
        case GSR_VIDEO_QUALITY_HIGH:
            return 85;
        case GSR_VIDEO_QUALITY_VERY_HIGH:
            return 91; // Quality above 90 makes the jpeg image encoder (stb_image_writer) use yuv444 instead of yuv420, which greatly improves small colored text quality on dark background
        case GSR_VIDEO_QUALITY_ULTRA:
            return 97;
    }
    assert(false);
    return 90;
}

static bool any_video_sources_uses_external_image(std::vector<VideoSource> &video_sources) {
    for(VideoSource &video_source : video_sources) {
        if(gsr_capture_uses_external_image(video_source.capture))
            return true;
    }
    return false;
}

static std::vector<VideoSource> create_video_sources(const args_parser &arg_parser, gsr_egl *egl, bool prefer_ximage, std::vector<CaptureSource> &capture_sources, vec2i &video_size) {
    std::vector<VideoSource> video_sources;
    video_sources.reserve(capture_sources.size());

    for(CaptureSource &capture_source : capture_sources) {
        gsr_capture_metadata capture_metadata;
        memset(&capture_metadata, 0, sizeof(capture_metadata));
        capture_metadata.fps = arg_parser.fps;
        capture_metadata.halign = capture_source.halign;
        capture_metadata.valign = capture_source.valign;
        capture_metadata.flip = (gsr_flip)capture_source.flip;
        video_sources.push_back(VideoSource{create_capture_impl(arg_parser, egl, capture_source, prefer_ximage), capture_metadata, &capture_source});
    }

    for(VideoSource &video_source : video_sources) {
        int capture_result = gsr_capture_start(video_source.capture, &video_source.metadata);
        if(capture_result != 0) {
            fprintf(stderr, "gsr error: gsr_capture_start failed\n");
            _exit(capture_result);
        }
    }

    vec2i start_pos = {99999, 99999};
    vec2i end_pos = {-99999, -99999};
    for(const VideoSource &video_source : video_sources) {
        // TODO: Skip scalar positions for now, but this should be handled in a better way.
        // Maybe handle scalars at the next loop by multiplying video size by the scalar.
        if(video_source.capture_source->pos.x_type == VVEC2I_TYPE_SCALAR || video_source.capture_source->pos.y_type == VVEC2I_TYPE_SCALAR
            || (video_source.capture_source->size.x_type == VVEC2I_TYPE_SCALAR && video_source.capture_source->size.x != 100)
            || (video_source.capture_source->size.y_type == VVEC2I_TYPE_SCALAR && video_source.capture_source->size.y != 100))
        {
            continue;
        }
        const vec2i video_source_start_pos = {video_source.capture_source->pos.x, video_source.capture_source->pos.y};
        const vec2i video_source_end_pos = {video_source_start_pos.x + video_source.metadata.video_size.x, video_source_start_pos.y + video_source.metadata.video_size.y};

        start_pos.x = std::min(start_pos.x, video_source_start_pos.x);
        start_pos.y = std::min(start_pos.y, video_source_start_pos.y);

        end_pos.x = std::max(end_pos.x, video_source_end_pos.x);
        end_pos.y = std::max(end_pos.y, video_source_end_pos.y);
    }

    video_size.x = std::max(0, end_pos.x - start_pos.x);
    video_size.y = std::max(0, end_pos.y - start_pos.y);

    for(VideoSource &video_source : video_sources) {
        video_source.metadata.video_size = video_size;
    }

    return video_sources;
}

static void video_sources_update_with_real_video_size(std::vector<CaptureSource> &capture_sources, std::vector<VideoSource> &video_sources, vec2i video_size) {
    assert(capture_sources.size() == video_sources.size());
    for(size_t i = 0; i < capture_sources.size(); ++i) {
        CaptureSource &capture_source = capture_sources[i];
        VideoSource &video_source = video_sources[i];

        video_source.metadata.recording_size = video_source.metadata.video_size;
        // TODO: What if this updated resolution is above max resolution?
        video_source.metadata.video_size = video_size;

        if(capture_source.pos.x != 0 || capture_source.pos.y != 0) {
            video_source.metadata.position.x = capture_source.pos.x;
            video_source.metadata.position.y = capture_source.pos.y;

            if(capture_source.pos.x_type == VVEC2I_TYPE_SCALAR)
                video_source.metadata.position.x = video_source.metadata.video_size.x * ((double)video_source.metadata.position.x / 100.0);

            if(capture_source.pos.y_type == VVEC2I_TYPE_SCALAR)
                video_source.metadata.position.y = video_source.metadata.video_size.y * ((double)video_source.metadata.position.y / 100.0);
        }

        if(capture_source.size.x != 0 || capture_source.size.y != 0) {
            video_source.metadata.recording_size.x = capture_source.size.x;
            video_source.metadata.recording_size.y = capture_source.size.y;

            if(capture_source.size.x_type == VVEC2I_TYPE_SCALAR)
                video_source.metadata.recording_size.x = video_source.metadata.video_size.x * ((double)video_source.metadata.recording_size.x / 100.0);

            if(capture_source.size.y_type == VVEC2I_TYPE_SCALAR)
                video_source.metadata.recording_size.y = video_source.metadata.video_size.y * ((double)video_source.metadata.recording_size.y / 100.0);
        }
    }
}

static void gsr_capture_kms_cleanup_kms_fds() {
    for(int i = 0; i < kms_response.num_items; ++i) {
        for(int j = 0; j < kms_response.items[i].num_dma_bufs; ++j) {
            gsr_kms_response_dma_buf *dma_buf = &kms_response.items[i].dma_buf[j];
            if(dma_buf->fd > 0) {
                close(dma_buf->fd);
                dma_buf->fd = -1;
            }
        }
        kms_response.items[i].num_dma_bufs = 0;
    }
    kms_response.num_items = 0;
}

static void load_plugins(gsr_plugins *plugins, args_parser &arg_parser, gsr_egl *egl, vec2i video_size) {
    const Arg *plugin_arg = args_parser_get_arg(&arg_parser, "-p");
    assert(plugin_arg);

    if(plugin_arg->num_values > 0) {
        const gsr_color_depth color_depth = video_codec_to_bit_depth(arg_parser.video_codec);
        assert(color_depth == GSR_COLOR_DEPTH_8_BITS || color_depth == GSR_COLOR_DEPTH_10_BITS);

        const gsr_plugin_init_params plugin_init_params = {
            (unsigned int)video_size.x,
            (unsigned int)video_size.y,
            (unsigned int)arg_parser.fps,
            color_depth == GSR_COLOR_DEPTH_8_BITS ? GSR_PLUGIN_COLOR_DEPTH_8_BITS : GSR_PLUGIN_COLOR_DEPTH_10_BITS,
            egl->context_type == GSR_GL_CONTEXT_TYPE_GLX ? GSR_PLUGIN_GRAPHICS_API_GLX : GSR_PLUGIN_GRAPHICS_API_EGL_ES,
        };

        if(!gsr_plugins_init(plugins, plugin_init_params, egl))
            _exit(1);

        for(int i = 0; i < plugin_arg->num_values; ++i) {
            if(!gsr_plugins_load_plugin(plugins, plugin_arg->values[i]))
                _exit(1);
        }
    }
}

// TODO: 10-bit and hdr.
static void capture_image_to_file(args_parser &arg_parser, gsr_egl *egl, gsr_window *window, gsr_image_format image_format, std::vector<CaptureSource> &capture_sources) {
    const int image_quality = video_quality_to_image_quality_value(arg_parser.video_quality);
    const gsr_color_range color_range = image_format_to_color_range(image_format, image_quality);
    arg_parser.fps = 60; // We want to capture an image as soon as possible

    vec2i video_size = {0, 0};
    std::vector<VideoSource> video_sources = create_video_sources(arg_parser, egl, true, capture_sources, video_size);
    video_sources_update_with_real_video_size(capture_sources, video_sources, video_size);

    const Arg *plugin_arg = args_parser_get_arg(&arg_parser, "-p");
    assert(plugin_arg);

    gsr_plugins plugins;
    memset(&plugins, 0, sizeof(plugins));

    if(plugin_arg->num_values > 0)
        load_plugins(&plugins, arg_parser, egl, video_size);

    gsr_image_writer image_writer;
    if(!gsr_image_writer_init_opengl(&image_writer, egl, video_size.x, video_size.y)) {
        fprintf(stderr, "gsr error: capture_image_to_file: gsr_image_write_gl_init failed\n");
        _exit(1);
    }

    gsr_color_conversion_params color_conversion_params;
    memset(&color_conversion_params, 0, sizeof(color_conversion_params));
    color_conversion_params.color_range = color_range;
    color_conversion_params.egl = egl;
    color_conversion_params.load_external_image_shader = any_video_sources_uses_external_image(video_sources);

    color_conversion_params.destination_textures[0] = image_writer.texture;
    color_conversion_params.destination_textures_size[0] = video_size;
    color_conversion_params.num_destination_textures = 1;
    color_conversion_params.destination_color = GSR_DESTINATION_COLOR_RGB;

    gsr_color_conversion color_conversion;
    if(gsr_color_conversion_init(&color_conversion, &color_conversion_params) != 0) {
        fprintf(stderr, "gsr error: capture_image_to_file: failed to create color conversion\n");
        _exit(1);
    }

    gsr_color_conversion_clear(&color_conversion);

    gsr_color_conversion *output_color_conversion = plugins.num_plugins > 0 ? &plugins.color_conversion : &color_conversion;

    bool should_stop_error = false;
    egl->glClear(0);

    while(running) {
        while(gsr_window_process_event(window)) {
            if(x11_cursor_display && arg_parser.record_cursor)
                gsr_cursor_on_event(&x11_cursor, gsr_window_get_event_data(window));

            for(VideoSource &video_source : video_sources) {
                gsr_capture_on_event(video_source.capture, egl);
            }
        }

        if(x11_cursor_display && arg_parser.record_cursor)
            gsr_cursor_tick(&x11_cursor, DefaultRootWindow(x11_cursor_display));

        gsr_capture_kms_cleanup_kms_fds();

        if(kms_client_initialized) {
            if(gsr_kms_client_get_kms(&kms_client, &kms_response) != 0)
                fprintf(stderr, "gsr error: failed to get kms, error: %d (%s)\n", kms_response.result, kms_response.err_msg);
        }

        should_stop_error = false;
        for(VideoSource &video_source : video_sources) {
            gsr_capture_tick(video_source.capture);
            if(gsr_capture_should_stop(video_source.capture, &should_stop_error)) {
                running = 0;
                break;
            }
        }

        for(VideoSource &video_source : video_sources) {
            if(video_source.capture->pre_capture)
                video_source.capture->pre_capture(video_source.capture, &video_source.metadata, output_color_conversion);
        }

        if(output_color_conversion->schedule_clear) {
            output_color_conversion->schedule_clear = false;
            gsr_color_conversion_clear(output_color_conversion);
        }

        bool all_sources_captured = true;
        for(VideoSource &video_source : video_sources) {
            // It can fail, for example when capturing portal and the target is a monitor that hasn't been updated.
            // This can also happen for example if the system suspends and the monitor to capture's framebuffer is gone, or if the target window disappeared.
            if(gsr_capture_capture(video_source.capture, &video_source.metadata, output_color_conversion) != 0)
                all_sources_captured = false;
        }

        gsr_capture_kms_cleanup_kms_fds();

        if(all_sources_captured)
            break;

        if(running)
            usleep(30 * 1000); // 30 ms
    }

    if(plugins.num_plugins > 0) {
        gsr_plugins_draw(&plugins);
        gsr_color_conversion_draw(&color_conversion, plugins.texture,
            {0, 0}, video_size,
            {0, 0}, video_size,
            video_size, GSR_ROT_0, GSR_FLIP_NONE, GSR_SOURCE_COLOR_RGB, false);
    }

    gsr_egl_swap_buffers(egl);

    if(!should_stop_error) {
        if(!gsr_image_writer_write_to_file(&image_writer, arg_parser.filename, image_format, image_quality)) {
            fprintf(stderr, "gsr error: capture_image_to_file: failed to write opengl texture to image output file %s\n", arg_parser.filename);
            _exit(1);
        }

        if(arg_parser.recording_saved_script)
            run_recording_saved_script_async(arg_parser.recording_saved_script, arg_parser.filename, "screenshot");
    }

    gsr_plugins_deinit(&plugins);
    gsr_image_writer_deinit(&image_writer);
    for(VideoSource &video_source : video_sources) {
        gsr_capture_destroy(video_source.capture);
    }
    _exit(should_stop_error ? 3 : 0);
}

static AVPixelFormat get_pixel_format(gsr_video_codec video_codec, gsr_gpu_vendor vendor, bool use_software_video_encoder) {
    if(use_software_video_encoder) {
        return AV_PIX_FMT_NV12;
    } else {
        if(video_codec_is_vulkan(video_codec))
            return AV_PIX_FMT_VULKAN;
        else
            return vendor == GSR_GPU_VENDOR_NVIDIA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
    }
}

static void match_app_audio_input_to_available_apps(const std::vector<AudioInput> &requested_audio_inputs, const std::vector<std::string> &app_audio_names) {
    for(const AudioInput &request_audio_input : requested_audio_inputs) {
        if(request_audio_input.type != AudioInputType::APPLICATION || request_audio_input.inverted)
            continue;

        bool match = false;
        for(const std::string &app_name : app_audio_names) {
            if(strcasecmp(app_name.c_str(), request_audio_input.name.c_str()) == 0) {
                match = true;
                break;
            }
        }

        if(!match) {
            fprintf(stderr, "gsr warning: no audio application with the name \"%s\" was found, expected one of the following:\n", request_audio_input.name.c_str());
            for(const std::string &app_name : app_audio_names) {
                fprintf(stderr, "  * %s\n", app_name.c_str());
            }
            fprintf(stderr, "  assuming this is intentional (if you are trying to record audio for applications that haven't started yet).\n");
        }
    }
}

// Manually check if the audio inputs we give exist. This is only needed for pipewire, not pulseaudio.
// Pipewire instead DEFAULTS TO THE DEFAULT AUDIO INPUT. THAT'S RETARDED.
// OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
static std::vector<MergedAudioInputs> parse_audio_inputs(const AudioDevices &audio_devices, const Arg *audio_input_arg) {
    std::vector<MergedAudioInputs> requested_audio_inputs;

    for(int i = 0; i < audio_input_arg->num_values; ++i) {
        const char *audio_input = audio_input_arg->values[i];
        if(!audio_input || audio_input[0] == '\0')
            continue;

        requested_audio_inputs.push_back(parse_audio_input_arg(audio_input));
        for(AudioInput &request_audio_input : requested_audio_inputs.back().audio_inputs) {
            if(request_audio_input.type != AudioInputType::DEVICE)
                continue;

            bool match = false;

            if(request_audio_input.name == "default_output") {
                if(audio_devices.default_output.empty()) {
                    fprintf(stderr, "gsr error: -a default_output was specified but no default audio output is specified in the audio server\n");
                    _exit(2);
                }
                match = true;
            } else if(request_audio_input.name == "default_input") {
                if(audio_devices.default_input.empty()) {
                    fprintf(stderr, "gsr error: -a default_input was specified but no default audio input is specified in the audio server\n");
                    _exit(2);
                }
                match = true;
            } else {
                const bool name_is_existing_audio_device = get_audio_device_by_name(audio_devices.audio_inputs, request_audio_input.name.c_str()) != nullptr;
                if(name_is_existing_audio_device)
                    match = true;
            }

            if(!match) {
                fprintf(stderr, "gsr error: Audio device '%s' is not a valid audio device, expected one of:\n", request_audio_input.name.c_str());
                if(!audio_devices.default_output.empty())
                    fprintf(stderr, "    default_output (Default output)\n");
                if(!audio_devices.default_input.empty())
                    fprintf(stderr, "    default_input (Default input)\n");
                for(const auto &audio_device_input : audio_devices.audio_inputs) {
                    fprintf(stderr, "    %s (%s)\n", audio_device_input.name.c_str(), audio_device_input.description.c_str());
                }
                _exit(50);
            }
        }
    }

    return requested_audio_inputs;
}

static bool is_hex_num(char c) {
    return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static bool contains_non_hex_number(const char *str) {
    bool hex_start = false;
    size_t len = strlen(str);
    if(len >= 2 && memcmp(str, "0x", 2) == 0) {
        str += 2;
        len -= 2;
        hex_start = true;
    }

    bool is_hex = false;
    for(size_t i = 0; i < len; ++i) {
        char c = str[i];
        if(c == '\0')
            return false;
        if(!is_hex_num(c))
            return true;
        if((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
            is_hex = true;
    }

    return is_hex && !hex_start;
}

template <typename T>
static bool string_to_int(const char *str, size_t len, T *number) {
    char number_str[32];
    snprintf(number_str, sizeof(number_str), "%.*s", (int)len, str);

    errno = 0;
    *number = strtol(number_str, NULL, 0);
    return errno == 0;
}

static void capture_source_type_from_string(const char *capture_source_str, size_t size, CaptureSource &capture_source) {
    char capture_source_str_n[64];
    snprintf(capture_source_str_n, sizeof(capture_source_str_n), "%.*s", (int)size, capture_source_str);

    if(size == 7 && memcmp(capture_source_str_n, "focused", 7) == 0) {
        capture_source.type = GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW;
    } else if(size == 6 && memcmp(capture_source_str_n, "portal", 6) == 0) {
        capture_source.type = GSR_CAPTURE_SOURCE_TYPE_PORTAL;
    } else if(size == 6 && memcmp(capture_source_str_n, "region", 6) == 0) {
        capture_source.type = GSR_CAPTURE_SOURCE_TYPE_REGION;
    } else if(size >= 10 && memcmp(capture_source_str_n, "/dev/video", 10) == 0) {
        capture_source.type = GSR_CAPTURE_SOURCE_TYPE_V4L2;
    } else if(sscanf(capture_source_str_n, "%dx%d+%d+%d", &capture_source.region_size.x, &capture_source.region_size.y, &capture_source.region_pos.x, &capture_source.region_pos.y) == 4) {
        capture_source.type = GSR_CAPTURE_SOURCE_TYPE_REGION;
        capture_source.region_set = true;
    } else if(contains_non_hex_number(capture_source_str_n)) {
        capture_source.type = GSR_CAPTURE_SOURCE_TYPE_MONITOR;
    } else {
        capture_source.type = GSR_CAPTURE_SOURCE_TYPE_WINDOW;
    }
}

static bool string_to_capture_alignment(const char *str, size_t len, gsr_capture_alignment *alignment) {
    if(len == 5 && memcmp(str, "start", 5) == 0) {
        *alignment = GSR_CAPTURE_ALIGN_START;
        return true;
    } else if(len == 6 && memcmp(str, "center", 6) == 0) {
        *alignment = GSR_CAPTURE_ALIGN_CENTER;
        return true;
    } else if(len == 3 && memcmp(str, "end", 3) == 0) {
        *alignment = GSR_CAPTURE_ALIGN_END;
        return true;
    } else {
        return false;
    }
}

static bool string_to_v4l2_pixfmt(const char *str, size_t len, gsr_capture_v4l2_pixfmt *pixfmt) {
    if(len == 4 && memcmp(str, "auto", 4) == 0) {
        *pixfmt = GSR_CAPTURE_V4L2_PIXFMT_AUTO;
        return true;
    } else if(len == 4 && memcmp(str, "yuyv", 4) == 0) {
        *pixfmt = GSR_CAPTURE_V4L2_PIXFMT_YUYV;
        return true;
    } else if(len == 5 && memcmp(str, "mjpeg", 5) == 0) {
        *pixfmt = GSR_CAPTURE_V4L2_PIXFMT_MJPEG;
        return true;
    } else {
        return false;
    }
}

static bool string_to_bool(const char *str, size_t len, bool *value) {
    if(len == 4 && memcmp(str, "true", 4) == 0) {
        *value = true;
        return true;
    } else if(len == 5 && memcmp(str, "false", 5) == 0) {
        *value = false;
        return true;
    } else {
        return false;
    }
}

static void parse_capture_source_options(const std::string &capture_source_str, CaptureSource &capture_source) {
    bool is_first_column = true;

    split_string(capture_source_str, ';', [&](const char *sub, size_t size) {
        if(size == 0)
            return true;

        // First column contains the capture target
        if(is_first_column) {
            is_first_column = false;
            return true;
        }

        if(string_starts_with(sub, size, "x=")) {
            capture_source.pos.x_type = sub[size - 1] == '%' ? VVEC2I_TYPE_SCALAR : VVEC2I_TYPE_PIXELS;
            sub += 2;
            size -= 2;
            if(!string_to_int(sub, size, &capture_source.pos.x)) {
                fprintf(stderr, "gsr error: invalid capture target value for option x: \"%.*s\", expected a number\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "y=")) {
            capture_source.pos.y_type = sub[size - 1] == '%' ? VVEC2I_TYPE_SCALAR : VVEC2I_TYPE_PIXELS;
            sub += 2;
            size -= 2;
            if(!string_to_int(sub, size, &capture_source.pos.y)) {
                fprintf(stderr, "gsr error: invalid capture target value for option y: \"%.*s\", expected a number\n", (int)size, sub);
                _exit(1);
            }

        } else if(string_starts_with(sub, size, "width=")) {
            capture_source.size.x_type = sub[size - 1] == '%' ? VVEC2I_TYPE_SCALAR : VVEC2I_TYPE_PIXELS;
            sub += 6;
            size -= 6;
            if(!string_to_int(sub, size, &capture_source.size.x)) {
                fprintf(stderr, "gsr error: invalid capture target value for option width: \"%.*s\", expected a number\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "height=")) {
            capture_source.size.y_type = sub[size - 1] == '%' ? VVEC2I_TYPE_SCALAR : VVEC2I_TYPE_PIXELS;
            sub += 7;
            size -= 7;
            if(!string_to_int(sub, size, &capture_source.size.y)) {
                fprintf(stderr, "gsr error: invalid capture target value for option height: \"%.*s\", expected a number\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "halign=")) {
            sub += 7;
            size -= 7;
            if(!string_to_capture_alignment(sub, size, &capture_source.halign)) {
                fprintf(stderr, "gsr error: invalid capture target value for option halign: \"%.*s\", expected a \"start\", \"center\" or \"end\"\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "valign=")) {
            sub += 7;
            size -= 7;
            if(!string_to_capture_alignment(sub, size, &capture_source.valign)) {
                fprintf(stderr, "gsr error: invalid capture target value for option valign: \"%.*s\", expected a \"start\", \"center\" or \"end\"\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "pixfmt=")) {
            sub += 7;
            size -= 7;
            if(!string_to_v4l2_pixfmt(sub, size, &capture_source.v4l2_pixfmt)) {
                fprintf(stderr, "gsr error: invalid v4l2 pixfmt value for option pixfmt: \"%.*s\", expected a \"auto\", \"yuyv\" or \"mjpeg\"\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "hflip=")) {
            sub += 6;
            size -= 6;
            bool hflip = false;
            if(!string_to_bool(sub, size, &hflip)) {
                fprintf(stderr, "gsr error: invalid bool value for option hflip: \"%.*s\", expected a \"true\" or \"false\"\n", (int)size, sub);
                _exit(1);
            }

            if(hflip)
                capture_source.flip |= GSR_FLIP_HORIZONTAL;
        } else if(string_starts_with(sub, size, "vflip=")) {
            sub += 6;
            size -= 6;
            bool vflip = false;
            if(!string_to_bool(sub, size, &vflip)) {
                fprintf(stderr, "gsr error: invalid bool value for option vflip: \"%.*s\", expected a \"true\" or \"false\"\n", (int)size, sub);
                _exit(1);
            }

            if(vflip)
                capture_source.flip |= GSR_FLIP_VERTICAL;
        } else if(string_starts_with(sub, size, "camera_fps=")) {
            sub += 11;
            size -= 11;
            if(!string_to_int(sub, size, &capture_source.camera_fps)) {
                fprintf(stderr, "gsr error: invalid capture target value for option camera_fps: \"%.*s\", expected a number\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "camera_width=")) {
            sub += 13;
            size -= 13;
            if(!string_to_int(sub, size, &capture_source.camera_resolution.x)) {
                fprintf(stderr, "gsr error: invalid capture target value for option camera_width: \"%.*s\", expected a number\n", (int)size, sub);
                _exit(1);
            }
        } else if(string_starts_with(sub, size, "camera_height=")) {
            sub += 14;
            size -= 14;
            if(!string_to_int(sub, size, &capture_source.camera_resolution.y)) {
                fprintf(stderr, "gsr error: invalid capture target value for option camera_height: \"%.*s\", expected a number\n", (int)size, sub);
                _exit(1);
            }
        } else {
            fprintf(stderr, "gsr error: invalid capture target option \"%.*s\", expected x, y, width, height, halign, valign, pixfmt, hflip, vflip, camera_fps, camera_width or camera_height\n", (int)size, sub);
            _exit(1);
        }

        return true;
    });
}

static std::vector<CaptureSource> parse_capture_source_arg(const char *capture_source_arg, const args_parser &arg_parser) {
    std::vector<CaptureSource> requested_capture_sources;
    const bool has_multiple_capture_sources = strchr(capture_source_arg, '|') != nullptr;

    split_string(capture_source_arg, '|', [&](const char *sub, size_t size) {
        if(size == 0)
            return true;

        const char *substr_start = sub;
        size_t capture_source_size = size;
        const char *capture_source_end = (const char*)memchr(sub, ';', size);
        if(capture_source_end)
            capture_source_size = capture_source_end - sub;

        CaptureSource capture_source;
        capture_source.region_pos = arg_parser.region_position;
        capture_source.region_size = arg_parser.region_size;

        if(string_starts_with(sub, capture_source_size, "monitor:")) {
            capture_source.type = GSR_CAPTURE_SOURCE_TYPE_MONITOR;
            sub += 8;
            capture_source_size -= 8;
        } else if(string_starts_with(sub, capture_source_size, "window:")) {
            capture_source.type = GSR_CAPTURE_SOURCE_TYPE_WINDOW;
            sub += 7;
            capture_source_size -= 7;
        } else if(string_starts_with(sub, capture_source_size, "v4l2:")) {
            capture_source.type = GSR_CAPTURE_SOURCE_TYPE_V4L2;
            sub += 5;
            capture_source_size -= 5;
        } else {
            capture_source_type_from_string(sub, capture_source_size, capture_source);
        }

        capture_source.name.assign(sub, capture_source_size);

        if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_WINDOW) {
            if(!string_to_int(capture_source.name.c_str(), capture_source.name.size(), &capture_source.window_id)) {
                fprintf(stderr, "gsr error: invalid window number %s\n", capture_source.name.c_str());
                args_parser_print_usage();
                _exit(1);
            }
        }

        if(has_multiple_capture_sources) {
            capture_source.halign = GSR_CAPTURE_ALIGN_START;
            capture_source.valign = GSR_CAPTURE_ALIGN_START;
            capture_source.pos = {0, 0, VVEC2I_TYPE_PIXELS, VVEC2I_TYPE_PIXELS};
        }

        parse_capture_source_options(std::string(substr_start, size), capture_source);
        requested_capture_sources.push_back(capture_source);
        return true;
    });

    return requested_capture_sources;
}

static bool audio_inputs_has_app_audio(const std::vector<AudioInput> &audio_inputs) {
    for(const auto &audio_input : audio_inputs) {
        if(audio_input.type == AudioInputType::APPLICATION)
            return true;
    }
    return false;
}

static bool merged_audio_inputs_has_app_audio(const std::vector<MergedAudioInputs> &merged_audio_inputs) {
    for(const auto &merged_audio_input : merged_audio_inputs) {
        if(audio_inputs_has_app_audio(merged_audio_input.audio_inputs))
            return true;
    }
    return false;
}

// Should use amix if more than 1 audio device and 0 application audio, merged
static bool audio_inputs_should_use_amix(const std::vector<AudioInput> &audio_inputs) {
    int num_audio_devices = 0;
    int num_app_audio = 0;

    for(const auto &audio_input : audio_inputs) {
        if(audio_input.type == AudioInputType::DEVICE)
            ++num_audio_devices;
        else if(audio_input.type == AudioInputType::APPLICATION)
            ++num_app_audio;
    }

    return num_audio_devices > 1 && num_app_audio == 0;
}

static bool merged_audio_inputs_should_use_amix(const std::vector<MergedAudioInputs> &merged_audio_inputs) {
    for(const auto &merged_audio_input : merged_audio_inputs) {
        if(audio_inputs_should_use_amix(merged_audio_input.audio_inputs))
            return true;
    }
    return false;
}

static void validate_merged_audio_inputs_app_audio(const std::vector<MergedAudioInputs> &merged_audio_inputs, const std::vector<std::string> &app_audio_names) {
    for(const auto &merged_audio_input : merged_audio_inputs) {
        int num_app_audio = 0;
        int num_app_inverted_audio = 0;

        for(const auto &audio_input : merged_audio_input.audio_inputs) {
            if(audio_input.type == AudioInputType::APPLICATION) {
                if(audio_input.inverted)
                    ++num_app_inverted_audio;
                else
                    ++num_app_audio;
            }
        }

        match_app_audio_input_to_available_apps(merged_audio_input.audio_inputs, app_audio_names);

        if(num_app_audio > 0 && num_app_inverted_audio > 0) {
            fprintf(stderr, "gsr error: argument -a was provided with both app: and app-inverse:, only one of them can be used for one audio track\n");
            _exit(2);
        }
    }
}

static gsr_audio_codec select_audio_codec_with_fallback(gsr_audio_codec audio_codec, const std::string &file_extension, bool uses_amix) {
    switch(audio_codec) {
        case GSR_AUDIO_CODEC_AAC: {
            if(file_extension == "webm") {
                //audio_codec_to_use = "opus";
                audio_codec = GSR_AUDIO_CODEC_OPUS;
                fprintf(stderr, "gsr warning: .webm files only support opus audio codec, changing audio codec from aac to opus\n");
            }
            break;
        }
        case GSR_AUDIO_CODEC_OPUS: {
            if(file_extension != "mp4" && file_extension != "mkv" && file_extension != "webm" && file_extension != "ts") {
                //audio_codec_to_use = "aac";
                audio_codec = GSR_AUDIO_CODEC_AAC;
                fprintf(stderr, "gsr warning: opus audio codec is only supported by .mp4, .mkv, .webm and .ts files, falling back to aac instead\n");
            }
            break;
        }
        case GSR_AUDIO_CODEC_FLAC: {
            // TODO: Also check mpegts?
            if(file_extension == "webm") {
                //audio_codec_to_use = "opus";
                audio_codec = GSR_AUDIO_CODEC_OPUS;
                fprintf(stderr, "gsr warning: .webm files only support opus audio codec, changing audio codec from flac to opus\n");
            } else if(file_extension != "mp4" && file_extension != "mkv") {
                //audio_codec_to_use = "aac";
                audio_codec = GSR_AUDIO_CODEC_AAC;
                fprintf(stderr, "gsr warning: flac audio codec is only supported by .mp4 and .mkv files, falling back to aac instead\n");
            } else if(uses_amix) {
                // TODO: remove this? is it true anymore?
                //audio_codec_to_use = "opus";
                audio_codec = GSR_AUDIO_CODEC_OPUS;
                fprintf(stderr, "gsr warning: flac audio codec is not supported when mixing audio sources, falling back to opus instead\n");
            }
            break;
        }
    }
    return audio_codec;
}

static bool video_codec_only_supports_low_power_mode(const gsr_supported_video_codecs &supported_video_codecs, gsr_video_codec video_codec) {
    switch(video_codec) {
        case GSR_VIDEO_CODEC_H264:              return supported_video_codecs.h264.low_power;
        case GSR_VIDEO_CODEC_HEVC:              return supported_video_codecs.hevc.low_power;
        case GSR_VIDEO_CODEC_HEVC_HDR:          return supported_video_codecs.hevc_hdr.low_power;
        case GSR_VIDEO_CODEC_HEVC_10BIT:        return supported_video_codecs.hevc_10bit.low_power;
        case GSR_VIDEO_CODEC_AV1:               return supported_video_codecs.av1.low_power;
        case GSR_VIDEO_CODEC_AV1_HDR:           return supported_video_codecs.av1_hdr.low_power;
        case GSR_VIDEO_CODEC_AV1_10BIT:         return supported_video_codecs.av1_10bit.low_power;
        case GSR_VIDEO_CODEC_VP8:               return supported_video_codecs.vp8.low_power;
        case GSR_VIDEO_CODEC_VP9:               return supported_video_codecs.vp9.low_power;
        case GSR_VIDEO_CODEC_H264_VULKAN:       return supported_video_codecs.h264.low_power;
        case GSR_VIDEO_CODEC_HEVC_VULKAN:       return supported_video_codecs.hevc.low_power;
        case GSR_VIDEO_CODEC_HEVC_HDR_VULKAN:   return supported_video_codecs.hevc_hdr.low_power;
        case GSR_VIDEO_CODEC_HEVC_10BIT_VULKAN: return supported_video_codecs.hevc_10bit.low_power;
        case GSR_VIDEO_CODEC_AV1_VULKAN:        return supported_video_codecs.av1.low_power;
        case GSR_VIDEO_CODEC_AV1_HDR_VULKAN:    return supported_video_codecs.av1_hdr.low_power;
        case GSR_VIDEO_CODEC_AV1_10BIT_VULKAN:  return supported_video_codecs.av1_10bit.low_power;
    }
    return false;
}

static const AVCodec* get_av_codec_if_supported(gsr_video_codec video_codec, gsr_egl *egl, bool use_software_video_encoder, const gsr_supported_video_codecs *supported_video_codecs) {
    switch(video_codec) {
        case GSR_VIDEO_CODEC_H264:
        case GSR_VIDEO_CODEC_H264_VULKAN: {
            if(use_software_video_encoder)
                return avcodec_find_encoder_by_name("libx264");
            else if(supported_video_codecs->h264.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_HEVC:
        case GSR_VIDEO_CODEC_HEVC_VULKAN: {
            if(supported_video_codecs->hevc.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_HEVC_HDR:
        case GSR_VIDEO_CODEC_HEVC_HDR_VULKAN: {
            if(supported_video_codecs->hevc_hdr.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_HEVC_10BIT:
         case GSR_VIDEO_CODEC_HEVC_10BIT_VULKAN: {
            if(supported_video_codecs->hevc_10bit.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_AV1:
        case GSR_VIDEO_CODEC_AV1_VULKAN: {
            if(supported_video_codecs->av1.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_AV1_HDR:
        case GSR_VIDEO_CODEC_AV1_HDR_VULKAN: {
            if(supported_video_codecs->av1_hdr.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_AV1_10BIT:
        case GSR_VIDEO_CODEC_AV1_10BIT_VULKAN: {
            if(supported_video_codecs->av1_10bit.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_VP8: {
            if(supported_video_codecs->vp8.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
        case GSR_VIDEO_CODEC_VP9: {
            if(supported_video_codecs->vp9.supported)
                return get_ffmpeg_video_codec(video_codec, egl->gpu_info.vendor);
            break;
        }
    }
    return nullptr;
}

static vec2i codec_get_max_resolution(gsr_video_codec video_codec, bool use_software_video_encoder, const gsr_supported_video_codecs *supported_video_codecs) {
    switch(video_codec) {
        case GSR_VIDEO_CODEC_H264:
        case GSR_VIDEO_CODEC_H264_VULKAN: {
            if(use_software_video_encoder)
                return {4096, 2304};
            else if(supported_video_codecs->h264.supported)
                return supported_video_codecs->h264.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_HEVC:
        case GSR_VIDEO_CODEC_HEVC_VULKAN: {
            if(supported_video_codecs->hevc.supported)
                return supported_video_codecs->hevc.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_HEVC_HDR:
        case GSR_VIDEO_CODEC_HEVC_HDR_VULKAN: {
            if(supported_video_codecs->hevc_hdr.supported)
                return supported_video_codecs->hevc_hdr.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_HEVC_10BIT:
        case GSR_VIDEO_CODEC_HEVC_10BIT_VULKAN: {
            if(supported_video_codecs->hevc_10bit.supported)
                return supported_video_codecs->hevc_10bit.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_AV1:
        case GSR_VIDEO_CODEC_AV1_VULKAN: {
            if(supported_video_codecs->av1.supported)
                return supported_video_codecs->av1.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_AV1_HDR:
        case GSR_VIDEO_CODEC_AV1_HDR_VULKAN: {
            if(supported_video_codecs->av1_hdr.supported)
                return supported_video_codecs->av1_hdr.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_AV1_10BIT:
        case GSR_VIDEO_CODEC_AV1_10BIT_VULKAN: {
            if(supported_video_codecs->av1_10bit.supported)
                return supported_video_codecs->av1_10bit.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_VP8: {
            if(supported_video_codecs->vp8.supported)
                return supported_video_codecs->vp8.max_resolution;
            break;
        }
        case GSR_VIDEO_CODEC_VP9: {
            if(supported_video_codecs->vp9.supported)
                return supported_video_codecs->vp9.max_resolution;
            break;
        }
    }
    return {0, 0};
}

static bool codec_supports_resolution(vec2i codec_max_resolution, vec2i capture_resolution) {
    if(codec_max_resolution.x == 0 || codec_max_resolution.y == 0)
        return true;
    return codec_max_resolution.x >= capture_resolution.x && codec_max_resolution.y >= capture_resolution.y;
}

static void print_codec_error(gsr_video_codec video_codec) {
    if(video_codec == (gsr_video_codec)GSR_VIDEO_CODEC_AUTO)
        video_codec = GSR_VIDEO_CODEC_H264;

    const char *video_codec_name = video_codec_to_string(video_codec);
    fprintf(stderr, "gsr error: your gpu does not support '%s' video codec. If you are sure that your gpu does support '%s' video encoding and you are using an AMD/Intel GPU,\n"
        "  then make sure you have installed the GPU specific vaapi packages (intel-media-driver, libva-intel-driver, libva-mesa-driver and linux-firmware).\n"
        "  It's also possible that your distro has disabled hardware accelerated video encoding for '%s' video codec.\n"
        "  This may be the case on corporate distros such as Manjaro, Fedora or OpenSUSE.\n"
        "  You can test this by running 'vainfo | grep VAEntrypointEncSlice' to see if it matches any H264/HEVC/AV1/VP8/VP9 profile.\n"
        "  On such distros, you need to manually install mesa from source to enable H264/HEVC hardware acceleration, or use a more user friendly distro. Alternatively record with AV1 if supported by your GPU.\n"
        "  You can alternatively use the flatpak version of GPU Screen Recorder (https://flathub.org/apps/com.dec05eba.gpu_screen_recorder) which bypasses system issues with patented H264/HEVC codecs.\n"
        "  If your GPU doesn't support hardware accelerated video encoding then you can use '-fallback-cpu-encoding yes' option to encode with your cpu instead.\n", video_codec_name, video_codec_name, video_codec_name);
}

static void force_cpu_encoding(args_parser *args_parser) {
    args_parser->video_codec = GSR_VIDEO_CODEC_H264;
    args_parser->video_encoder = GSR_VIDEO_ENCODER_HW_CPU;
    if(args_parser->bitrate_mode == GSR_BITRATE_MODE_VBR) {
        fprintf(stderr, "gsr warning: bitrate mode has been forcefully set to qp because software encoding option doesn't support vbr option\n");
        args_parser->bitrate_mode = GSR_BITRATE_MODE_QP;
    }
}

static const AVCodec* pick_video_codec(gsr_egl *egl, args_parser *args_parser, bool use_fallback_codec, bool *low_power, gsr_supported_video_codecs *supported_video_codecs) {
    // TODO: software encoder for hevc, av1, vp8 and vp9
    *low_power = false;
    const AVCodec *video_codec_f = get_av_codec_if_supported(args_parser->video_codec, egl, args_parser->video_encoder == GSR_VIDEO_ENCODER_HW_CPU, supported_video_codecs);

    if(!video_codec_f && use_fallback_codec && args_parser->video_encoder != GSR_VIDEO_ENCODER_HW_CPU) {
        switch(args_parser->video_codec) {
            case GSR_VIDEO_CODEC_H264: {
                fprintf(stderr, "gsr error: selected video codec h264 is not supported by your hardware\n");
                if(args_parser->fallback_cpu_encoding) {
                    fprintf(stderr, "gsr warning: gpu encoding is not available on your system, trying cpu encoding instead because -fallback-cpu-encoding is enabled. Install the proper vaapi drivers on your system (if supported) if you experience performance issues\n");
                    force_cpu_encoding(args_parser);
                }
                break;
            }
            case GSR_VIDEO_CODEC_HEVC:
            case GSR_VIDEO_CODEC_HEVC_HDR:
            case GSR_VIDEO_CODEC_HEVC_10BIT: {
                fprintf(stderr, "gsr warning: selected video codec hevc is not supported by your hardware, trying h264 instead\n");
                args_parser->video_codec = GSR_VIDEO_CODEC_H264;
                return pick_video_codec(egl, args_parser, true, low_power, supported_video_codecs);
            }
            case GSR_VIDEO_CODEC_AV1:
            case GSR_VIDEO_CODEC_AV1_HDR:
            case GSR_VIDEO_CODEC_AV1_10BIT: {
                fprintf(stderr, "gsr warning: selected video codec av1 is not supported by your hardware, trying h264 instead\n");
                args_parser->video_codec = GSR_VIDEO_CODEC_H264;
                return pick_video_codec(egl, args_parser, true, low_power, supported_video_codecs);
            }
            case GSR_VIDEO_CODEC_VP8:
            case GSR_VIDEO_CODEC_VP9:
                // TODO: Cant fallback to other codec because webm only supports vp8/vp9
                break;
            case GSR_VIDEO_CODEC_H264_VULKAN: {
                fprintf(stderr, "gsr warning: selected video codec h264_vulkan is not supported by your hardware, trying h264 instead\n");
                args_parser->video_codec = GSR_VIDEO_CODEC_H264;
                // Need to do a query again because this time it's without vulkan
                if(!get_supported_video_codecs(egl, args_parser->video_codec, false, true, supported_video_codecs)) {
                    fprintf(stderr, "gsr error: failed to query for supported video codecs\n");
                    print_codec_error(args_parser->video_codec);
                    _exit(11);
                }
                return pick_video_codec(egl, args_parser, true, low_power, supported_video_codecs);
            }
            case GSR_VIDEO_CODEC_HEVC_VULKAN:
            case GSR_VIDEO_CODEC_HEVC_HDR_VULKAN:
            case GSR_VIDEO_CODEC_HEVC_10BIT_VULKAN: {
                fprintf(stderr, "gsr warning: selected video codec hevc_vulkan is not supported by your hardware, trying hevc instead\n");
                args_parser->video_codec = GSR_VIDEO_CODEC_HEVC;
                // Need to do a query again because this time it's without vulkan
                if(!get_supported_video_codecs(egl, args_parser->video_codec, false, true, supported_video_codecs)) {
                    fprintf(stderr, "gsr error: failed to query for supported video codecs\n");
                    print_codec_error(args_parser->video_codec);
                    _exit(11);
                }
                return pick_video_codec(egl, args_parser, true, low_power, supported_video_codecs);
            }
            case GSR_VIDEO_CODEC_AV1_VULKAN:
            case GSR_VIDEO_CODEC_AV1_HDR_VULKAN:
            case GSR_VIDEO_CODEC_AV1_10BIT_VULKAN: {
                fprintf(stderr, "gsr warning: selected video codec av1_vulkan is not supported by your hardware, trying av1 instead\n");
                args_parser->video_codec = GSR_VIDEO_CODEC_AV1;
                // Need to do a query again because this time it's without vulkan
                if(!get_supported_video_codecs(egl, args_parser->video_codec, false, true, supported_video_codecs)) {
                    fprintf(stderr, "gsr error: failed to query for supported video codecs\n");
                    print_codec_error(args_parser->video_codec);
                    _exit(11);
                }
                return pick_video_codec(egl, args_parser, true, low_power, supported_video_codecs);
            }
        }

        video_codec_f = get_av_codec_if_supported(args_parser->video_codec, egl, args_parser->video_encoder == GSR_VIDEO_ENCODER_HW_CPU, supported_video_codecs);
    }

    if(!video_codec_f) {
        print_codec_error(args_parser->video_codec);
        _exit(54);
    }

    *low_power = video_codec_only_supports_low_power_mode(*supported_video_codecs, args_parser->video_codec);

    return video_codec_f;
}

/* Returns -1 if none is available */
static gsr_video_codec select_appropriate_video_codec_automatically(vec2i video_size, const gsr_supported_video_codecs *supported_video_codecs) {
    if(supported_video_codecs->h264.supported && codec_supports_resolution(supported_video_codecs->h264.max_resolution, video_size)) {
        fprintf(stderr, "gsr info: using h264 encoder because a codec was not specified\n");
        return GSR_VIDEO_CODEC_H264;
    } else if(supported_video_codecs->hevc.supported && codec_supports_resolution(supported_video_codecs->hevc.max_resolution, video_size)) {
        fprintf(stderr, "gsr info: using hevc encoder because a codec was not specified and h264 supported max resolution (%dx%d) is less than the capture resolution (%dx%d)\n",
            supported_video_codecs->h264.max_resolution.x, supported_video_codecs->h264.max_resolution.y,
            video_size.x, video_size.y);
        return GSR_VIDEO_CODEC_HEVC;
    } else if(supported_video_codecs->av1.supported && codec_supports_resolution(supported_video_codecs->av1.max_resolution, video_size)) {
        fprintf(stderr, "gsr info: using av1 encoder because a codec was not specified and hevc supported max resolution (%dx%d) is less than the capture resolution (%dx%d)\n",
            supported_video_codecs->hevc.max_resolution.x, supported_video_codecs->hevc.max_resolution.y,
            video_size.x, video_size.y);
        return GSR_VIDEO_CODEC_AV1;
    } else {
        return (gsr_video_codec)-1;
    }
}

static const AVCodec* select_video_codec_with_fallback(vec2i video_size, args_parser *args_parser, const char *file_extension, gsr_egl *egl, bool *low_power) {
    gsr_supported_video_codecs supported_video_codecs_non_vulkan;
    get_supported_video_codecs(egl, args_parser->video_codec, args_parser->video_encoder == GSR_VIDEO_ENCODER_HW_CPU, true, &supported_video_codecs_non_vulkan);

    gsr_supported_video_codecs supported_video_codecs_vulkan = supported_video_codecs_non_vulkan;
    set_supported_video_codecs_ffmpeg(&supported_video_codecs_non_vulkan, &supported_video_codecs_vulkan, egl->gpu_info.vendor);

    gsr_supported_video_codecs *supported_video_codecs = video_codec_is_vulkan(args_parser->video_codec)
        ? &supported_video_codecs_vulkan
        : &supported_video_codecs_non_vulkan;

    const bool video_codec_auto = args_parser->video_codec == (gsr_video_codec)GSR_VIDEO_CODEC_AUTO;
    if(video_codec_auto) {
        if(strcmp(file_extension, "webm") == 0) {
            fprintf(stderr, "gsr info: using vp8 encoder because a codec was not specified and the file extension is .webm\n");
            args_parser->video_codec = GSR_VIDEO_CODEC_VP8;
        } else if(args_parser->video_encoder == GSR_VIDEO_ENCODER_HW_CPU) {
            fprintf(stderr, "gsr info: using h264 encoder because a codec was not specified\n");
            args_parser->video_codec = GSR_VIDEO_CODEC_H264;
        } else if(args_parser->video_encoder != GSR_VIDEO_ENCODER_HW_CPU) {
            args_parser->video_codec = select_appropriate_video_codec_automatically(video_size, &supported_video_codecs_non_vulkan);
            if(args_parser->video_codec == (gsr_video_codec)-1) {
                if(args_parser->fallback_cpu_encoding) {
                    fprintf(stderr, "gsr warning: gpu encoding is not available on your system or your gpu doesn't support recording at the resolution you are trying to record, trying cpu encoding instead because -fallback-cpu-encoding is enabled. Install the proper vaapi drivers on your system (if supported) if you experience performance issues\n");
                    force_cpu_encoding(args_parser);
                } else {
                    fprintf(stderr, "gsr error: no video encoder was specified and neither h264, hevc nor av1 are supported on your system or you are trying to capture at a resolution higher than your system supports for each codec.\n");
                    fprintf(stderr, "  Ensure that you have installed the proper vaapi driver. If your gpu doesn't support video encoding then you can run gpu-screen-recorder with \"-fallback-cpu-encoding yes\" option to use cpu encoding.\n");
                    _exit(52);
                }
            }
        }
    }

    if(LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(60, 10, 100) && strcmp(file_extension, "flv") == 0) {
        if(args_parser->video_codec != GSR_VIDEO_CODEC_H264) {
            args_parser->video_codec = GSR_VIDEO_CODEC_H264;
            fprintf(stderr, "gsr warning: hevc/av1 is not compatible with flv in your outdated version of ffmpeg, falling back to h264 instead.\n");
        }
    } else if(strcmp(file_extension, "m3u8") == 0) {
        if(video_codec_is_av1(args_parser->video_codec)) {
            args_parser->video_codec = GSR_VIDEO_CODEC_HEVC;
            fprintf(stderr, "gsr warning: av1 is not compatible with hls (m3u8), falling back to hevc instead.\n");
        }
    }

    const AVCodec *codec = pick_video_codec(egl, args_parser, true, low_power, supported_video_codecs);

    const vec2i codec_max_resolution = codec_get_max_resolution(args_parser->video_codec, args_parser->video_encoder == GSR_VIDEO_ENCODER_HW_CPU, supported_video_codecs);
    if(!codec_supports_resolution(codec_max_resolution, video_size)) {
        const char *video_codec_name = video_codec_to_string(args_parser->video_codec);
        fprintf(stderr, "gsr error: The max resolution for video codec %s is %dx%d while you are trying to capture at resolution %dx%d. Change capture resolution or video codec and try again\n",
            video_codec_name, codec_max_resolution.x, codec_max_resolution.y, video_size.x, video_size.y);
        _exit(53);
    }

    return codec;
}

static std::vector<AudioDeviceData> create_device_audio_inputs(const std::vector<AudioInput> &audio_inputs, AVCodecContext *audio_codec_context, int num_channels, double num_audio_frames_shift, std::vector<AVFilterContext*> &src_filter_ctx, bool use_amix) {
    std::vector<AudioDeviceData> audio_track_audio_devices;
    for(size_t i = 0; i < audio_inputs.size(); ++i) {
        const auto &audio_input = audio_inputs[i];
        AVFilterContext *src_ctx = nullptr;
        if(use_amix)
            src_ctx = src_filter_ctx[i];

        AudioDeviceData audio_device;
        audio_device.audio_input = audio_input;
        audio_device.src_filter_ctx = src_ctx;

        if(audio_input.name.empty()) {
            audio_device.sound_device.handle = NULL;
            audio_device.sound_device.frames = 0;
        } else {
            const std::string description = "gsr-" + audio_input.name;
            if(sound_device_get_by_name(&audio_device.sound_device, description.c_str(), audio_input.name.c_str(), description.c_str(), num_channels, audio_codec_context->frame_size, audio_codec_context_get_audio_format(audio_codec_context)) != 0) {
                fprintf(stderr, "gsr error: failed to get \"%s\" audio device\n", audio_input.name.c_str());
                _exit(1);
            }
        }

        audio_device.frame = create_audio_frame(audio_codec_context);
        audio_device.frame->pts = -audio_codec_context->frame_size * num_audio_frames_shift;

        audio_track_audio_devices.push_back(std::move(audio_device));
    }
    return audio_track_audio_devices;
}

#ifdef GSR_APP_AUDIO
static AudioDeviceData create_application_audio_audio_input(const MergedAudioInputs &merged_audio_inputs, AVCodecContext *audio_codec_context, int num_channels, double num_audio_frames_shift, gsr_pipewire_audio *pipewire_audio) {
    AudioDeviceData audio_device;
    audio_device.frame = create_audio_frame(audio_codec_context);
    audio_device.frame->pts = -audio_codec_context->frame_size * num_audio_frames_shift;

    char random_str[8];
    if(!generate_random_characters_standard_alphabet(random_str, sizeof(random_str))) {
        fprintf(stderr, "gsr error: failed to generate random string\n");
        _exit(1);
    }

    std::string combined_sink_name = "gsr-combined-";
    combined_sink_name.append(random_str, sizeof(random_str));
    combined_sink_name += ".monitor";

    if(sound_device_get_by_name(&audio_device.sound_device, combined_sink_name.c_str(), "", "gpu-screen-recorder", num_channels, audio_codec_context->frame_size, audio_codec_context_get_audio_format(audio_codec_context)) != 0) {
        fprintf(stderr, "gsr error: failed to setup audio recording to combined sink\n");
        _exit(1);
    }

    std::vector<const char*> audio_devices_sources;
    for(const auto &audio_input : merged_audio_inputs.audio_inputs) {
        if(audio_input.type == AudioInputType::DEVICE)
            audio_devices_sources.push_back(audio_input.name.c_str());
    }

    bool app_audio_inverted = false;
    std::vector<const char*> app_names;
    for(const auto &audio_input : merged_audio_inputs.audio_inputs) {
        if(audio_input.type == AudioInputType::APPLICATION) {
            app_names.push_back(audio_input.name.c_str());
            app_audio_inverted = audio_input.inverted;
        }
    }

    if(!audio_devices_sources.empty()) {
        if(!gsr_pipewire_audio_add_link_from_sources_to_stream(pipewire_audio, audio_devices_sources.data(), audio_devices_sources.size(), combined_sink_name.c_str())) {
            fprintf(stderr, "gsr error: failed to add application audio link\n");
            _exit(1);
        }
    }

    if(app_audio_inverted) {
        if(!gsr_pipewire_audio_add_link_from_apps_to_stream_inverted(pipewire_audio, app_names.data(), app_names.size(), combined_sink_name.c_str())) {
            fprintf(stderr, "gsr error: failed to add application audio link\n");
            _exit(1);
        }
    } else {
        if(!gsr_pipewire_audio_add_link_from_apps_to_stream(pipewire_audio, app_names.data(), app_names.size(), combined_sink_name.c_str())) {
            fprintf(stderr, "gsr error: failed to add application audio link\n");
            _exit(1);
        }
    }

    return audio_device;
}
#endif

static bool get_image_format_from_filename(const char *filename, gsr_image_format *image_format) {
    if(string_ends_with(filename, ".jpg") || string_ends_with(filename, ".jpeg")) {
        *image_format = GSR_IMAGE_FORMAT_JPEG;
        return true;
    } else if(string_ends_with(filename, ".png")) {
        *image_format = GSR_IMAGE_FORMAT_PNG;
        return true;
    } else {
        return false;
    }
}

// TODO: replace this with start_recording_create_steams
static bool av_open_file_write_header(AVFormatContext *av_format_context, const char *filename, const char *ffmpeg_opts) {
    int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
    if(ret < 0) {
        fprintf(stderr, "gsr error: Could not open '%s': %s\n", filename, av_error_to_string(ret));
        return false;
    }

    AVDictionary *options = nullptr;
    av_dict_set(&options, "strict", "experimental", 0);

    if(ffmpeg_opts)
        av_dict_parse_string(&options, ffmpeg_opts, "=", ";", 0);

    ret = avformat_write_header(av_format_context, &options);
    if(ret < 0)
        fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));

    const bool success = ret >= 0;
    if(!success)
        avio_close(av_format_context->pb);

    av_dict_free(&options);
    return success;
}

static int audio_codec_get_frame_size(gsr_audio_codec audio_codec) {
    switch(audio_codec) {
        case GSR_AUDIO_CODEC_AAC: return 1024;
        case GSR_AUDIO_CODEC_OPUS: return 960;
        case GSR_AUDIO_CODEC_FLAC:
            assert(false);
            return 1024;
    }
    assert(false);
    return 1024;
}

static size_t calculate_estimated_replay_buffer_packets(int64_t replay_buffer_size_secs, int fps, gsr_audio_codec audio_codec, const std::vector<MergedAudioInputs> &audio_inputs) {
    if(replay_buffer_size_secs == -1)
        return 0;

    int audio_fps = 0;
    if(!audio_inputs.empty())
        audio_fps = AUDIO_SAMPLE_RATE / audio_codec_get_frame_size(audio_codec);

    return replay_buffer_size_secs * (fps + audio_fps * audio_inputs.size());
}

static void set_display_server_environment_variables() {
    // Some users dont have properly setup environments (no display manager that does systemctl --user import-environment DISPLAY WAYLAND_DISPLAY)
    const char *display = getenv("DISPLAY");
    if(!display) {
        display = ":0";
        setenv("DISPLAY", display, true);
    }

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if(!wayland_display) {
        wayland_display = "wayland-0";
        setenv("WAYLAND_DISPLAY", wayland_display, true);
    }
}

static bool is_capturing_damage_tracked_target(const std::vector<CaptureSource> &capture_sources) {
    for(const CaptureSource &capture_source : capture_sources) {
        if(capture_source.type != GSR_CAPTURE_SOURCE_TYPE_V4L2)
            return true;
    }
    return false;
}

static bool is_capturing_type(const std::vector<CaptureSource> &capture_sources, CaptureSourceType target_type) {
    for(const CaptureSource &capture_source : capture_sources) {
        if(capture_source.type == target_type)
            return true;
    }
    return false;
}

static bool has_capture_source_with_region_set(const std::vector<CaptureSource> &capture_sources) {
    for(const CaptureSource &capture_source : capture_sources) {
        if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_REGION && capture_source.region_set)
            return true;
    }
    return false;
}

static bool is_capturing_monitor_or_region(const std::vector<CaptureSource> &capture_sources) {
    for(const CaptureSource &capture_source : capture_sources) {
        if(capture_source.type == GSR_CAPTURE_SOURCE_TYPE_MONITOR || capture_source.type == GSR_CAPTURE_SOURCE_TYPE_REGION)
            return true;
    }
    return false;
}

static void validate_args_with_capture_sources(args_parser &arg_parser, const std::vector<CaptureSource> &capture_sources) {
    const Arg *output_resolution_arg = args_parser_get_arg(&arg_parser, "-s");
    assert(output_resolution_arg);

    const Arg *region_arg = args_parser_get_arg(&arg_parser, "-region");
    assert(region_arg);

    if(is_capturing_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW) && output_resolution_arg->num_values == 0) {
        fprintf(stderr, "gsr error: option -s is required when using '-w focused' option\n");
        args_parser_print_usage();
        _exit(1);
    }

    const bool is_capturing_region = is_capturing_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_REGION);
    if(region_arg->num_values == 0) {
        if(is_capturing_region && !has_capture_source_with_region_set(capture_sources)) {
            fprintf(stderr, "gsr error: option -region is required when '-w region' is used\n");
            args_parser_print_usage();
            _exit(1);
        }
    } else {
        if(is_capturing_region) {
            fprintf(stderr, "gsr warning: option -region is deprecated, use -w with region directly instead, for example: -w %s\n", region_arg->values[0]);
        } else {
            fprintf(stderr, "gsr error: option -region can only be used when option '-w region' is used\n");
            args_parser_print_usage();
            _exit(1);
        }
    }

    if(!arg_parser.restore_portal_session && is_capturing_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_PORTAL))
        fprintf(stderr, "gsr info: option '-w portal' was used without '-restore-portal-session yes'. The previous screencast session will be ignored\n");
}

static void install_cuda_no_stable_perf_limit() {
    const char *home = getenv("HOME");
    if(!home) {
        fprintf(stderr, "gsr warning: install_cuda_no_stable_perf_limit: $HOME not set\n");
        return;
    }

    char nv_profiles_path[4096];
    snprintf(nv_profiles_path, sizeof(nv_profiles_path), "%s/.nv/nvidia-application-profiles-rc.d", home);

    if(create_directory_recursive(nv_profiles_path) != 0) {
        fprintf(stderr, "gsr warning: install_cuda_no_stable_perf_limit: failed to create directory: %s\n", nv_profiles_path);
        return;
    }

    snprintf(nv_profiles_path, sizeof(nv_profiles_path), "%s/.nv/nvidia-application-profiles-rc.d/10-gsr-cuda-no-stable-perf-limit", home);

    FILE *f = fopen(nv_profiles_path, "wb");
    if(!f) {
        fprintf(stderr, "gsr warning: install_cuda_no_stable_perf_limit: failed to create file: %s\n", nv_profiles_path);
        return;
    }

    const char *profile_data =
        "{\n"
        "    \"profiles\": [\n"
        "        {\n"
        "            \"name\": \"CudaNoStablePerfLimit\",\n"
        "            \"settings\": [\"0x166c5e\", 0]\n"
        "        }\n"
        "    ],\n"
        "    \"rules\": [\n"
        "        { \"pattern\": \"gpu-screen-recorder\", \"profile\": \"CudaNoStablePerfLimit\" }\n"
        "    ]\n"
        "}\n";

    fwrite(profile_data, 1, strlen(profile_data), f);
    fclose(f);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "C"); // Sigh... stupid C
#ifdef __GLIBC__
    mallopt(M_MMAP_THRESHOLD, 65536);
#endif

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGUSR1, save_replay_handler);
    signal(SIGUSR2, toggle_pause_handler);
    signal(SIGRTMIN, toggle_replay_recording_handler);
    signal(SIGRTMIN+1, save_replay_10_seconds_handler);
    signal(SIGRTMIN+2, save_replay_30_seconds_handler);
    signal(SIGRTMIN+3, save_replay_1_minute_handler);
    signal(SIGRTMIN+4, save_replay_5_minutes_handler);
    signal(SIGRTMIN+5, save_replay_10_minutes_handler);
    signal(SIGRTMIN+6, save_replay_30_minutes_handler);

    set_display_server_environment_variables();
    install_cuda_no_stable_perf_limit();

    // Linux nvidia driver 580.105.08 added the environment variable CUDA_DISABLE_PERF_BOOST to disable the p2 power level issue,
    // where running cuda (which includes nvenc) causes the gpu to be forcefully set to p2 power level which on many nvidia gpus
    // decreases gpu performance in games. On my GTX 1080 it decreased game performance by 10% for absolutely no reason.
    // TODO: This only seems to allow the gpu to go to lower power level states, but not higher than p2.
    setenv("CUDA_DISABLE_PERF_BOOST", "1", true);
    // Stop nvidia driver from buffering frames
    setenv("__GL_MaxFramesAllowed", "1", true);
    // If this is set to 1 then cuGraphicsGLRegisterImage will fail for egl context with error: invalid OpenGL or DirectX context,
    // so we overwrite it
    setenv("__GL_THREADED_OPTIMIZATIONS", "0", true);
    // Some people set this to nvidia (for nvdec) or vdpau (for nvidia vdpau), which breaks gpu screen recorder since
    // nvidia doesn't support vaapi and nvidia-vaapi-driver doesn't support encoding yet.
    // Let vaapi find the right vaapi driver instead of forcing a specific one.
    unsetenv("LIBVA_DRIVER_NAME");
    // Some people set this to force all applications to vsync on nvidia, but this makes eglSwapBuffers never return.
    unsetenv("__GL_SYNC_TO_VBLANK");
    // Same as above, but for amd/intel
    unsetenv("vblank_mode");

    if(geteuid() == 0) {
        fprintf(stderr, "gsr error: don't run gpu-screen-recorder as the root user\n");
        _exit(1);
    }

    args_handlers arg_handlers;
    arg_handlers.version = version_command;
    arg_handlers.info = info_command;
    arg_handlers.list_audio_devices = list_audio_devices_command;
    arg_handlers.list_application_audio = list_application_audio_command;
    arg_handlers.list_v4l2_devices = list_v4l2_devices;
    arg_handlers.list_capture_options = list_capture_options_command;
    arg_handlers.list_monitors = list_monitors_command;

    args_parser arg_parser;
    if(!args_parser_parse(&arg_parser, argc, argv, &arg_handlers, NULL))
        _exit(1);

    if(!arg_parser.low_power) {
        // Forces low latency encoding mode. Use this environment variable until vaapi supports setting this as a parameter.
        // The downside of this is that it always uses maximum power, which is not ideal for replay mode that runs on system startup.
        // This option was added in mesa 24.1.4, released in july 17, 2024.
        // Seems like the performance issue is not in encoding, but rendering the frame.
        // Some frames end up taking 10 times longer. Seems to be an issue with amd gpu power management when letting the application sleep on the cpu side?
        setenv("AMD_DEBUG", "lowlatencyenc", true);
    }

    std::vector<CaptureSource> capture_sources = parse_capture_source_arg(arg_parser.capture_source, arg_parser);
    if(capture_sources.empty()) {
        fprintf(stderr, "gsr error: option -w can't be empty. You need to capture video from at least one source\n");
        args_parser_print_usage();
        _exit(1);
    }
    validate_args_with_capture_sources(arg_parser, capture_sources);

    //av_log_set_level(AV_LOG_TRACE);

    const Arg *audio_input_arg = args_parser_get_arg(&arg_parser, "-a");
    assert(audio_input_arg);

    AudioDevices audio_devices;
    if(audio_input_arg->num_values > 0)
        audio_devices = get_pulseaudio_inputs();

    std::vector<MergedAudioInputs> requested_audio_inputs = parse_audio_inputs(audio_devices, audio_input_arg);

    const bool uses_app_audio = merged_audio_inputs_has_app_audio(requested_audio_inputs);
    std::vector<std::string> app_audio_names;
#ifdef GSR_APP_AUDIO
    gsr_pipewire_audio pipewire_audio;
    memset(&pipewire_audio, 0, sizeof(pipewire_audio));
    if(uses_app_audio) {
        if(!pulseaudio_server_is_pipewire()) {
            fprintf(stderr, "gsr error: your sound server is not PipeWire. Application audio is only available when running PipeWire audio server\n");
            _exit(2);
        }

        if(!gsr_pipewire_audio_init(&pipewire_audio)) {
            fprintf(stderr, "gsr error: failed to setup PipeWire audio for application audio capture\n");
            _exit(2);
        }

        gsr_pipewire_audio_for_each_app(&pipewire_audio, [](const char *app_name, void *userdata) {
            std::vector<std::string> *app_audio_names = (std::vector<std::string>*)userdata;
            app_audio_names->push_back(app_name);
            return true;
        }, &app_audio_names);
    }
#else
    if(uses_app_audio) {
        fprintf(stderr, "gsr error: application audio can't be recorded because GPU Screen Recorder is built without application audio support (-Dapp_audio option)\n");
        _exit(2);
    }
#endif

    validate_merged_audio_inputs_app_audio(requested_audio_inputs, app_audio_names);

    const bool is_replaying = arg_parser.replay_buffer_size_secs != -1;

    bool wayland = false;
    Display *dpy = XOpenDisplay(nullptr);
    if(dpy) {
        XSelectInput(dpy, DefaultRootWindow(dpy), PropertyChangeMask);
    } else {
        wayland = true;
        fprintf(stderr, "gsr warning: failed to connect to the X server. Assuming wayland is running without Xwayland\n");
    }

    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);

    if(!wayland)
        wayland = is_xwayland(dpy);

    if(!wayland && is_using_prime_run()) {
        // Disable prime-run and similar options as it doesn't work, the monitor to capture has to be run on the same device.
        // This is fine on wayland since nvidia uses drm interface there and the monitor query checks the monitors connected
        // to the drm device.
        fprintf(stderr, "gsr warning: use of prime-run on X11 is not supported. Disabling prime-run\n");
        disable_prime_run();
    }

    gsr_window *window = gsr_window_create(dpy, wayland);
    if(!window) {
        fprintf(stderr, "gsr error: failed to create window\n");
        _exit(1);
    }

    if(is_capturing_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_PORTAL)) {
        if(is_using_prime_run()) {
            fprintf(stderr, "gsr warning: use of prime-run with -w portal option is currently not supported. Disabling prime-run\n");
            disable_prime_run();
        }

        if(video_codec_is_hdr(arg_parser.video_codec)) {
            fprintf(stderr, "gsr warning: portal capture option doesn't support hdr yet (PipeWire doesn't support hdr), the video will be tonemapped from hdr to sdr\n");
            arg_parser.video_codec = hdr_video_codec_to_sdr_video_codec(arg_parser.video_codec);
        }
    }

    gsr_egl egl;
    if(!gsr_egl_load(&egl, window, is_capturing_monitor_or_region(capture_sources), arg_parser.gl_debug)) {
        fprintf(stderr, "gsr error: failed to load opengl\n");
        _exit(1);
    }

    gsr_shader_enable_debug_output(arg_parser.gl_debug);
#ifndef NDEBUG
    gsr_shader_enable_debug_output(true);
#endif

    if(!args_parser_validate_with_gl_info(&arg_parser, &egl))
        _exit(1);

    egl.card_path[0] = '\0';
    if(monitor_capture_use_drm(window, egl.gpu_info.vendor)) {
        // TODO: Allow specifying another card, and in other places
        if(!gsr_get_valid_card_path(&egl, egl.card_path, is_capturing_monitor_or_region(capture_sources))) {
            fprintf(stderr, "gsr error: no /dev/dri/cardX device found. Make sure that you have at least one monitor connected or record a single window instead on X11 or record with the -w portal option\n");
            _exit(2);
        }
    } else {
        gsr_get_valid_card_path(&egl, egl.card_path, false);
    }

    memset(&x11_cursor, 0, sizeof(x11_cursor));
    x11_cursor_display = NULL;
    if(gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_X11 && arg_parser.record_cursor) {
        x11_cursor_display = (Display*)gsr_window_get_display(egl.window);
        gsr_cursor_init(&x11_cursor, &egl, x11_cursor_display);
    }

    // if(wayland && arg_parser.capture_source_type == GSR_CAPTURE_SOURCE_TYPE_MONITOR) {
    //     fprintf(stderr, "gsr warning: it's not possible to sync video to recorded monitor exactly on wayland when recording a monitor."
    //         " If you experience stutter in the video then record with portal capture option instead (-w portal) or use X11 instead\n");
    // }

    gsr_image_format image_format;
    if(get_image_format_from_filename(arg_parser.filename, &image_format)) {
        if(audio_input_arg->num_values > 0) {
            fprintf(stderr, "gsr error: can't record audio (-a) when taking a screenshot\n");
            _exit(1);
        }

        capture_image_to_file(arg_parser, &egl, window, image_format, capture_sources);
        _exit(0);
    }

    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, arg_parser.container_format, arg_parser.filename);
    if (!av_format_context) {
        if(arg_parser.container_format) {
            fprintf(stderr, "gsr error: Container format '%s' (argument -c) is not valid\n", arg_parser.container_format);
        } else {
            fprintf(stderr, "gsr error: Failed to deduce container format from file extension. Use the '-c' option to specify container format\n");
            args_parser_print_usage();
            _exit(1);
        }
        _exit(1);
    }

    set_format_context_options(av_format_context);

    const AVOutputFormat *output_format = av_format_context->oformat;

    std::string file_extension = output_format->extensions ? output_format->extensions : "";
    {
        size_t comma_index = file_extension.find(',');
        if(comma_index != std::string::npos)
            file_extension = file_extension.substr(0, comma_index);
    }

    const bool force_no_audio_offset = arg_parser.is_livestream || arg_parser.is_output_piped || (file_extension != "mp4" && file_extension != "mkv" && file_extension != "webm");
    const double target_fps = 1.0 / (double)arg_parser.fps;

    const bool uses_amix = merged_audio_inputs_should_use_amix(requested_audio_inputs);
    arg_parser.audio_codec = select_audio_codec_with_fallback(arg_parser.audio_codec, file_extension, uses_amix);

    vec2i video_size = {0, 0};
    std::vector<VideoSource> video_sources = create_video_sources(arg_parser, &egl, false, capture_sources, video_size);

    // (Some?) livestreaming services require at least one audio track to work.
    // If not audio is provided then create one silent audio track.
    if(arg_parser.is_livestream && requested_audio_inputs.empty()) {
        fprintf(stderr, "gsr info: live streaming but no audio track was added. Adding a silent audio track\n");
        MergedAudioInputs mai;
        mai.audio_inputs.push_back({""});
        requested_audio_inputs.push_back(std::move(mai));
    }

    AVStream *video_stream = nullptr;
    std::vector<AudioTrack> audio_tracks;

    if(arg_parser.video_encoder == GSR_VIDEO_ENCODER_HW_CPU && arg_parser.video_codec != (gsr_video_codec)GSR_VIDEO_CODEC_AUTO && arg_parser.video_codec != GSR_VIDEO_CODEC_H264) {
        fprintf(stderr, "gsr error: -encoder cpu was specified but a codec other than h264 was specified. -encoder cpu supports only h264 at the moment\n");
        _exit(1);
    }

    bool low_power = false;
    const AVCodec *video_codec_f = select_video_codec_with_fallback(video_size, &arg_parser, file_extension.c_str(), &egl, &low_power);

    const enum AVPixelFormat video_pix_fmt = get_pixel_format(arg_parser.video_codec, egl.gpu_info.vendor, arg_parser.video_encoder == GSR_VIDEO_ENCODER_HW_CPU);
    AVCodecContext *video_codec_context = create_video_codec_context(video_pix_fmt, video_codec_f, egl, arg_parser, video_size.x, video_size.y);
    if(!is_replaying)
        video_stream = create_stream(av_format_context, video_codec_context);

    AVFrame *video_frame = av_frame_alloc();
    if(!video_frame) {
        fprintf(stderr, "gsr error: Failed to allocate video frame\n");
        _exit(1);
    }
    video_frame->format = video_codec_context->pix_fmt;
    video_frame->width = video_size.x;
    video_frame->height = video_size.y;
    video_frame->color_range = video_codec_context->color_range;
    video_frame->color_primaries = video_codec_context->color_primaries;
    video_frame->color_trc = video_codec_context->color_trc;
    video_frame->colorspace = video_codec_context->colorspace;
    video_frame->chroma_location = video_codec_context->chroma_sample_location;

    const size_t estimated_replay_buffer_packets = calculate_estimated_replay_buffer_packets(arg_parser.replay_buffer_size_secs, arg_parser.fps, arg_parser.audio_codec, requested_audio_inputs);
    gsr_encoder encoder;
    if(!gsr_encoder_init(&encoder, arg_parser.replay_storage, estimated_replay_buffer_packets, arg_parser.replay_buffer_size_secs, arg_parser.filename)) {
        fprintf(stderr, "gsr error: failed to create encoder\n");
        _exit(1);
    }

    gsr_video_encoder *video_encoder = create_video_encoder(&egl, arg_parser);
    if(!video_encoder) {
        fprintf(stderr, "gsr error: failed to create video encoder\n");
        _exit(1);
    }

    if(!gsr_video_encoder_start(video_encoder, video_codec_context, video_frame)) {
        fprintf(stderr, "gsr error: failed to start video encoder\n");
        _exit(1);
    }

    video_size.x = video_codec_context->width;
    video_size.y = video_codec_context->height;
    video_sources_update_with_real_video_size(capture_sources, video_sources, video_size);

    const Arg *plugin_arg = args_parser_get_arg(&arg_parser, "-p");
    assert(plugin_arg);

    gsr_plugins plugins;
    memset(&plugins, 0, sizeof(plugins));

    if(plugin_arg->num_values > 0)
        load_plugins(&plugins, arg_parser, &egl, video_size);

    gsr_color_conversion_params color_conversion_params;
    memset(&color_conversion_params, 0, sizeof(color_conversion_params));
    color_conversion_params.color_range = arg_parser.color_range;
    color_conversion_params.egl = &egl;
    color_conversion_params.load_external_image_shader = any_video_sources_uses_external_image(video_sources);
    gsr_video_encoder_get_textures(video_encoder, color_conversion_params.destination_textures, color_conversion_params.destination_textures_size, &color_conversion_params.num_destination_textures, &color_conversion_params.destination_color);

    gsr_color_conversion color_conversion;
    if(gsr_color_conversion_init(&color_conversion, &color_conversion_params) != 0) {
        fprintf(stderr, "gsr error: main: failed to create color conversion\n");
        _exit(1);
    }

    gsr_color_conversion_clear(&color_conversion);

    gsr_color_conversion *output_color_conversion = plugins.num_plugins > 0 ? &plugins.color_conversion : &color_conversion;

    if(arg_parser.video_encoder == GSR_VIDEO_ENCODER_HW_CPU) {
        open_video_software(video_codec_context, arg_parser);
    } else {
        open_video_hardware(video_codec_context, low_power, egl, arg_parser);
    }

    if(video_stream) {
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);
        const size_t video_destination_id = gsr_encoder_add_recording_destination(&encoder, video_codec_context, av_format_context, video_stream, 0);
        if(arg_parser.write_first_frame_ts && video_destination_id != (size_t)-1) {
            std::string ts_filepath = std::string(arg_parser.filename) + ".ts";
            gsr_encoder_set_recording_destination_first_frame_ts_filepath(&encoder, video_destination_id, ts_filepath.c_str());
        }
    }

    int audio_max_frame_size = 1024;
    int audio_stream_index = VIDEO_STREAM_INDEX + 1;
    for(const MergedAudioInputs &merged_audio_inputs : requested_audio_inputs) {
        const bool use_amix = audio_inputs_should_use_amix(merged_audio_inputs.audio_inputs);
        AVCodecContext *audio_codec_context = create_audio_codec_context(arg_parser.fps, arg_parser.audio_codec, use_amix, arg_parser.audio_bitrate);

        AVStream *audio_stream = nullptr;
        if(!is_replaying) {
            audio_stream = create_stream(av_format_context, audio_codec_context);
            if(gsr_encoder_add_recording_destination(&encoder, audio_codec_context, av_format_context, audio_stream, 0) == (size_t)-1)
                fprintf(stderr, "gsr error: added too many audio sources\n");
        }

        if(audio_stream && !merged_audio_inputs.track_name.empty())
            av_dict_set(&audio_stream->metadata, "title", merged_audio_inputs.track_name.c_str(), 0);

        open_audio(audio_codec_context, arg_parser.ffmpeg_audio_opts);
        if(audio_stream)
            avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_context);

        #if LIBAVCODEC_VERSION_MAJOR < 60
        const int num_channels = audio_codec_context->channels;
        #else
        const int num_channels = audio_codec_context->ch_layout.nb_channels;
        #endif

        //audio_frame->sample_rate = audio_codec_context->sample_rate;

        std::vector<AVFilterContext*> src_filter_ctx;
        AVFilterGraph *graph = nullptr;
        AVFilterContext *sink = nullptr;
        if(use_amix) {
            int err = init_filter_graph(audio_codec_context, &graph, &sink, src_filter_ctx, merged_audio_inputs.audio_inputs.size());
            if(err < 0) {
                fprintf(stderr, "gsr error: failed to create audio filter\n");
                _exit(1);
            }
        }

        // TODO: Cleanup above

        const double audio_fps = (double)audio_codec_context->sample_rate / (double)audio_codec_context->frame_size;
        const double timeout_sec = 1000.0 / audio_fps / 1000.0;

        const double audio_startup_time_seconds = force_no_audio_offset ? 0 : audio_codec_get_desired_delay(arg_parser.audio_codec, arg_parser.fps);// * ((double)audio_codec_context->frame_size / 1024.0);
        const double num_audio_frames_shift = audio_startup_time_seconds / timeout_sec;

        std::vector<AudioDeviceData> audio_track_audio_devices;
        if(audio_inputs_has_app_audio(merged_audio_inputs.audio_inputs)) {
            assert(!use_amix);
#ifdef GSR_APP_AUDIO
            audio_track_audio_devices.push_back(create_application_audio_audio_input(merged_audio_inputs, audio_codec_context, num_channels, num_audio_frames_shift, &pipewire_audio));
#endif
        } else {
            audio_track_audio_devices = create_device_audio_inputs(merged_audio_inputs.audio_inputs, audio_codec_context, num_channels, num_audio_frames_shift, src_filter_ctx, use_amix);
        }

        AudioTrack audio_track;
        audio_track.name = merged_audio_inputs.track_name;
        audio_track.codec_context = audio_codec_context;
        audio_track.audio_devices = std::move(audio_track_audio_devices);
        audio_track.graph = graph;
        audio_track.sink = sink;
        audio_track.stream_index = audio_stream_index;
        audio_track.pts = -audio_codec_context->frame_size * num_audio_frames_shift;
        audio_tracks.push_back(std::move(audio_track));
        ++audio_stream_index;

        audio_max_frame_size = std::max(audio_max_frame_size, audio_codec_context->frame_size);
    }

    //av_dump_format(av_format_context, 0, filename, 1);

    if(!is_replaying) {
        if(!av_open_file_write_header(av_format_context, arg_parser.filename, arg_parser.ffmpeg_opts))
            _exit(1);
    }

    double fps_start_time = clock_get_monotonic_seconds();
    //double frame_timer_start = fps_start_time;
    int fps_counter = 0;
    int damage_fps_counter = 0;

    bool paused = false;
    std::atomic<double> paused_time_offset(0.0);
    double paused_time_start = 0.0;
    bool replay_recording = false;
    RecordingStartResult replay_recording_start_result;
    std::vector<size_t> replay_recording_items;
    std::string replay_recording_filepath;
    bool force_iframe_frame = false; // Only needed for video since audio frames are always iframes

    std::mutex audio_filter_mutex;

    const double record_start_time = clock_get_monotonic_seconds();

    const size_t audio_buffer_size = audio_max_frame_size * 4 * 2; // max 4 bytes/sample, 2 channels
    uint8_t *empty_audio = (uint8_t*)malloc(audio_buffer_size);
    if(!empty_audio) {
        fprintf(stderr, "gsr error: failed to create empty audio\n");
        _exit(1);
    }
    memset(empty_audio, 0, audio_buffer_size);

    for(AudioTrack &audio_track : audio_tracks) {
        for(AudioDeviceData &audio_device : audio_track.audio_devices) {
            audio_device.thread = std::thread([&]() mutable {
                const AVSampleFormat sound_device_sample_format = audio_format_to_sample_format(audio_codec_context_get_audio_format(audio_track.codec_context));
                // TODO: Always do conversion for now. This fixes issue with stuttering audio on pulseaudio with opus + multiple audio sources merged
                const bool needs_audio_conversion = true;//audio_track.codec_context->sample_fmt != sound_device_sample_format;
                SwrContext *swr = nullptr;
                if(needs_audio_conversion) {
                    swr = swr_alloc();
                    if(!swr) {
                        fprintf(stderr, "Failed to create SwrContext\n");
                        _exit(1);
                    }
                    #if LIBAVUTIL_VERSION_MAJOR <= 56
                    av_opt_set_channel_layout(swr, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                    av_opt_set_channel_layout(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                    #elif LIBAVUTIL_VERSION_MAJOR >= 59
                    av_opt_set_chlayout(swr, "in_chlayout", &audio_track.codec_context->ch_layout, 0);
                    av_opt_set_chlayout(swr, "out_chlayout", &audio_track.codec_context->ch_layout, 0);
                    #else
                    av_opt_set_chlayout(swr, "in_channel_layout", &audio_track.codec_context->ch_layout, 0);
                    av_opt_set_chlayout(swr, "out_channel_layout", &audio_track.codec_context->ch_layout, 0);
                    #endif
                    av_opt_set_int(swr, "in_sample_rate", audio_track.codec_context->sample_rate, 0);
                    av_opt_set_int(swr, "out_sample_rate", audio_track.codec_context->sample_rate, 0);
                    av_opt_set_sample_fmt(swr, "in_sample_fmt", sound_device_sample_format, 0);
                    av_opt_set_sample_fmt(swr, "out_sample_fmt", audio_track.codec_context->sample_fmt, 0);
                    swr_init(swr);
                }

                const double audio_fps = (double)audio_track.codec_context->sample_rate / (double)audio_track.codec_context->frame_size;
                const int64_t timeout_ms = std::round(1000.0 / audio_fps);
                const double timeout_sec = 1000.0 / audio_fps / 1000.0;
                bool first_frame = true;
                int64_t num_received_frames = 0;

                while(running) {
                    void *sound_buffer;
                    int sound_buffer_size = -1;
                    const double time_before_read_seconds = clock_get_monotonic_seconds();
                    if(audio_device.sound_device.handle) {
                        // TODO: use this instead of calculating time to read. But this can fluctuate and we dont want to go back in time,
                        // also it's 0.0 for some users???
                        double latency_seconds = 0.0;
                        sound_buffer_size = sound_device_read_next_chunk(&audio_device.sound_device, &sound_buffer, timeout_sec * 2.0, &latency_seconds);
                    }

                    const bool got_audio_data = sound_buffer_size >= 0;
                    //fprintf(stderr, "got audio data: %s\n", got_audio_data ? "yes" : "no");
                    //fprintf(stderr, "time to read: %f, %s, %f\n", time_to_read_seconds, got_audio_data ? "yes" : "no", timeout_sec);
                    const double this_audio_frame_time = clock_get_monotonic_seconds() - paused_time_offset;

                    if(paused) {
                        if(!audio_device.sound_device.handle)
                            av_usleep(timeout_ms * 1000);

                        continue;
                    }

                    int ret = av_frame_make_writable(audio_device.frame);
                    if (ret < 0) {
                        fprintf(stderr, "Failed to make audio frame writable\n");
                        break;
                    }

                    // TODO: Is this |received_audio_time| really correct?
                    const int64_t num_expected_frames = std::floor((this_audio_frame_time - record_start_time) / timeout_sec);
                    int64_t num_missing_frames = std::max((int64_t)0LL, num_expected_frames - num_received_frames);

                    if(got_audio_data)
                        num_missing_frames = std::max((int64_t)0LL, num_missing_frames - 1);

                    if(!audio_device.sound_device.handle)
                        num_missing_frames = std::max((int64_t)1, num_missing_frames);

                    // Fucking hell is there a better way to do this? I JUST WANT TO KEEP VIDEO AND AUDIO SYNCED HOLY FUCK I WANT TO KILL MYSELF NOW.
                    // THIS PIECE OF SHIT WANTS EMPTY FRAMES OTHERWISE VIDEO PLAYS TOO FAST TO KEEP UP WITH AUDIO OR THE AUDIO PLAYS TOO EARLY.
                    // BUT WE CANT USE DELAYS TO GIVE DUMMY DATA BECAUSE PULSEAUDIO MIGHT GIVE AUDIO A BIG DELAYED!!!
                    // This garbage is needed because we want to produce constant frame rate videos instead of variable frame rate
                    // videos because bad software such as video editing software and VLC do not support variable frame rate software,
                    // despite nvidia shadowplay and xbox game bar producing variable frame rate videos.
                    // So we have to make sure we produce frames at the same relative rate as the video.
                    if((num_missing_frames >= 1 && got_audio_data) || num_missing_frames >= 5 || !audio_device.sound_device.handle) {
                        // TODO:
                        //audio_track.frame->data[0] = empty_audio;
                        if(first_frame || num_missing_frames >= 5) {
                            if(needs_audio_conversion)
                                swr_convert(swr, &audio_device.frame->data[0], audio_track.codec_context->frame_size, (const uint8_t**)&empty_audio, audio_track.codec_context->frame_size);
                            else
                                audio_device.frame->data[0] = empty_audio;
                        }
                        first_frame = false;

                        // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
                        std::lock_guard<std::mutex> lock(audio_filter_mutex);
                        for(int i = 0; i < num_missing_frames; ++i) {
                            if(audio_track.graph) {
                                // TODO: av_buffersrc_add_frame
                                if(av_buffersrc_write_frame(audio_device.src_filter_ctx, audio_device.frame) < 0) {
                                    fprintf(stderr, "gsr error: failed to add audio frame to filter\n");
                                }
                            } else {
                                ret = avcodec_send_frame(audio_track.codec_context, audio_device.frame);
                                if(ret >= 0) {
                                    // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                    gsr_encoder_receive_packets(&encoder, audio_track.codec_context, audio_device.frame->pts, audio_track.stream_index);
                                } else {
                                    fprintf(stderr, "Failed to encode audio!\n");
                                }
                                audio_track.pts += audio_track.codec_context->frame_size;
                            }

                            audio_device.frame->pts += audio_track.codec_context->frame_size;
                            num_received_frames++;
                        }
                    }

                    if(!audio_device.sound_device.handle) {
                        av_usleep(timeout_ms * 1000);
                    } else if(got_audio_data) {
                        // TODO: Instead of converting audio, get float audio from alsa. Or does alsa do conversion internally to get this format?
                        if(needs_audio_conversion)
                            swr_convert(swr, &audio_device.frame->data[0], audio_track.codec_context->frame_size, (const uint8_t**)&sound_buffer, audio_track.codec_context->frame_size);
                        else
                            audio_device.frame->data[0] = (uint8_t*)sound_buffer;
                        first_frame = false;

                        std::lock_guard<std::mutex> lock(audio_filter_mutex);

                        if(audio_track.graph) {
                            // TODO: av_buffersrc_add_frame
                            if(av_buffersrc_write_frame(audio_device.src_filter_ctx, audio_device.frame) < 0) {
                                fprintf(stderr, "gsr error: failed to add audio frame to filter\n");
                            }
                        } else {
                            ret = avcodec_send_frame(audio_track.codec_context, audio_device.frame);
                            if(ret >= 0) {
                                // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                gsr_encoder_receive_packets(&encoder, audio_track.codec_context, audio_device.frame->pts, audio_track.stream_index);
                            } else {
                                fprintf(stderr, "Failed to encode audio!\n");
                            }
                            audio_track.pts += audio_track.codec_context->frame_size;
                        }

                        audio_device.frame->pts += audio_track.codec_context->frame_size;
                        num_received_frames++;
                    } else {
                        // TODO: Maybe sleep for time_to_sleep_until_next_frame/4? for better latency
                        const double time_after_read_seconds = clock_get_monotonic_seconds();
                        const double time_to_read_seconds = time_after_read_seconds - time_before_read_seconds;
                        const double time_to_sleep_until_next_frame = timeout_sec - time_to_read_seconds;
                        if(time_to_sleep_until_next_frame > 0.0)
                            av_usleep(time_to_sleep_until_next_frame * 1000ULL * 1000ULL);
                    }
                }

                if(swr)
                    swr_free(&swr);
            });
        }
    }

    std::thread amix_thread;
    if(uses_amix) {
        amix_thread = std::thread([&]() {
            AVFrame *aframe = av_frame_alloc();
            while(running) {
                {
                    std::lock_guard<std::mutex> lock(audio_filter_mutex);
                    for(AudioTrack &audio_track : audio_tracks) {
                        if(!audio_track.sink)
                            continue;

                        int err = 0;
                        while ((err = av_buffersink_get_frame(audio_track.sink, aframe)) >= 0) {
                            aframe->pts = audio_track.pts;
                            err = avcodec_send_frame(audio_track.codec_context, aframe);
                            if(err >= 0){
                                // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                gsr_encoder_receive_packets(&encoder, audio_track.codec_context, aframe->pts, audio_track.stream_index);
                            } else {
                                fprintf(stderr, "Failed to encode audio!\n");
                            }
                            av_frame_unref(aframe);
                            audio_track.pts += audio_track.codec_context->frame_size;
                        }
                    }
                }
                av_usleep(5 * 1000); // 5 milliseconds
            }
            av_frame_free(&aframe);
        });
    }

    // Set update_fps to 24 to test if duplicate/delayed frames cause video/audio desync or too fast/slow video.
    //const double update_fps = fps + 190;
    bool should_stop_error = false;

    int64_t video_pts_counter = 0;
    int64_t video_prev_pts = 0;

    bool hdr_metadata_set = false;
    const bool hdr = video_codec_is_hdr(arg_parser.video_codec);

    bool use_damage_tracking = false;
    gsr_damage damage;
    memset(&damage, 0, sizeof(damage));
    if(arg_parser.framerate_mode == GSR_FRAMERATE_MODE_CONTENT && is_capturing_damage_tracked_target(capture_sources)) {
        if(gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_X11) {
            gsr_damage_init(&damage, &egl, &x11_cursor, arg_parser.record_cursor);
            use_damage_tracking = true;

            for(const CaptureSource &capture_source : capture_sources) {
                switch(capture_source.type) {
                    case GSR_CAPTURE_SOURCE_TYPE_WINDOW:
                        gsr_damage_start_tracking_window(&damage, capture_source.window_id);
                        break;
                    case GSR_CAPTURE_SOURCE_TYPE_MONITOR:
                    case GSR_CAPTURE_SOURCE_TYPE_REGION:
                        // TODO: When capturing a region only track damage in that region
                        gsr_damage_start_tracking_monitor(&damage, capture_source.name.c_str());
                        break;
                    default:
                        break;
                }
            }
        } else if(is_capturing_monitor_or_region(capture_sources)) {
            fprintf(stderr, "gsr warning: \"-fm content\" has no effect on Wayland when recording a monitor. Either record a monitor on X11 or capture with desktop portal instead (-w portal)\n");
        }
    }

    while(running) {
        while(gsr_window_process_event(window)) {
            if(x11_cursor_display && arg_parser.record_cursor)
                gsr_cursor_on_event(&x11_cursor, gsr_window_get_event_data(window));

            gsr_damage_on_event(&damage, gsr_window_get_event_data(window));
            for(VideoSource &video_source : video_sources) {
                gsr_capture_on_event(video_source.capture, &egl);
            }
        }

        if(x11_cursor_display && arg_parser.record_cursor)
            gsr_cursor_tick(&x11_cursor, DefaultRootWindow(x11_cursor_display));

        gsr_damage_tick(&damage);

        should_stop_error = false;
        bool damaged = false;

        if(use_damage_tracking)
            damaged = gsr_damage_is_damaged(&damage);

        for(VideoSource &video_source : video_sources) {
            gsr_capture_tick(video_source.capture);

            if(gsr_capture_should_stop(video_source.capture, &should_stop_error)) {
                running = 0;
                break;
            }

            if(video_source.capture_source->type == GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW) {
                assert(video_source.capture->get_window_id);
                const Window damage_target_window = video_source.capture->get_window_id(video_source.capture);

                if((int64_t)damage_target_window != video_source.capture_source->window_id) {
                    gsr_damage_stop_tracking_window(&damage, video_source.capture_source->window_id);
                    if(damage_target_window != 0)
                        gsr_damage_start_tracking_window(&damage, damage_target_window);
                }

                video_source.capture_source->window_id = damage_target_window;
            }

            if(video_source.capture->is_damaged)
                damaged |= video_source.capture->is_damaged(video_source.capture);
            else if(!use_damage_tracking)
                damaged = true;
        }

        damaged |= gsr_plugins_is_damaged(&plugins);

        // TODO: Readd wayland sync warning when removing this
        if(arg_parser.framerate_mode != GSR_FRAMERATE_MODE_CONTENT)
            damaged = true;

        if(damaged)
            ++damage_fps_counter;

        ++fps_counter;
        const double time_now = clock_get_monotonic_seconds();
        //const double frame_timer_elapsed = time_now - frame_timer_start;
        const double elapsed = time_now - fps_start_time;
        if (elapsed >= 1.0) {
            if(arg_parser.verbose) {
                fprintf(stderr, "update fps: %d, damage fps: %d\n", fps_counter, damage_fps_counter);
            }
            fps_start_time = time_now;
            fps_counter = 0;
            damage_fps_counter = 0;
        }

        const double this_video_frame_time = clock_get_monotonic_seconds() - paused_time_offset;
        const int64_t expected_frames = std::floor((this_video_frame_time - record_start_time) / target_fps);
        const int64_t num_missed_frames = expected_frames - video_pts_counter;

        if(damaged && num_missed_frames >= 1 && !paused) {
            // TODO: Dont do this if no damage?
            egl.glClear(0);

            gsr_damage_clear(&damage);
            gsr_plugins_clear_damage(&plugins);
            gsr_capture_kms_cleanup_kms_fds();

            if(kms_client_initialized) {
                if(gsr_kms_client_get_kms(&kms_client, &kms_response) != 0)
                    fprintf(stderr, "gsr error: failed to get kms, error: %d (%s)\n", kms_response.result, kms_response.err_msg);
            }

            bool capture_has_synchronous_task = false;
            for(VideoSource &video_source : video_sources) {
                if(video_source.capture->clear_damage)
                    video_source.capture->clear_damage(video_source.capture);

                if(video_source.capture->capture_has_synchronous_task) {
                    capture_has_synchronous_task = video_source.capture->capture_has_synchronous_task(video_source.capture);
                    if(capture_has_synchronous_task) {
                        paused_time_start = clock_get_monotonic_seconds();
                        paused = true;
                    }
                }
            }

            for(VideoSource &video_source : video_sources) {
                if(video_source.capture->pre_capture)
                    video_source.capture->pre_capture(video_source.capture, &video_source.metadata, output_color_conversion);
            }

            if(output_color_conversion->schedule_clear) {
                output_color_conversion->schedule_clear = false;
                gsr_color_conversion_clear(output_color_conversion);
            }

            for(VideoSource &video_source : video_sources) {
                gsr_capture_capture(video_source.capture, &video_source.metadata, output_color_conversion);
            }

            gsr_capture_kms_cleanup_kms_fds();

            if(plugins.num_plugins > 0) {
                gsr_plugins_draw(&plugins);
                gsr_color_conversion_draw(&color_conversion, plugins.texture,
                    {0, 0}, video_size,
                    {0, 0}, video_size,
                    video_size, GSR_ROT_0, GSR_FLIP_NONE, GSR_SOURCE_COLOR_RGB, false);
            }

            if(capture_has_synchronous_task) {
                paused_time_offset = paused_time_offset + (clock_get_monotonic_seconds() - paused_time_start);
                paused = false;
            }

            gsr_egl_swap_buffers(&egl);
            gsr_video_encoder_copy_textures_to_frame(video_encoder, video_frame, output_color_conversion);

            for(VideoSource &video_source : video_sources) {
                if(hdr && !hdr_metadata_set && !is_replaying && add_hdr_metadata_to_video_stream(video_source.capture, video_stream))
                    hdr_metadata_set = true;
            }

            // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
            const int num_frames_to_encode = arg_parser.framerate_mode == GSR_FRAMERATE_MODE_CONSTANT ? num_missed_frames : 1;
            for(int i = 0; i < num_frames_to_encode; ++i) {
                if(arg_parser.framerate_mode == GSR_FRAMERATE_MODE_CONSTANT) {
                    video_frame->pts = video_pts_counter + i;
                } else {
                    video_frame->pts = (this_video_frame_time - record_start_time) * (double)AV_TIME_BASE;
                    const bool same_pts = video_frame->pts == video_prev_pts;
                    video_prev_pts = video_frame->pts;
                    if(same_pts)
                        continue;
                }

                if(force_iframe_frame) {
                    video_frame->pict_type = AV_PICTURE_TYPE_I;
                }

                int ret = avcodec_send_frame(video_codec_context, video_frame);
                if(ret == 0) {
                    // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                    gsr_encoder_receive_packets(&encoder, video_codec_context, video_frame->pts, VIDEO_STREAM_INDEX);
                } else {
                    fprintf(stderr, "gsr error: avcodec_send_frame failed, error: %s\n", av_error_to_string(ret));
                }

                if(force_iframe_frame) {
                    force_iframe_frame = false;
                    video_frame->pict_type = AV_PICTURE_TYPE_NONE;
                }
            }

            video_pts_counter += num_missed_frames;
        }

        if(toggle_pause == 1 && !is_replaying) {
            const bool new_paused_state = !paused;
            if(new_paused_state) {
                paused_time_start = clock_get_monotonic_seconds();
                fprintf(stderr, "Paused\n");
            } else {
                paused_time_offset = paused_time_offset + (clock_get_monotonic_seconds() - paused_time_start);
                fprintf(stderr, "Unpaused\n");
            }

            toggle_pause = 0;
            paused = !paused;
        }

        if(toggle_replay_recording && !arg_parser.replay_recording_directory) {
            toggle_replay_recording = 0;
            printf("gsr error: Unable to start recording since the -ro option was not specified\n");
            fflush(stdout);
        }

        if(toggle_replay_recording && arg_parser.replay_recording_directory) {
            toggle_replay_recording = 0;
            const bool new_replay_recording_state = !replay_recording;
            if(new_replay_recording_state) {
                std::lock_guard<std::mutex> lock(audio_filter_mutex);
                replay_recording_items.clear();
                replay_recording_filepath = create_new_recording_filepath_from_timestamp(arg_parser.replay_recording_directory, "Video", file_extension, arg_parser.date_folders);
                replay_recording_start_result = start_recording_create_streams(replay_recording_filepath.c_str(), arg_parser, video_codec_context, audio_tracks, hdr, video_sources);
                if(replay_recording_start_result.av_format_context) {
                    const size_t video_recording_destination_id = gsr_encoder_add_recording_destination(&encoder, video_codec_context, replay_recording_start_result.av_format_context, replay_recording_start_result.video_stream, video_frame->pts);
                    if(arg_parser.write_first_frame_ts && video_recording_destination_id != (size_t)-1) {
                        std::string ts_filepath = replay_recording_filepath + ".ts";
                        gsr_encoder_set_recording_destination_first_frame_ts_filepath(&encoder, video_recording_destination_id, ts_filepath.c_str());
                    }

                    if(video_recording_destination_id != (size_t)-1)
                        replay_recording_items.push_back(video_recording_destination_id);

                    for(const auto &audio_input : replay_recording_start_result.audio_inputs) {
                        const size_t audio_recording_destination_id = gsr_encoder_add_recording_destination(&encoder, audio_input.audio_track->codec_context, replay_recording_start_result.av_format_context, audio_input.stream, audio_input.audio_track->pts);
                        if(audio_recording_destination_id != (size_t)-1)
                            replay_recording_items.push_back(audio_recording_destination_id);
                    }

                    replay_recording = true;
                    force_iframe_frame = true;
                    fprintf(stderr, "Started recording\n");
                } else {
                    printf("gsr error: Failed to start recording\n");
                    fflush(stdout);
                }
            } else if(replay_recording_start_result.av_format_context) {
                for(size_t id : replay_recording_items) {
                    gsr_encoder_remove_recording_destination(&encoder, id);
                }
                replay_recording_items.clear();

                if(stop_recording_close_streams(replay_recording_start_result.av_format_context)) {
                    fprintf(stderr, "Stopped recording\n");
                    puts(replay_recording_filepath.c_str());
                    fflush(stdout);
                    if(arg_parser.recording_saved_script)
                        run_recording_saved_script_async(arg_parser.recording_saved_script, replay_recording_filepath.c_str(), "regular");
                } else {
                    printf("gsr error: Failed to save recording\n");
                    fflush(stdout);
                }

                replay_recording_start_result = RecordingStartResult{};
                replay_recording = false;
                replay_recording_filepath.clear();
            }
        }

        if(save_replay_thread.valid() && save_replay_thread.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            save_replay_thread.get();
            if(save_replay_output_filepath.empty()) {
                printf("gsr error: Failed to save replay\n");
                fflush(stdout);
            } else {
                puts(save_replay_output_filepath.c_str());
                fflush(stdout);
                if(arg_parser.recording_saved_script)
                    run_recording_saved_script_async(arg_parser.recording_saved_script, save_replay_output_filepath.c_str(), "replay");
            }
        }

        if(save_replay_seconds != 0 && !save_replay_thread.valid() && is_replaying) {
            int current_save_replay_seconds = save_replay_seconds;
            if(current_save_replay_seconds > 0)
                current_save_replay_seconds += arg_parser.keyint;

            save_replay_seconds = 0;
            save_replay_output_filepath.clear();
            save_replay_async(video_codec_context, VIDEO_STREAM_INDEX, audio_tracks, &encoder, arg_parser, file_extension, arg_parser.date_folders, hdr, video_sources, current_save_replay_seconds);

            if(arg_parser.restart_replay_on_save && current_save_replay_seconds == save_replay_seconds_full) {
                pthread_mutex_lock(&encoder.replay_mutex);
                gsr_replay_buffer_clear(encoder.replay_buffer);
                pthread_mutex_unlock(&encoder.replay_mutex);
            }
        }

        const double time_at_frame_end = clock_get_monotonic_seconds() - paused_time_offset;
        const double time_elapsed_total = time_at_frame_end - record_start_time;
        const int64_t frames_elapsed = std::floor(time_elapsed_total / target_fps);
        const double time_at_next_frame = (frames_elapsed + 1) * target_fps;
        double time_to_next_frame = time_at_next_frame - time_elapsed_total;
        if(time_to_next_frame > target_fps)
            time_to_next_frame = target_fps;
        const int64_t end_num_missed_frames = frames_elapsed - video_pts_counter;

        if(time_to_next_frame > 0.0 && end_num_missed_frames <= 0)
            av_usleep(time_to_next_frame * 1000.0 * 1000.0);
        else {
            if(paused)
                av_usleep(20.0 * 1000.0); // 20 milliseconds
            else if(arg_parser.framerate_mode == GSR_FRAMERATE_MODE_CONTENT)
                av_usleep(2.8 * 1000.0); // 2.8 milliseconds
        }
    }

    running = 0;

    if(save_replay_thread.valid()) {
        save_replay_thread.get();
        if(save_replay_output_filepath.empty()) {
            // TODO: Output failed to save
        } else {
            puts(save_replay_output_filepath.c_str());
            fflush(stdout);
            if(arg_parser.recording_saved_script)
                run_recording_saved_script_async(arg_parser.recording_saved_script, save_replay_output_filepath.c_str(), "replay");
        }
    }

    gsr_plugins_deinit(&plugins);

    if(replay_recording_start_result.av_format_context) {
        for(size_t id : replay_recording_items) {
            gsr_encoder_remove_recording_destination(&encoder, id);
        }
        replay_recording_items.clear();

        if(stop_recording_close_streams(replay_recording_start_result.av_format_context)) {
            fprintf(stderr, "Stopped recording\n");
            puts(replay_recording_filepath.c_str());
            fflush(stdout);
            if(arg_parser.recording_saved_script)
                run_recording_saved_script_async(arg_parser.recording_saved_script, replay_recording_filepath.c_str(), "regular");
        } else {
            printf("gsr error: Failed to save recording\n");
            fflush(stdout);
        }
    }

    for(AudioTrack &audio_track : audio_tracks) {
        for(auto &audio_device : audio_track.audio_devices) {
            audio_device.thread.join();
            sound_device_close(&audio_device.sound_device);
        }
    }

    if(amix_thread.joinable())
        amix_thread.join();

    // TODO: Replace this with start_recording_create_steams
    if(!is_replaying && av_write_trailer(av_format_context) != 0) {
        //fprintf(stderr, "Failed to write trailer\n");
    }

    if(!is_replaying) {
        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);
    }

    gsr_cursor_deinit(&x11_cursor);
    gsr_damage_deinit(&damage);
    gsr_color_conversion_deinit(&color_conversion);
    gsr_video_encoder_destroy(video_encoder, video_codec_context);
    gsr_encoder_deinit(&encoder);
    for(VideoSource &video_source : video_sources) {
        gsr_capture_destroy(video_source.capture);
    }
#ifdef GSR_APP_AUDIO
    gsr_pipewire_audio_deinit(&pipewire_audio);
#endif
    if(kms_client_initialized) {
        gsr_capture_kms_cleanup_kms_fds();
        gsr_kms_client_deinit(&kms_client);
    }

    if(!is_replaying && arg_parser.recording_saved_script)
        run_recording_saved_script_async(arg_parser.recording_saved_script, arg_parser.filename, "regular");

    if(dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(dpy);
    }

    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);

    //av_frame_free(&video_frame);
    free(empty_audio);
    args_parser_deinit(&arg_parser);
    // We do an _exit here because cuda uses at_exit to do _something_ that causes the program to freeze,
    // but only on some nvidia driver versions on some gpus (RTX?), and _exit exits the program without calling
    // the at_exit registered functions.
    // Cuda (cuvid library in this case) seems to be waiting for a thread that never finishes execution.
    // Maybe this happens because we dont clean up all ffmpeg resources?
    // TODO: Investigate this.
    _exit(should_stop_error ? 3 : 0);
}
