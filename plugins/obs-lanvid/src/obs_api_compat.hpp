#pragma once
// obs_api_compat.hpp — Includes real OBS Studio SDK headers when LV_HAVE_REAL_LIBOBS is defined,
// or provides a clean, ABI-faithful headless mock of libobs / obs-frontend-api for CI & unit tests.

#if defined(LV_HAVE_REAL_LIBOBS)
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#else

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obs_source obs_source_t;
typedef struct obs_output obs_output_t;
typedef struct obs_data obs_data_t;
typedef struct obs_properties obs_properties_t;
typedef struct obs_property obs_property_t;
typedef struct signal_handler signal_handler_t;

enum obs_source_type {
    OBS_SOURCE_TYPE_INPUT = 0,
    OBS_SOURCE_TYPE_FILTER = 1,
    OBS_SOURCE_TYPE_TRANSITION = 2,
    OBS_SOURCE_TYPE_SCENE = 3,
};

#define OBS_SOURCE_VIDEO (1 << 0)
#define OBS_SOURCE_AUDIO (1 << 1)
#define OBS_SOURCE_ASYNC (1 << 2)
#define OBS_SOURCE_ASYNC_VIDEO (OBS_SOURCE_ASYNC | OBS_SOURCE_VIDEO)
#define OBS_SOURCE_DO_NOT_DUPLICATE (1 << 4)

#define OBS_OUTPUT_VIDEO (1 << 0)
#define OBS_OUTPUT_AUDIO (1 << 1)
#define OBS_OUTPUT_AV (OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO)

enum video_format {
    VIDEO_FORMAT_NONE = 0,
    VIDEO_FORMAT_I420 = 1,
    VIDEO_FORMAT_NV12 = 2,
    VIDEO_FORMAT_BGRA = 5,
};

enum video_colorspace {
    VIDEO_CS_DEFAULT = 0,
    VIDEO_CS_601 = 1,
    VIDEO_CS_709 = 2,
    VIDEO_CS_2100_PQ = 4,
    VIDEO_CS_2100_HLG = 5,
};

enum video_range_type {
    VIDEO_RANGE_DEFAULT = 0,
    VIDEO_RANGE_PARTIAL = 1,
    VIDEO_RANGE_FULL = 2,
};

enum audio_format {
    AUDIO_FORMAT_FLOAT_PLANAR = 8,
};

enum speaker_layout {
    SPEAKERS_MONO = 1,
    SPEAKERS_STEREO = 2,
};

struct obs_source_frame {
    uint8_t* data[8];
    uint32_t linesize[8];
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;
    enum video_format format;
    float color_matrix[16];
    bool full_range;
    float color_range_min[3];
    float color_range_max[3];
    bool flip;
    uint8_t flags;
};

struct obs_source_audio {
    const uint8_t* data[8];
    uint32_t frames;
    enum speaker_layout speakers;
    enum audio_format format;
    uint32_t samples_per_sec;
    uint64_t timestamp;
};

struct video_data {
    uint8_t* data[8];
    uint32_t linesize[8];
    uint64_t timestamp;
};

struct audio_data {
    uint8_t* data[8];
    uint32_t frames;
    uint64_t timestamp;
};

enum obs_frontend_event {
    OBS_FRONTEND_EVENT_STREAMING_STARTED = 1,
    OBS_FRONTEND_EVENT_STREAMING_STOPPED = 3,
    OBS_FRONTEND_EVENT_RECORDING_STARTED = 5,
    OBS_FRONTEND_EVENT_RECORDING_STOPPED = 7,
    OBS_FRONTEND_EVENT_SCENE_CHANGED = 8,
    OBS_FRONTEND_EVENT_EXIT = 16,
    OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED = 25,
    OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED = 26,
    OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED = 27,
};

struct obs_source_info {
    const char* id;
    enum obs_source_type type;
    uint32_t output_flags;
    const char* (*get_name)(void* type_data);
    void* (*create)(obs_data_t* settings, obs_source_t* source);
    void (*destroy)(void* data);
    void (*get_defaults)(obs_data_t* settings);
    obs_properties_t* (*get_properties)(void* data);
    void (*update)(void* data, obs_data_t* settings);
    void (*activate)(void* data);
    void (*deactivate)(void* data);
    struct obs_source_frame* (*filter_video)(void* data, struct obs_source_frame* frame);
};

struct obs_output_info {
    const char* id;
    uint32_t flags;
    const char* (*get_name)(void* type_data);
    void* (*create)(obs_data_t* settings, obs_output_t* output);
    void (*destroy)(void* data);
    bool (*start)(void* data);
    void (*stop)(void* data, uint64_t ts);
    void (*raw_video)(void* data, struct video_data* frame);
    void (*raw_audio)(void* data, struct audio_data* frames);
    void (*get_defaults)(obs_data_t* settings);
    obs_properties_t* (*get_properties)(void* data);
};

#ifdef __cplusplus
}
#endif

#endif  // LV_HAVE_REAL_LIBOBS
