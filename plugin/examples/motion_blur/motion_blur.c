/*
 * Motion blur plugin for GPU Screen Recorder (frame blending).
 *
 * Build:
 *   gcc -shared -fPIC -O2 motion_blur.c -o motion_blur.so -ldl
 *
 * Use:
 *   GSR_MOTION_BLUR=0.5 gpu-screen-recorder -w screen -p ./motion_blur.so ...
 *
 * GSR_MOTION_BLUR is the weight of the previous frame (0.0 = off, 0.9 = very long trails).
 * Default is 0.5.
 *
 * How it works: the plugin framebuffer already contains the captured scene when draw() is called.
 * We draw the previous output over it with alpha = strength, then copy the result back into our
 * history texture, so every frame is an exponentially weighted blend of all earlier frames.
 */
#include "../../plugin.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_TEXTURE_2D            0x0DE1
#define GL_TEXTURE0              0x84C0
#define GL_RGBA8                 0x8058
#define GL_RGBA16                0x805B
#define GL_LINEAR                0x2601
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_WRAP_S        0x2802
#define GL_TEXTURE_WRAP_T        0x2803
#define GL_CLAMP_TO_EDGE         0x812F
#define GL_BLEND                 0x0BE2
#define GL_SRC_ALPHA             0x0302
#define GL_ONE_MINUS_SRC_ALPHA   0x0303
#define GL_TRIANGLES             0x0004
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82

typedef struct {
    void (*glGenTextures)(int, unsigned int*);
    void (*glDeleteTextures)(int, const unsigned int*);
    void (*glBindTexture)(unsigned int, unsigned int);
    void (*glActiveTexture)(unsigned int);
    void (*glTexParameteri)(unsigned int, unsigned int, int);
    void (*glTexStorage2D)(unsigned int, int, unsigned int, int, int);
    void (*glCopyTexSubImage2D)(unsigned int, int, int, int, int, int, int, int);
    unsigned int (*glCreateShader)(unsigned int);
    void (*glShaderSource)(unsigned int, int, const char *const*, const int*);
    void (*glCompileShader)(unsigned int);
    void (*glGetShaderiv)(unsigned int, unsigned int, int*);
    unsigned int (*glCreateProgram)(void);
    void (*glAttachShader)(unsigned int, unsigned int);
    void (*glLinkProgram)(unsigned int);
    void (*glGetProgramiv)(unsigned int, unsigned int, int*);
    void (*glDeleteShader)(unsigned int);
    void (*glDeleteProgram)(unsigned int);
    void (*glUseProgram)(unsigned int);
    int  (*glGetUniformLocation)(unsigned int, const char*);
    void (*glUniform1i)(int, int);
    void (*glUniform1f)(int, float);
    void (*glEnable)(unsigned int);
    void (*glDisable)(unsigned int);
    void (*glBlendFunc)(unsigned int, unsigned int);
    void (*glDrawArrays)(unsigned int, int, int);
    void (*glGenVertexArrays)(int, unsigned int*);
    void (*glBindVertexArray)(unsigned int);
    void (*glDeleteVertexArrays)(int, const unsigned int*);
} gl_funcs;

typedef struct {
    gl_funcs gl;
    unsigned int history;
    unsigned int program;
    unsigned int vao;
    int u_tex, u_strength;
    float strength;
    int width, height;
    bool have_history;
} state_t;

static const char *VS =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform float strength;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(texture(tex, uv).rgb, strength); }\n";

static void *get_proc(gsr_plugin_graphics_api api, const char *name) {
    void *p = NULL;
    if(api == GSR_PLUGIN_GRAPHICS_API_EGL_ES) {
        void *lib = dlopen("libEGL.so.1", RTLD_LAZY);
        void *(*get)(const char*) = lib ? (void *(*)(const char*))dlsym(lib, "eglGetProcAddress") : NULL;
        if(get) p = get(name);
    } else {
        void *lib = dlopen("libGL.so.1", RTLD_LAZY);
        void *(*get)(const char*) = lib ? (void *(*)(const char*))dlsym(lib, "glXGetProcAddressARB") : NULL;
        if(get) p = get(name);
    }
    return p;
}

static bool load_gl(gl_funcs *g, gsr_plugin_graphics_api api) {
    #define LOAD(fn) do { g->fn = get_proc(api, #fn); if(!g->fn) { fprintf(stderr, "motion_blur: missing GL function %s\n", #fn); return false; } } while(0)
    LOAD(glGenTextures); LOAD(glDeleteTextures); LOAD(glBindTexture); LOAD(glActiveTexture);
    LOAD(glTexParameteri); LOAD(glTexStorage2D); LOAD(glCopyTexSubImage2D);
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram); LOAD(glGetProgramiv);
    LOAD(glDeleteShader); LOAD(glDeleteProgram); LOAD(glUseProgram);
    LOAD(glGetUniformLocation); LOAD(glUniform1i); LOAD(glUniform1f);
    LOAD(glEnable); LOAD(glDisable); LOAD(glBlendFunc); LOAD(glDrawArrays);
    LOAD(glGenVertexArrays); LOAD(glBindVertexArray); LOAD(glDeleteVertexArrays);
    #undef LOAD
    return true;
}

static unsigned int compile(gl_funcs *g, unsigned int type, const char *src) {
    unsigned int s = g->glCreateShader(type);
    g->glShaderSource(s, 1, &src, NULL);
    g->glCompileShader(s);
    int ok = 0;
    g->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok) { g->glDeleteShader(s); return 0; }
    return s;
}

static void blur_draw(const gsr_plugin_draw_params *params, void *userdata) {
    state_t *st = userdata;
    gl_funcs *g = &st->gl;
    const int w = (int)params->width, h = (int)params->height;

    if(st->have_history) {
        g->glUseProgram(st->program);
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, st->history);
        g->glUniform1i(st->u_tex, 0);
        g->glUniform1f(st->u_strength, st->strength);
        g->glEnable(GL_BLEND);
        g->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        g->glBindVertexArray(st->vao);
        g->glDrawArrays(GL_TRIANGLES, 0, 3);
        g->glBindVertexArray(0);
        g->glDisable(GL_BLEND);
        g->glUseProgram(0);
    }

    /* Save the (blended) result of this frame for the next one */
    g->glBindTexture(GL_TEXTURE_2D, st->history);
    g->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    g->glBindTexture(GL_TEXTURE_2D, 0);
    st->have_history = true;
}

bool gsr_plugin_init(const gsr_plugin_init_params *params, gsr_plugin_init_return *ret) {
    state_t *st = calloc(1, sizeof(*st));
    if(!st) return false;

    if(!load_gl(&st->gl, params->graphics_api)) { free(st); return false; }
    gl_funcs *g = &st->gl;

    st->strength = 0.5f;
    const char *env = getenv("GSR_MOTION_BLUR");
    if(env) {
        float v = strtof(env, NULL);
        if(v < 0.0f) v = 0.0f;
        if(v > 0.95f) v = 0.95f;
        st->strength = v;
    }
    st->width = (int)params->width;
    st->height = (int)params->height;

    unsigned int vs = compile(g, GL_VERTEX_SHADER, VS);
    unsigned int fs = compile(g, GL_FRAGMENT_SHADER, FS);
    if(!vs || !fs) {
        fprintf(stderr, "motion_blur: shader compile failed\n");
        free(st);
        return false;
    }
    st->program = g->glCreateProgram();
    g->glAttachShader(st->program, vs);
    g->glAttachShader(st->program, fs);
    g->glLinkProgram(st->program);
    g->glDeleteShader(vs);
    g->glDeleteShader(fs);
    int ok = 0;
    g->glGetProgramiv(st->program, GL_LINK_STATUS, &ok);
    if(!ok) {
        fprintf(stderr, "motion_blur: program link failed\n");
        g->glDeleteProgram(st->program);
        free(st);
        return false;
    }
    st->u_tex = g->glGetUniformLocation(st->program, "tex");
    st->u_strength = g->glGetUniformLocation(st->program, "strength");

    g->glGenVertexArrays(1, &st->vao);

    /* Same internal format as the plugin framebuffer so the copy is compatible */
    const unsigned int fmt = params->color_depth == GSR_PLUGIN_COLOR_DEPTH_10_BITS ? GL_RGBA16 : GL_RGBA8;
    g->glGenTextures(1, &st->history);
    g->glBindTexture(GL_TEXTURE_2D, st->history);
    g->glTexStorage2D(GL_TEXTURE_2D, 1, fmt, st->width, st->height);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->glBindTexture(GL_TEXTURE_2D, 0);

    fprintf(stderr, "motion_blur: strength %.2f, %dx%d\n", st->strength, st->width, st->height);

    ret->name = "motion_blur";
    ret->version = 1;
    ret->userdata = st;
    ret->draw = blur_draw;
    return true;
}

void gsr_plugin_deinit(void *userdata) {
    state_t *st = userdata;
    if(!st) return;
    gl_funcs *g = &st->gl;
    if(st->history) g->glDeleteTextures(1, &st->history);
    if(st->vao) g->glDeleteVertexArrays(1, &st->vao);
    if(st->program) g->glDeleteProgram(st->program);
    free(st);
}
