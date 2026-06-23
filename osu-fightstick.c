/*
 * osu-fightstick - Universal Gamepad/Fightstick Input Bridge
 *
 * Automatically discovers connected gamepads/fightsticks and maps
 * directional D-Pad/Joystick inputs to standard keyboard outputs.
 *
 * Copyright 2026 Zachary Kessler
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <stdarg.h>

#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <linux/input.h>

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

#define DEF_V1     KEY_Z
#define DEF_V2     KEY_X
#define DEF_NAME   "Universal Gamepad Input Bridge"
#define DEF_WAV    "./click.wav"

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void logf_(const char *level, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

static void logf_(const char *level, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[osu-fightstick] %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define LOG_INFO(...) logf_("info",  __VA_ARGS__)
#define LOG_WARN(...) logf_("warn",  __VA_ARGS__)
#define LOG_ERR(...)  logf_("error", __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Audio (PipeWire)                                                          */
/* ------------------------------------------------------------------------- */

static int audio_available = 0;
static int audio_enabled_flag = 0;
static const char *wav_path_flag = DEF_WAV;

static struct {
    float                    *samples;
    size_t                    num_frames;
    int                       channels;
    int                       sample_rate;

    struct pw_thread_loop    *loop;
    struct pw_stream         *stream;

    atomic_int                pending;
    atomic_bool               playing;
    atomic_bool               reset;
    atomic_size_t             frame_pos;
} audio;

typedef struct { char id[4]; uint32_t size; } wav_chunk;
typedef struct {
    uint16_t fmt;
    uint16_t ch;
    uint32_t rate;
    uint32_t br;
    uint16_t ba;
    uint16_t bps;
} wav_fmt;

static int wav_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_WARN("Cannot open WAV %s: %s", path, strerror(errno));
        return -1;
    }

    char riff[4]; uint32_t rsize; char wave[4];
    if (fread(riff, 4, 1, f) != 1 || fread(&rsize, 4, 1, f) != 1 ||
        fread(wave, 4, 1, f) != 1 ||
        memcmp(riff, "RIFF", 4) || memcmp(wave, "WAVE", 4)) {
        fclose(f);
        return -1;
    }

    wav_fmt fmt = {0};
    uint32_t ds = 0;

    for (;;) {
        wav_chunk ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) break;
        if (!memcmp(ch.id, "fmt ", 4)) {
            size_t rs = ch.size < sizeof(fmt) ? ch.size : sizeof(fmt);
            if (fread(&fmt, rs, 1, f) != 1) { fclose(f); return -1; }
            if (ch.size > sizeof(fmt)) fseek(f, (long)(ch.size - sizeof(fmt)), SEEK_CUR);
        } else if (!memcmp(ch.id, "data", 4)) {
            ds = ch.size; break;
        } else {
            fseek(f, (long)ch.size, SEEK_CUR);
        }
    }

    if (fmt.fmt != 1 || ds == 0 || fmt.bps == 0 || fmt.ch == 0) {
        fclose(f);
        return -1;
    }

    audio.channels    = fmt.ch;
    audio.sample_rate = (int)fmt.rate;
    audio.num_frames  = ds / (fmt.bps / 8u) / fmt.ch;
    audio.samples     = calloc(audio.num_frames * audio.channels, sizeof(float));
    if (!audio.samples) { fclose(f); return -1; }

    {
        uint8_t *raw = malloc(ds);
        if (!raw || fread(raw, ds, 1, f) != 1) {
            free(raw); fclose(f); free(audio.samples); audio.samples = NULL; return -1;
        }
        fclose(f);

        size_t total = audio.num_frames * audio.channels;
        switch (fmt.bps) {
            case 16: for (size_t i = 0; i < total; i++) audio.samples[i] = ((int16_t *)raw)[i] / 32768.0f; break;
            case 24: for (size_t i = 0; i < total; i++) audio.samples[i] = (int32_t)(raw[i*3] | ((uint32_t)raw[i*3+1] << 8) | ((int32_t)((int8_t)raw[i*3+2]) << 16)) / 8388608.0f; break;
            case 32: for (size_t i = 0; i < total; i++) audio.samples[i] = ((int32_t *)raw)[i] / 2147483648.0f; break;
            default: free(raw); free(audio.samples); audio.samples = NULL; return -1;
        }
        free(raw);
    }
    LOG_INFO("Loaded %s: %zu frames, %d ch, %d Hz", path, audio.num_frames, audio.channels, audio.sample_rate);
    return 0;
}

static void on_process(void *userdata) {
    (void)userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(audio.stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) return;

    int stride = (int)(sizeof(float) * audio.channels);
    int n_frames = b->requested ? SPA_MIN((int)b->requested, buf->datas[0].maxsize / stride) : buf->datas[0].maxsize / stride;
    size_t nf = (size_t)n_frames;

    if (!atomic_load_explicit(&audio.playing, memory_order_acquire) && atomic_load(&audio.pending) > 0) {
        atomic_store_explicit(&audio.playing, true, memory_order_relaxed);
        atomic_store(&audio.frame_pos, 0);
        atomic_fetch_sub(&audio.pending, 1);
    }

    if (atomic_load_explicit(&audio.playing, memory_order_acquire)) {
        if (atomic_exchange(&audio.reset, false)) atomic_store(&audio.frame_pos, 0);

        size_t pos = atomic_load(&audio.frame_pos);
        size_t tc = (nf < audio.num_frames - pos) ? nf : audio.num_frames - pos;

        if (tc > 0) memcpy(dst, audio.samples + pos * audio.channels, tc * stride);
        if (nf > tc) memset(dst + tc * audio.channels, 0, (nf - tc) * stride);

        pos += tc;
        if (pos >= audio.num_frames) {
            atomic_store(&audio.playing, false);
            atomic_store(&audio.frame_pos, 0);
            if (atomic_load(&audio.pending) > 0) {
                atomic_store(&audio.playing, true);
                atomic_fetch_sub(&audio.pending, 1);
            }
        } else {
            atomic_store(&audio.frame_pos, pos);
        }
    } else {
        memset(dst, 0, nf * stride);
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size   = nf * stride;
    pw_stream_queue_buffer(audio.stream, b);
}

static const struct pw_stream_events stream_events = { PW_VERSION_STREAM_EVENTS, .process = on_process };

static int audio_init(void) {
    pw_init(NULL, NULL);
    if (!(audio.loop = pw_thread_loop_new("osu-audio", NULL))) { pw_deinit(); return -1; }

    pw_thread_loop_lock(audio.loop);
    struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game", NULL);
    audio.stream = pw_stream_new_simple(pw_thread_loop_get_loop(audio.loop), "osu-intercept", props, &stream_events, NULL);

    uint8_t podbuf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
    const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .channels = audio.channels, .rate = audio.sample_rate)) };

    pw_stream_connect(audio.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
    pw_thread_loop_unlock(audio.loop);

    if (pw_thread_loop_start(audio.loop) < 0) return -1;
    return 0;
}

static void audio_trigger(void) {
    if (!audio_available) return;
    if (atomic_load(&audio.playing)) atomic_store(&audio.reset, true);
    else atomic_fetch_add(&audio.pending, 1);
}

/* ------------------------------------------------------------------------- */
/* Input devices & Auto-Discovery                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    struct libevdev  *dev;
    int               fd;
    char             *path;
    int               y_raw;  /* Analog Y axis or D-Pad Up/Down */
    int               x_raw;  /* Analog X axis or D-Pad Left/Right */
    int               k1;     /* Binary normalized Y state */
    int               k2;     /* Binary normalized X state */
    int               act;    /* 1 == v1 active, 0 == v2 active */
} input_dev_t;

static int input_open(const char *path, input_dev_t *out) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); return -1; }

    /* Filter: Only grab things that look like gamepads or fightsticks */
    if (!libevdev_has_event_code(dev, EV_KEY, BTN_GAMEPAD) &&
        !libevdev_has_event_code(dev, EV_KEY, BTN_SOUTH)) { 
        //!libevdev_has_event_code(dev, EV_ABS, ABS_X) &&
        //!libevdev_has_event_code(dev, EV_ABS, ABS_HAT0X)) {
        libevdev_free(dev);
        close(fd);
        return -1; /* Not a target device */
    }

    if (libevdev_grab(dev, LIBEVDEV_GRAB) < 0) { libevdev_free(dev); close(fd); return -1; }

    memset(out, 0, sizeof(*out));
    out->fd   = fd;
    out->dev  = dev;
    out->path = strdup(path);
    LOG_INFO("Discovered and grabbed Gamepad: %s (\"%s\")", path, libevdev_get_name(dev) ? libevdev_get_name(dev) : "?");
    return 0;
}

static void input_close(input_dev_t *in) {
    if (!in || !in->dev) return;
    libevdev_grab(in->dev, LIBEVDEV_UNGRAB);
    libevdev_free(in->dev);
    close(in->fd);
    free(in->path);
    memset(in, 0, sizeof(*in));
}

/* ------------------------------------------------------------------------- */
/* Virtual uinput device                                                     */
/* ------------------------------------------------------------------------- */

static struct libevdev         *vdev  = NULL;
static struct libevdev_uinput  *uidev = NULL;

static int build_virtual(void) {
    if (!(vdev = libevdev_new())) return -1;
    libevdev_set_name(vdev, DEF_NAME);
    libevdev_set_id_bustype(vdev, BUS_USB);
    libevdev_set_id_vendor(vdev, 0x045E); // Generic Microsoft vendor spoof
    libevdev_set_id_product(vdev, 0x028E);

    libevdev_enable_event_type(vdev, EV_SYN);
    libevdev_enable_event_type(vdev, EV_KEY);
    libevdev_enable_event_code(vdev, EV_KEY, DEF_V1, NULL);
    libevdev_enable_event_code(vdev, EV_KEY, DEF_V2, NULL);

    if (libevdev_uinput_create_from_device(vdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev) < 0) {
        libevdev_free(vdev); return -1;
    }
    LOG_INFO("Created virtual keyboard \"%s\" at %s", DEF_NAME, libevdev_uinput_get_devnode(uidev));
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Normalized Analog State Machine                                           */
/* ------------------------------------------------------------------------- */

// Flag for rapid-fire mode
static int rapid = 0;

static int process_event(input_dev_t *in, const struct input_event *ie) {
    if (ie->type == EV_MSC || ie->type == EV_SYN) return 0;

    int axis_changed = 0;

    /* Normalize Absolute Axes (D-pad/Joystick) to binary tracking */
    if (ie->type == EV_ABS) {
        if (ie->code == ABS_HAT0Y || ie->code == ABS_Y) { in->y_raw = ie->value; axis_changed = 1; }
        else if (ie->code == ABS_HAT0X || ie->code == ABS_X) { in->x_raw = ie->value; axis_changed = 1; }
    } 
    /* Catch standard discrete D-pad buttons if the controller uses them */
    else if (ie->type == EV_KEY) {
        if (ie->code == BTN_DPAD_UP || ie->code == BTN_DPAD_DOWN) { in->y_raw = ie->value; axis_changed = 1; }
        else if (ie->code == BTN_DPAD_LEFT || ie->code == BTN_DPAD_RIGHT) { in->x_raw = ie->value; axis_changed = 1; }
    }

    if (!axis_changed) return 0;

    /* Treat any non-zero deflection on an axis as a physical key-down */
    int new_k1 = (in->y_raw != 0) ? 1 : 0;
    int new_k2 = (in->x_raw != 0) ? 1 : 0;

    if (new_k1 == in->k1 && new_k2 == in->k2) return 0; /* Axis changed value, but not state (e.g., analog deadzone drift) */

    int old_state = in->k1 + in->k2;
    in->k1 = new_k1;
    in->k2 = new_k2;
    int new_state = in->k1 + in->k2;
    int triggered = 0;

    /* Evaluate the exact same overlapping-release logic using normalized states */
    if (old_state == 0 && new_state == 1) {
        int code = in->k1 ? DEF_V1 : DEF_V2;
        in->act = in->k1;
        libevdev_uinput_write_event(uidev, EV_KEY, code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        triggered = 1;
    } else if ((rapid && old_state == 1 && new_state == 2) || (old_state == 2 && new_state == 1)) {
        int up_code = in->act ? DEF_V1 : DEF_V2;
        int down_code = in->act ? DEF_V2 : DEF_V1;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        libevdev_uinput_write_event(uidev, EV_KEY, down_code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        in->act = !in->act;
        triggered = 1;
    } else if (old_state == 1 && new_state == 0) {
        int up_code = in->act ? DEF_V1 : DEF_V2;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        in->act = 0;
    }

    return triggered;
}

/* ------------------------------------------------------------------------- */
/* Loop                                                                      */
/* ------------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static int run_loop(input_dev_t *inputs, size_t n_inputs) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    for (size_t i = 0; i < n_inputs; i++) {
        struct epoll_event ev = { .events = EPOLLIN, .data = { .u64 = i } };
        epoll_ctl(epfd, EPOLL_CTL_ADD, inputs[i].fd, &ev);
    }

    struct epoll_event events[16];
    size_t alive = n_inputs;

    while (g_running && alive > 0) {
        int nfd = epoll_wait(epfd, events, 16, -1);
        if (nfd < 0 && errno == EINTR) continue;

        for (int i = 0; i < nfd; i++) {
            input_dev_t *in = &inputs[events[i].data.u64];
            if (!in->dev) continue;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
                input_close(in);
                alive--;
                continue;
            }

            unsigned int flag = LIBEVDEV_READ_FLAG_NORMAL;
            for (;;) {
                struct input_event ie;
                int rc = libevdev_next_event(in->dev, flag, &ie);
                if (rc == -EAGAIN) break;
                if (rc == LIBEVDEV_READ_STATUS_SYNC) { flag = LIBEVDEV_READ_FLAG_SYNC; continue; }
                flag = LIBEVDEV_READ_FLAG_NORMAL;
                if (process_event(in, &ie)) audio_trigger();
            }
        }
    }
    close(epfd);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

static void print_usage(FILE *s, const char *prog) {
    fprintf(s, "osu-fightstick - Universal Gamepad Input Bridge\n\n"
               "usage: %s [-h] [-a <path>]\n\n"
               "options:\n"
               "    -h          show this help and exit\n"
               "    -r          enable rapid-fire mode\n"
               "    -a <wav_file>   enable audio clicks via wav file\n", prog);
}

int main(int argc, char **argv) {
    for (int opt; (opt = getopt(argc, argv, "hra:")) != -1; ) {
        switch (opt) {
            case 'h': print_usage(stdout, argv[0]); return EXIT_SUCCESS;
            case 'r': rapid = 1; break;
            case 'a': audio_enabled_flag = 1; wav_path_flag = optarg; break;
            default: print_usage(stderr, argv[0]); return EXIT_FAILURE;
        }
    }

    struct sched_param sp = { .sched_priority = 90 };
    sched_setscheduler(0, SCHED_FIFO, &sp);

    if (audio_enabled_flag) {
        if (wav_load(wav_path_flag) == 0 && audio_init() == 0) {
            mlockall(MCL_CURRENT | MCL_FUTURE);
            audio_available = 1;
        } else {
            LOG_WARN("Audio disabled (WAV load or PipeWire init failed)");
        }
    }

    glob_t gl;
    glob("/dev/input/event*", 0, NULL, &gl);
    input_dev_t *inputs = calloc(gl.gl_pathc, sizeof(*inputs));
    size_t opened = 0;
    
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        if (input_open(gl.gl_pathv[i], &inputs[opened]) == 0) opened++;
    }
    globfree(&gl);

    if (opened == 0 || build_virtual() != 0) {
        LOG_ERR("Failed to initialize devices. Aborting.");
        return EXIT_FAILURE;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    LOG_INFO("Ready. Waiting for joystick commands...");
    run_loop(inputs, opened);

    if (uidev) libevdev_uinput_destroy(uidev);
    if (vdev) libevdev_free(vdev);
    for (size_t i = 0; i < opened; i++) input_close(&inputs[i]);
    free(inputs);
    return EXIT_SUCCESS;
}
