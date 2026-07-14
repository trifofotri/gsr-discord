#include "../../include/window/wayland.h"
#include "../../include/wayland_host_bridge.h"

#include "../../include/vec2.h"
#include "../../include/defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "kde-output-device-v2-client-protocol.h"

#define GSR_MAX_OUTPUTS 32

typedef struct gsr_window_wayland gsr_window_wayland;

typedef struct {
    uint32_t wl_name;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    vec2i pos;
    vec2i size;
    vec2i logical_size;
    int32_t transform;
    char *name;
} gsr_wayland_output;

#define KDE_PLASMA_ASSUMED_MONITOR_PEAK_LUMINANCE 800.0f
#define KDE_BRIGHTNESS_MULTIPLIER_MAX 10000

typedef struct {
    struct kde_output_device_v2 *device;
    char *name;
    bool hdr_enabled;
    uint32_t sdr_brightness;              /* cd/m² (nits) */
    uint32_t max_peak_brightness;         /* cd/m² (nits) */
    int32_t max_peak_brightness_override; /* cd/m² (nits), -1 if not set */
    uint32_t brightness;                  /* 0-10000 */
    uint32_t dimming;                     /* 0-10000 */
} gsr_kde_output_device;

struct gsr_window_wayland {
    struct wl_display *display;
    struct wl_egl_window *window;
    struct wl_registry *registry;
    struct wl_surface *surface;
    struct wl_compositor *compositor;
    gsr_wayland_output outputs[GSR_MAX_OUTPUTS];
    int num_outputs;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct kde_output_device_registry_v2 *kde_output_device_registry;
    gsr_kde_output_device kde_output_devices[GSR_MAX_OUTPUTS];
};

static void output_handle_geometry(void *data, struct wl_output *wl_output,
        int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
        int32_t subpixel, const char *make, const char *model,
        int32_t transform) {
    (void)wl_output;
    (void)phys_width;
    (void)phys_height;
    (void)subpixel;
    (void)make;
    (void)model;
    gsr_wayland_output *gsr_output = data;
    gsr_output->pos.x = x;
    gsr_output->pos.y = y;
    gsr_output->transform = transform;
}

static void output_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)wl_output;
    (void)flags;
    (void)refresh;
    gsr_wayland_output *gsr_output = data;
    gsr_output->size.x = width;
    gsr_output->size.y = height;
}

static void output_handle_done(void *data, struct wl_output *wl_output) {
    (void)data;
    (void)wl_output;
}

static void output_handle_scale(void* data, struct wl_output *wl_output, int32_t factor) {
    (void)data;
    (void)wl_output;
    (void)factor;
}

static void output_handle_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)wl_output;
    gsr_wayland_output *gsr_output = data;
    if(gsr_output->name) {
        free(gsr_output->name);
        gsr_output->name = NULL;
    }
    gsr_output->name = strdup(name);
}

static void output_handle_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)data;
    (void)wl_output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .name = output_handle_name,
    .description = output_handle_description,
};

static void kde_output_device_free(gsr_kde_output_device *device) {
    if(device->device) {
        kde_output_device_v2_destroy(device->device);
        device->device = NULL;
    }

    if(device->name) {
        free(device->name);
        device->name = NULL;
    }
}

static void kde_output_device_handle_geometry(void *data, struct kde_output_device_v2 *device, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
    int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data; (void)device; (void)x; (void)y; (void)physical_width; (void)physical_height; (void)subpixel; (void)make; (void)model; (void)transform;
}

static void kde_output_device_handle_current_mode(void *data, struct kde_output_device_v2 *device, struct kde_output_device_mode_v2 *mode) {
    (void)data; (void)device; (void)mode;
}

static void kde_output_device_handle_mode(void *data, struct kde_output_device_v2 *device, struct kde_output_device_mode_v2 *mode) {
    (void)data; (void)device;
    kde_output_device_mode_v2_destroy(mode);
}

static void kde_output_device_handle_done(void *data, struct kde_output_device_v2 *device) {
    (void)data; (void)device;
}

static void kde_output_device_handle_scale(void *data, struct kde_output_device_v2 *device, wl_fixed_t factor) {
    (void)data; (void)device; (void)factor;
}

static void kde_output_device_handle_edid(void *data, struct kde_output_device_v2 *device, const char *raw) {
    (void)data; (void)device; (void)raw;
}

static void kde_output_device_handle_enabled(void *data, struct kde_output_device_v2 *device, int32_t enabled) {
    (void)data; (void)device; (void)enabled;
}

static void kde_output_device_handle_string_noop(void *data, struct kde_output_device_v2 *device, const char *value) {
    (void)data; (void)device; (void)value;
}

static void kde_output_device_handle_uint_noop(void *data, struct kde_output_device_v2 *device, uint32_t value) {
    (void)data; (void)device; (void)value;
}

static void kde_output_device_handle_name(void *data, struct kde_output_device_v2 *device, const char *name) {
    (void)device;
    gsr_kde_output_device *gsr_device = data;
    if(gsr_device->name) {
        free(gsr_device->name);
        gsr_device->name = NULL;
    }
    gsr_device->name = strdup(name);
}

static void kde_output_device_handle_high_dynamic_range(void *data, struct kde_output_device_v2 *device, uint32_t hdr_enabled) {
    (void)device;
    gsr_kde_output_device *gsr_device = data;
    gsr_device->hdr_enabled = hdr_enabled != 0;
}

static void kde_output_device_handle_sdr_brightness(void *data, struct kde_output_device_v2 *device, uint32_t sdr_brightness) {
    (void)device;
    gsr_kde_output_device *gsr_device = data;
    gsr_device->sdr_brightness = sdr_brightness;
}

static void kde_output_device_handle_brightness_metadata(void *data, struct kde_output_device_v2 *device, uint32_t max_peak_brightness, uint32_t max_frame_average_brightness, uint32_t min_brightness) {
    (void)device; (void)max_frame_average_brightness; (void)min_brightness;
    gsr_kde_output_device *gsr_device = data;
    gsr_device->max_peak_brightness = max_peak_brightness;
}

static void kde_output_device_handle_brightness_overrides(void *data, struct kde_output_device_v2 *device, int32_t max_peak_brightness, int32_t max_average_brightness, int32_t min_brightness) {
    (void)device; (void)max_average_brightness; (void)min_brightness;
    gsr_kde_output_device *gsr_device = data;
    gsr_device->max_peak_brightness_override = max_peak_brightness;
}

static void kde_output_device_handle_brightness(void *data, struct kde_output_device_v2 *device, uint32_t brightness) {
    (void)device;
    gsr_kde_output_device *gsr_device = data;
    gsr_device->brightness = brightness;
}

static void kde_output_device_handle_dimming(void *data, struct kde_output_device_v2 *device, uint32_t multiplier) {
    (void)device;
    gsr_kde_output_device *gsr_device = data;
    gsr_device->dimming = multiplier;
}

static void kde_output_device_handle_max_bits_per_color_range(void *data, struct kde_output_device_v2 *device, uint32_t min_value, uint32_t max_value) {
    (void)data; (void)device; (void)min_value; (void)max_value;
}

static void kde_output_device_handle_removed(void *data, struct kde_output_device_v2 *device) {
    (void)device;
    gsr_kde_output_device *gsr_device = data;
    kde_output_device_free(gsr_device);
}

static const struct kde_output_device_v2_listener kde_output_device_listener = {
    .geometry = kde_output_device_handle_geometry,
    .current_mode = kde_output_device_handle_current_mode,
    .mode = kde_output_device_handle_mode,
    .done = kde_output_device_handle_done,
    .scale = kde_output_device_handle_scale,
    .edid = kde_output_device_handle_edid,
    .enabled = kde_output_device_handle_enabled,
    .uuid = kde_output_device_handle_string_noop,
    .serial_number = kde_output_device_handle_string_noop,
    .eisa_id = kde_output_device_handle_string_noop,
    .capabilities = kde_output_device_handle_uint_noop,
    .overscan = kde_output_device_handle_uint_noop,
    .vrr_policy = kde_output_device_handle_uint_noop,
    .rgb_range = kde_output_device_handle_uint_noop,
    .name = kde_output_device_handle_name,
    .high_dynamic_range = kde_output_device_handle_high_dynamic_range,
    .sdr_brightness = kde_output_device_handle_sdr_brightness,
    .wide_color_gamut = kde_output_device_handle_uint_noop,
    .auto_rotate_policy = kde_output_device_handle_uint_noop,
    .icc_profile_path = kde_output_device_handle_string_noop,
    .brightness_metadata = kde_output_device_handle_brightness_metadata,
    .brightness_overrides = kde_output_device_handle_brightness_overrides,
    .sdr_gamut_wideness = kde_output_device_handle_uint_noop,
    .color_profile_source = kde_output_device_handle_uint_noop,
    .brightness = kde_output_device_handle_brightness,
    .color_power_tradeoff = kde_output_device_handle_uint_noop,
    .dimming = kde_output_device_handle_dimming,
    .replication_source = kde_output_device_handle_string_noop,
    .ddc_ci_allowed = kde_output_device_handle_uint_noop,
    .max_bits_per_color = kde_output_device_handle_uint_noop,
    .max_bits_per_color_range = kde_output_device_handle_max_bits_per_color_range,
    .automatic_max_bits_per_color_limit = kde_output_device_handle_uint_noop,
    .edr_policy = kde_output_device_handle_uint_noop,
    .sharpness = kde_output_device_handle_uint_noop,
    .priority = kde_output_device_handle_uint_noop,
    .auto_brightness = kde_output_device_handle_uint_noop,
    .removed = kde_output_device_handle_removed,
    .hdr_icc_profile_path = kde_output_device_handle_string_noop,
    .hdr_color_profile_source = kde_output_device_handle_uint_noop,
    .abm_level = kde_output_device_handle_uint_noop,
};

static void gsr_window_wayland_add_kde_output_device(gsr_window_wayland *self, struct kde_output_device_v2 *device) {
    gsr_kde_output_device *gsr_device = NULL;
    for(int i = 0; i < GSR_MAX_OUTPUTS; ++i) {
        if(!self->kde_output_devices[i].device) {
            gsr_device = &self->kde_output_devices[i];
            break;
        }
    }

    if(!gsr_device) {
        fprintf(stderr, "gsr warning: gsr_window_wayland_add_kde_output_device: reached maximum outputs (%d), ignoring output\n", GSR_MAX_OUTPUTS);
        kde_output_device_v2_destroy(device);
        return;
    }

    if(gsr_device->name) {
        free(gsr_device->name);
        gsr_device->name = NULL;
    }

    *gsr_device = (gsr_kde_output_device) {
        .device = device,
        .name = NULL,
        .hdr_enabled = false,
        .sdr_brightness = 0,
        .max_peak_brightness = 0,
        .max_peak_brightness_override = -1,
        .brightness = KDE_BRIGHTNESS_MULTIPLIER_MAX,
        .dimming = KDE_BRIGHTNESS_MULTIPLIER_MAX,
    };
    kde_output_device_v2_add_listener(device, &kde_output_device_listener, gsr_device);
}

static void kde_output_device_registry_handle_output(void *data, struct kde_output_device_registry_v2 *registry, struct kde_output_device_v2 *output) {
    (void)registry;
    gsr_window_wayland *self = data;
    gsr_window_wayland_add_kde_output_device(self, output);
}

static void kde_output_device_registry_handle_finished(void *data, struct kde_output_device_registry_v2 *registry) {
    (void)registry;
    gsr_window_wayland *self = data;
    if(self->kde_output_device_registry) {
        kde_output_device_registry_v2_destroy(self->kde_output_device_registry);
        self->kde_output_device_registry = NULL;
    }
}

static const struct kde_output_device_registry_v2_listener kde_output_device_registry_listener = {
    .finished = kde_output_device_registry_handle_finished,
    .output = kde_output_device_registry_handle_output,
};

static void registry_add_object(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)version;
    gsr_window_wayland *window_wayland = data;
    if(strcmp(interface, "wl_compositor") == 0) {
        if(window_wayland->compositor)
            return;

        window_wayland->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if(strcmp(interface, wl_output_interface.name) == 0) {
        if(version < 4) {
            fprintf(stderr, "gsr warning: wl output interface version is < 4, expected >= 4 to capture a monitor\n");
            return;
        }

        if(window_wayland->num_outputs == GSR_MAX_OUTPUTS) {
            fprintf(stderr, "gsr warning: reached maximum outputs (%d), ignoring output %u\n", GSR_MAX_OUTPUTS, name);
            return;
        }

        gsr_wayland_output *gsr_output = &window_wayland->outputs[window_wayland->num_outputs];
        window_wayland->num_outputs++;
        *gsr_output = (gsr_wayland_output) {
            .wl_name = name,
            .output = wl_registry_bind(registry, name, &wl_output_interface, 4),
            .pos = { .x = 0, .y = 0 },
            .size = { .x = 0, .y = 0 },
            .logical_size = { .x = 0, .y = 0 },
            .transform = 0,
            .name = NULL,
        };
        wl_output_add_listener(gsr_output->output, &output_listener, gsr_output);
    } else if(strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        if(version < 1) {
            fprintf(stderr, "gsr warning: xdg output interface version is < 1, expected >= 1 to capture a monitor\n");
            return;
        }

        if(window_wayland->xdg_output_manager)
            return;

        window_wayland->xdg_output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 1);
    } else if(strcmp(interface, kde_output_device_registry_v2_interface.name) == 0) {
        if(window_wayland->kde_output_device_registry)
            return;

        uint32_t bind_version = version;
        if(bind_version > (uint32_t)kde_output_device_registry_v2_interface.version)
            bind_version = kde_output_device_registry_v2_interface.version;

        window_wayland->kde_output_device_registry = wl_registry_bind(registry, name, &kde_output_device_registry_v2_interface, bind_version);
        kde_output_device_registry_v2_add_listener(window_wayland->kde_output_device_registry, &kde_output_device_registry_listener, window_wayland);
    } else if(strcmp(interface, kde_output_device_v2_interface.name) == 0) {
        if(version < 3)
            return;

        uint32_t bind_version = version;
        if(bind_version > (uint32_t)kde_output_device_v2_interface.version)
            bind_version = kde_output_device_v2_interface.version;

        struct kde_output_device_v2 *device = wl_registry_bind(registry, name, &kde_output_device_v2_interface, bind_version);
        gsr_window_wayland_add_kde_output_device(window_wayland, device);
    }
}

static void registry_remove_object(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
    // TODO: Remove output
}

static struct wl_registry_listener registry_listener = {
    .global = registry_add_object,
    .global_remove = registry_remove_object,
};

static void xdg_output_logical_position(void *data, struct zxdg_output_v1 *zxdg_output_v1, int32_t x, int32_t y) {
    (void)zxdg_output_v1;
    gsr_wayland_output *gsr_xdg_output = data;
    gsr_xdg_output->pos.x = x;
    gsr_xdg_output->pos.y = y;
}

static void xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height) {
    (void)xdg_output;
    gsr_wayland_output *gsr_xdg_output = data;
    gsr_xdg_output->logical_size.x = width;
    gsr_xdg_output->logical_size.y = height;
}

static void xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output) {
    (void)data;
    (void)xdg_output;
}

static void xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name) {
    (void)data;
    (void)xdg_output;
    (void)name;
}

static void xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output, const char *description) {
    (void)data;
    (void)xdg_output;
    (void)description;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
};

static void gsr_window_wayland_set_monitor_outputs_from_xdg_output(gsr_window_wayland *self) {
    if(!self->xdg_output_manager) {
        fprintf(stderr, "gsr warning: zxdg_output_manager not found. registered monitor positions might be incorrect\n");
        return;
    }

    for(int i = 0; i < self->num_outputs; ++i) {
        self->outputs[i].xdg_output = zxdg_output_manager_v1_get_xdg_output(self->xdg_output_manager, self->outputs[i].output);
        zxdg_output_v1_add_listener(self->outputs[i].xdg_output, &xdg_output_listener, &self->outputs[i]);
    }

    // Fetch xdg_output
    wl_display_roundtrip(self->display);
}

// static int monitor_sort_x_pos(const void* a, const void* b) {
//     const gsr_wayland_output *arg1 = *(const gsr_wayland_output**)a;
//     const gsr_wayland_output *arg2 = *(const gsr_wayland_output**)b;
//     return arg1->logical_pos.x - arg2->logical_pos.x;
// }

// static int monitor_sort_y_pos(const void* a, const void* b) {
//     const gsr_wayland_output *arg1 = *(const gsr_wayland_output**)a;
//     const gsr_wayland_output *arg2 = *(const gsr_wayland_output**)b;
//     return arg1->logical_pos.y - arg2->logical_pos.y;
// }

static void gsr_window_wayland_set_monitor_real_positions(gsr_window_wayland *self) {
    gsr_wayland_output *sorted_outputs[GSR_MAX_OUTPUTS];
    for(int i = 0; i < self->num_outputs; ++i) {
        sorted_outputs[i] = &self->outputs[i];
    }

    // TODO: set correct physical positions

    // qsort(sorted_outputs, self->num_outputs, sizeof(gsr_wayland_output*), monitor_sort_x_pos);
    // int x_pos = 0;
    // for(int i = 0; i < self->num_outputs; ++i) {
    //     fprintf(stderr, "monitor: %s\n", sorted_outputs[i]->name);
    //     sorted_outputs[i]->pos.x = x_pos;
    //     x_pos += sorted_outputs[i]->logical_size.x;
    // }

    // qsort(sorted_outputs, self->num_outputs, sizeof(gsr_wayland_output*), monitor_sort_y_pos);
    // int y_pos = 0;
    // for(int i = 0; i < self->num_outputs; ++i) {
    //     sorted_outputs[i]->pos.y = y_pos;
    //     y_pos += sorted_outputs[i]->logical_size.y;
    // }
}

static void gsr_window_wayland_deinit(gsr_window_wayland *self) {
    if(self->window) {
        wl_egl_window_destroy(self->window);
        self->window = NULL;
    }

    if(self->surface) {
        wl_surface_destroy(self->surface);
        self->surface = NULL;
    }

    for(int i = 0; i < self->num_outputs; ++i) {
        if(self->outputs[i].output) {
            wl_output_destroy(self->outputs[i].output);
            self->outputs[i].output = NULL;
        }

        if(self->outputs[i].name) {
            free(self->outputs[i].name);
            self->outputs[i].name = NULL;
        }

        if(self->outputs[i].xdg_output) {
            zxdg_output_v1_destroy(self->outputs[i].xdg_output);
            self->outputs[i].output = NULL;
        }
    }
    self->num_outputs = 0;

    for(int i = 0; i < GSR_MAX_OUTPUTS; ++i) {
        kde_output_device_free(&self->kde_output_devices[i]);
    }

    if(self->kde_output_device_registry) {
        kde_output_device_registry_v2_destroy(self->kde_output_device_registry);
        self->kde_output_device_registry = NULL;
    }

    if(self->xdg_output_manager) {
        zxdg_output_manager_v1_destroy(self->xdg_output_manager);
        self->xdg_output_manager = NULL;
    }

    if(self->compositor) {
        wl_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    if(self->registry) {
        wl_registry_destroy(self->registry);
        self->registry = NULL;
    }

    if(self->display) {
        wl_display_disconnect(self->display);
        self->display = NULL;
    }
}

static bool gsr_window_wayland_init(gsr_window_wayland *self) {
    self->display = wayland_connect_to_host();
    if(!self->display) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to connect to the Wayland server\n");
        goto fail;
    }

    self->registry = wl_display_get_registry(self->display); // TODO: Error checking
    wl_registry_add_listener(self->registry, &registry_listener, self); // TODO: Error checking

    // Fetch globals
    wl_display_roundtrip(self->display);

    // Fetch wl_output
    wl_display_roundtrip(self->display);

    gsr_window_wayland_set_monitor_outputs_from_xdg_output(self);
    gsr_window_wayland_set_monitor_real_positions(self);

    if(!self->compositor) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to find compositor\n");
        goto fail;
    }

    self->surface = wl_compositor_create_surface(self->compositor);
    if(!self->surface) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to create surface\n");
        goto fail;
    }

    self->window = wl_egl_window_create(self->surface, 16, 16);
    if(!self->window) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to create window\n");
        goto fail;
    }

    return true;

    fail:
    gsr_window_wayland_deinit(self);
    return false;
}

static void gsr_window_wayland_destroy(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    gsr_window_wayland_deinit(self);
    free(self);
    free(window);
}

static bool gsr_window_wayland_process_event(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    // TODO: pselect on wl_display_get_fd before doing dispatch
    const bool events_available = wl_display_dispatch_pending(self->display) > 0;
    wl_display_flush(self->display);
    return events_available;
}

static gsr_display_server gsr_wayland_get_display_server(void) {
    return GSR_DISPLAY_SERVER_WAYLAND;
}

static void* gsr_window_wayland_get_display(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    return self->display;
}

static void* gsr_window_wayland_get_window(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    return self->window;
}

static gsr_monitor_rotation wayland_transform_to_gsr_rotation(int32_t rot) {
    switch(rot) {
        case 0: return GSR_MONITOR_ROT_0;
        case 1: return GSR_MONITOR_ROT_90;
        case 2: return GSR_MONITOR_ROT_180;
        case 3: return GSR_MONITOR_ROT_270;
    }
    return GSR_MONITOR_ROT_0;
}

static vec2i get_monitor_size_rotated(int width, int height, gsr_monitor_rotation rotation) {
    vec2i size = { .x = width, .y = height };
    if(rotation == GSR_MONITOR_ROT_90 || rotation == GSR_MONITOR_ROT_270) {
        int tmp_x = size.x;
        size.x = size.y;
        size.y = tmp_x;
    }
    return size;
}

static void gsr_window_wayland_for_each_active_monitor_output_cached(const gsr_window *window, active_monitor_callback callback, void *userdata) {
    const gsr_window_wayland *self = window->priv;
    for(int i = 0; i < self->num_outputs; ++i) {
        const gsr_wayland_output *output = &self->outputs[i];
        if(!output->name)
            continue;

        const gsr_monitor_rotation rotation = wayland_transform_to_gsr_rotation(output->transform);

        vec2i size = { .x = output->size.x, .y = output->size.y };
        size = get_monitor_size_rotated(size.x, size.y, rotation);

        vec2i logical_size = { .x = output->logical_size.x, .y = output->logical_size.y };
        if(logical_size.x == 0 || logical_size.y == 0)
            logical_size = size;

        const int connector_type_index = get_connector_type_by_name(output->name);
        const int connector_type_id = get_connector_type_id_by_name(output->name);
        const gsr_monitor monitor = {
            .name = output->name,
            .name_len = strlen(output->name),
            .pos = { .x = output->pos.x, .y = output->pos.y },
            .size = size,
            .logical_pos = { .x = output->pos.x, .y = output->pos.y },
            .logical_size = logical_size,
            .connector_id = 0,
            .rotation = rotation,
            .monitor_identifier = (connector_type_index != -1 && connector_type_id != -1) ? monitor_identifier_from_type_and_count(connector_type_index, connector_type_id) : 0
        };
        callback(&monitor, userdata);
    }
}

static bool gsr_window_wayland_get_monitor_hdr_info(const gsr_window *window, const char *monitor_name, gsr_monitor_hdr_info *hdr_info) {
    const gsr_window_wayland *self = window->priv;
    for(int i = 0; i < GSR_MAX_OUTPUTS; ++i) {
        const gsr_kde_output_device *device = &self->kde_output_devices[i];
        if(!device->device || !device->name || strcmp(device->name, monitor_name) != 0)
            continue;

        if(device->sdr_brightness == 0)
            return false;

        const float brightness_multiplier = ((float)device->brightness / (float)KDE_BRIGHTNESS_MULTIPLIER_MAX) * ((float)device->dimming / (float)KDE_BRIGHTNESS_MULTIPLIER_MAX);
        hdr_info->hdr_enabled = device->hdr_enabled;
        hdr_info->sdr_white_luminance = (float)device->sdr_brightness * brightness_multiplier;
        hdr_info->max_peak_luminance = device->max_peak_brightness_override > 0 ? (float)device->max_peak_brightness_override : (float)device->max_peak_brightness;
        if(hdr_info->max_peak_luminance <= 0.0f)
            hdr_info->max_peak_luminance = KDE_PLASMA_ASSUMED_MONITOR_PEAK_LUMINANCE;
        return true;
    }
    return false;
}

gsr_window* gsr_window_wayland_create(void) {
    gsr_window *window = calloc(1, sizeof(gsr_window));
    if(!window)
        return window;

    gsr_window_wayland *window_wayland = calloc(1, sizeof(gsr_window_wayland));
    if(!window_wayland) {
        free(window);
        return NULL;
    }

    if(!gsr_window_wayland_init(window_wayland)) {
        free(window_wayland);
        free(window);
        return NULL;
    }

    *window = (gsr_window) {
        .destroy = gsr_window_wayland_destroy,
        .process_event = gsr_window_wayland_process_event,
        .get_event_data = NULL,
        .get_display_server = gsr_wayland_get_display_server,
        .get_display = gsr_window_wayland_get_display,
        .get_window = gsr_window_wayland_get_window,
        .for_each_active_monitor_output_cached = gsr_window_wayland_for_each_active_monitor_output_cached,
        .get_monitor_hdr_info = gsr_window_wayland_get_monitor_hdr_info,
        .priv = window_wayland
    };

    return window;
}
