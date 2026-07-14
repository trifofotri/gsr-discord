#include "../include/kde_night_light.h"

#ifdef GSR_DBUS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <dbus/dbus.h>

#define NIGHT_LIGHT_POLL_SECONDS 1
#define NIGHT_LIGHT_TEMPERATURE_NEUTRAL 6500
#define NIGHT_LIGHT_TEMPERATURE_MIN 1000

typedef struct {
    double v[3];
} vec3d;

struct gsr_kde_night_light {
    pthread_t thread;
    bool thread_created;
    atomic_bool stop;
    atomic_int temperature;
    int cached_temperature;
    float cached_inverse_matrix[9];
};

/* Blackbody whitepoint table for 1000K-6500K at 100K intervals, from https://github.com/jonls/redshift/blob/master/README-colorramp */
static const double blackbody_whitepoints[56][3] = {
    {1.00000000, 0.18172716, 0.00000000}, {1.00000000, 0.25503671, 0.00000000},
    {1.00000000, 0.30942099, 0.00000000}, {1.00000000, 0.35357379, 0.00000000},
    {1.00000000, 0.39091524, 0.00000000}, {1.00000000, 0.42322816, 0.00000000},
    {1.00000000, 0.45159884, 0.00000000}, {1.00000000, 0.47675916, 0.00000000},
    {1.00000000, 0.49923747, 0.00000000}, {1.00000000, 0.51943421, 0.00000000},
    {1.00000000, 0.54360078, 0.08679949}, {1.00000000, 0.56618736, 0.14065513},
    {1.00000000, 0.58734976, 0.18362641}, {1.00000000, 0.60724493, 0.22137978},
    {1.00000000, 0.62600248, 0.25591950}, {1.00000000, 0.64373109, 0.28819679},
    {1.00000000, 0.66052319, 0.31873863}, {1.00000000, 0.67645822, 0.34786758},
    {1.00000000, 0.69160518, 0.37579588}, {1.00000000, 0.70602449, 0.40267128},
    {1.00000000, 0.71976951, 0.42860152}, {1.00000000, 0.73288760, 0.45366838},
    {1.00000000, 0.74542112, 0.47793608}, {1.00000000, 0.75740814, 0.50145662},
    {1.00000000, 0.76888303, 0.52427322}, {1.00000000, 0.77987699, 0.54642268},
    {1.00000000, 0.79041843, 0.56793692}, {1.00000000, 0.80053332, 0.58884417},
    {1.00000000, 0.81024551, 0.60916971}, {1.00000000, 0.81957693, 0.62893653},
    {1.00000000, 0.82854786, 0.64816570}, {1.00000000, 0.83717703, 0.66687674},
    {1.00000000, 0.84548188, 0.68508786}, {1.00000000, 0.85347859, 0.70281616},
    {1.00000000, 0.86118227, 0.72007777}, {1.00000000, 0.86860704, 0.73688797},
    {1.00000000, 0.87576611, 0.75326132}, {1.00000000, 0.88267187, 0.76921169},
    {1.00000000, 0.88933596, 0.78475236}, {1.00000000, 0.89576933, 0.79989606},
    {1.00000000, 0.90198230, 0.81465502}, {1.00000000, 0.90963069, 0.82838210},
    {1.00000000, 0.91710889, 0.84190889}, {1.00000000, 0.92441842, 0.85523742},
    {1.00000000, 0.93156127, 0.86836903}, {1.00000000, 0.93853986, 0.88130458},
    {1.00000000, 0.94535695, 0.89404470}, {1.00000000, 0.95201559, 0.90658983},
    {1.00000000, 0.95851906, 0.91894041}, {1.00000000, 0.96487079, 0.93109690},
    {1.00000000, 0.97107439, 0.94305985}, {1.00000000, 0.97713351, 0.95482993},
    {1.00000000, 0.98305189, 0.96640795}, {1.00000000, 0.98883326, 0.97779486},
    {1.00000000, 0.99448139, 0.98899179}, {1.00000000, 1.00000000, 1.00000000}
};

/* The same channel factors that kde plasma uses for night light (kwin colortemperature.h, sampleColorTemperature) */
static vec3d night_light_channel_factors(int temperature) {
    if(temperature < NIGHT_LIGHT_TEMPERATURE_MIN)
        temperature = NIGHT_LIGHT_TEMPERATURE_MIN;
    if(temperature >= NIGHT_LIGHT_TEMPERATURE_NEUTRAL)
        return (vec3d){ .v = { 1.0, 1.0, 1.0 } };

    const int index = (temperature - NIGHT_LIGHT_TEMPERATURE_MIN) / 100;
    const double blend = (temperature % 100) / 100.0;
    vec3d result;
    for(int i = 0; i < 3; ++i) {
        const double whitepoint = blackbody_whitepoints[index][i] * (1.0 - blend) + blackbody_whitepoints[index + 1][i] * blend;
        result.v[i] = pow(whitepoint, 2.2);
    }
    return result;
}

/* Kde plasma multiplies the composited image with the night light channel factors in linear space (verified against kde plasma 6.7, both in sdr and hdr mode) */
static void compute_inverse_night_light_matrix(int temperature, float inverse_matrix[9]) {
    const vec3d factors = night_light_channel_factors(temperature);
    memset(inverse_matrix, 0, sizeof(float) * 9);
    inverse_matrix[0] = 1.0 / fmax(factors.v[0], 0.0001);
    inverse_matrix[4] = 1.0 / fmax(factors.v[1], 0.0001);
    inverse_matrix[8] = 1.0 / fmax(factors.v[2], 0.0001);
}

static bool dbus_get_night_light_property(DBusConnection *connection, const char *property_name, int expected_type, DBusBasicValue *value) {
    DBusMessage *message = dbus_message_new_method_call("org.kde.KWin.NightLight", "/org/kde/KWin/NightLight", "org.freedesktop.DBus.Properties", "Get");
    if(!message)
        return false;

    const char *interface_name = "org.kde.KWin.NightLight";
    dbus_message_append_args(message, DBUS_TYPE_STRING, &interface_name, DBUS_TYPE_STRING, &property_name, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, 500, NULL);
    dbus_message_unref(message);
    if(!reply)
        return false;

    bool success = false;
    DBusMessageIter iter;
    if(dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter variant_iter;
        dbus_message_iter_recurse(&iter, &variant_iter);
        if(dbus_message_iter_get_arg_type(&variant_iter) == expected_type) {
            dbus_message_iter_get_basic(&variant_iter, value);
            success = true;
        }
    }

    dbus_message_unref(reply);
    return success;
}

static int query_night_light_temperature(DBusConnection *connection) {
    DBusBasicValue temperature;
    if(!dbus_get_night_light_property(connection, "currentTemperature", DBUS_TYPE_UINT32, &temperature))
        return 0;

    if(temperature.u32 == 0 || temperature.u32 >= NIGHT_LIGHT_TEMPERATURE_NEUTRAL)
        return 0;

    return temperature.u32;
}

static void* night_light_poll_thread(void *userdata) {
    gsr_kde_night_light *self = userdata;

    DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, NULL);
    if(!connection) {
        atomic_store(&self->temperature, 0);
        return NULL;
    }

    while(!atomic_load(&self->stop)) {
        atomic_store(&self->temperature, query_night_light_temperature(connection));
        for(int i = 0; i < NIGHT_LIGHT_POLL_SECONDS * 10 && !atomic_load(&self->stop); ++i) {
            usleep(100 * 1000);
        }
    }

    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    return NULL;
}

gsr_kde_night_light* gsr_kde_night_light_create(void) {
    gsr_kde_night_light *self = calloc(1, sizeof(gsr_kde_night_light));
    if(!self)
        return NULL;

    self->cached_temperature = -1;

    if(pthread_create(&self->thread, NULL, night_light_poll_thread, self) != 0) {
        free(self);
        return NULL;
    }

    self->thread_created = true;
    return self;
}

void gsr_kde_night_light_destroy(gsr_kde_night_light *self) {
    if(!self)
        return;

    if(self->thread_created) {
        atomic_store(&self->stop, true);
        pthread_join(self->thread, NULL);
    }
    free(self);
}

bool gsr_kde_night_light_get_inverse_matrix(gsr_kde_night_light *self, float inverse_matrix[9]) {
    if(!self)
        return false;

    const int temperature = atomic_load(&self->temperature);
    if(temperature <= 0)
        return false;

    if(self->cached_temperature != temperature) {
        compute_inverse_night_light_matrix(temperature, self->cached_inverse_matrix);
        self->cached_temperature = temperature;
    }

    memcpy(inverse_matrix, self->cached_inverse_matrix, sizeof(self->cached_inverse_matrix));
    return true;
}

#else /* GSR_DBUS */

#include <stddef.h>

gsr_kde_night_light* gsr_kde_night_light_create(void) {
    return NULL;
}

void gsr_kde_night_light_destroy(gsr_kde_night_light *self) {
    (void)self;
}

bool gsr_kde_night_light_get_inverse_matrix(gsr_kde_night_light *self, float inverse_matrix[9]) {
    (void)self;
    (void)inverse_matrix;
    return false;
}

#endif /* GSR_DBUS */
