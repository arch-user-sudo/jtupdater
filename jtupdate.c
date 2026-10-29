/*
 * jtupdate — a small JTLab repository updater.
 *
 * Wayland + cairo + Pango. No X11, no GTK, no Qt.
 *
 *   make
 *   ./jtupdate
 *   ./jtupdate '#748382'                 (optional accent override)
 *   ./jtupdatectl accent '#748382'       (change accent while running)
 *   ./jtupdatectl update                 (start an update)
 *
 * The repositories are cloned into <base-dir>/<slug>, which defaults to
 * ~/jtupdate/repos and can be overridden with --base-dir.  Add the clone
 * URLs to the g_repos table below.  An existing git checkout can also be used
 * by setting its path; a missing URL then means "pull the existing origin".
 *
 * Controls:
 *   Space / Enter        check for updates (or stop while running)
 *   Esc / q              quit
 *   Drag header          move the window
 *   Accent CLI / socket  live theme updates
 *
 * Palette from ~/JTLab/config.h:
 *   bg #030303, fg #FFFFFF, borders #404040,
 *   accent #E8A03C (overridable), success #66B366, font Liberation.
 */

#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "cursor-shape-client-protocol.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Look & feel                                                        */
/* ------------------------------------------------------------------ */

/* Number of managed repositories. The window grows vertically to show every
 * row without scrolling, so adding a repo only changes this value and the
 * table below. */
#define REPO_COUNT 6

#define WIN_W 760.0

#define MARGIN 24.0
#define HEADER_H 58.0
#define HERO_Y 70.0
#define HERO_H 126.0
#define LIST_LABEL_Y 218.0
#define LIST_Y 238.0
#define ROW_H 62.0
#define ROW_GAP 8.0
#define PROGRESS_Y (LIST_Y + REPO_COUNT * (ROW_H + ROW_GAP) + 12.0)
#define PROGRESS_H 76.0
#define BUTTON_Y (PROGRESS_Y + PROGRESS_H + 14.0)
#define BUTTON_W 220.0
#define BUTTON_H 46.0

/* Footer sits just inside the bottom edge; the button keeps the same gap
 * above it that the original four-repo window had. */
#define WIN_H (BUTTON_Y + BUTTON_H + 34.0)
#define FOOTER_Y (WIN_H - 16.0)

#define FONT_FAMILY "Liberation"

#define C_BG        0x030303
#define C_BG_TOP    0x090A0B
#define C_FG        0xFFFFFF
#define C_DIM       0x9A9A9A
#define C_MUTED     0x707070
#define C_FAINT     0x5C5C5C
#define C_BORDER    0x404040
#define C_CARD      0x0D0E0F
#define C_CARD_HI   0x131416
#define C_TRACK     0x1A1B1D
#define C_GREEN     0x66B366
#define C_RED       0xD06B6B

/* The accent is shared by every accent surface.  It can be changed at launch
 * or live through jtupdatectl. */
static unsigned accent_rgb = 0xE8A03C;
static unsigned on_accent_rgb = 0x14100A;
#define C_ACCENT    accent_rgb
#define C_ON_ACCENT on_accent_rgb

#define PI 3.14159265358979323846

#define R_(h) ((((h) >> 16) & 0xFF) / 255.0)
#define G_(h) ((((h) >> 8) & 0xFF) / 255.0)
#define B_(h) (((h) & 0xFF) / 255.0)

static void
rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, R_(hex), G_(hex), B_(hex), a);
}

static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    double d = 2.0 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    r = d / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2.0, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
}

static double
mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double
clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int
accent_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int
accent_parse(const char *s, unsigned *out)
{
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (strlen(s) != 6)
        return 0;

    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = accent_hex_nibble(s[i]);
        if (nibble < 0)
            return 0;
        value = (value << 4) | (unsigned)nibble;
    }
    *out = value;
    return 1;
}

static double
accent_linear_channel(unsigned channel)
{
    double v = (double)channel / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double
accent_luminance(unsigned rgb)
{
    return 0.2126 * accent_linear_channel((rgb >> 16) & 0xFF) +
           0.7152 * accent_linear_channel((rgb >> 8) & 0xFF) +
           0.0722 * accent_linear_channel(rgb & 0xFF);
}

static double
accent_contrast(double a, double b)
{
    if (a < b) {
        double t = a;
        a = b;
        b = t;
    }
    return (a + 0.05) / (b + 0.05);
}

static unsigned
accent_ink(unsigned rgb)
{
    const unsigned dark = 0x14100A;
    const unsigned light = 0xF7F5F0;
    double background = accent_luminance(rgb);
    double dark_contrast = accent_contrast(background,
                                           accent_luminance(dark));
    double light_contrast = accent_contrast(background,
                                            accent_luminance(light));
    return light_contrast > dark_contrast ? light : dark;
}

static void
accent_set(unsigned rgb)
{
    accent_rgb = rgb;
    on_accent_rgb = accent_ink(rgb);
}

static int
accent_cli(int *argc, char **argv)
{
    for (int i = 1; i < *argc; i++) {
        const char *value = NULL;
        int consumed = 0;

        if (!strcmp(argv[i], "--accent")) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "%s: --accent requires a color\n", argv[0]);
                return 0;
            }
            value = argv[i + 1];
            consumed = 2;
        } else if (!strncmp(argv[i], "--accent=", 9)) {
            value = argv[i] + 9;
            consumed = 1;
        } else if (argv[i][0] == '#') {
            value = argv[i];
            consumed = 1;
        } else if (i == 1) {
            unsigned candidate;
            if (accent_parse(argv[i], &candidate)) {
                value = argv[i];
                consumed = 1;
            }
        }

        if (!value)
            continue;

        unsigned rgb;
        if (!accent_parse(value, &rgb)) {
            fprintf(stderr, "%s: invalid accent color '%s' "
                            "(expected #RRGGBB)\n", argv[0], value);
            return 0;
        }
        accent_set(rgb);

        for (int j = i; j + consumed < *argc; j++)
            argv[j] = argv[j + consumed];
        *argc -= consumed;
        i--;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Repositories                                                       */
/* ------------------------------------------------------------------ */

/* Parses --base-dir DIR / --base-dir=DIR, removes it from argv, and stores the
 * value in *out (NULL when the option was not given). */
static int
base_dir_cli(int *argc, char **argv, const char **out)
{
    *out = NULL;
    for (int i = 1; i < *argc; i++) {
        const char *value = NULL;
        int consumed = 0;

        if (!strcmp(argv[i], "--base-dir")) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "%s: --base-dir requires a directory\n",
                        argv[0]);
                return 0;
            }
            value = argv[i + 1];
            consumed = 2;
        } else if (!strncmp(argv[i], "--base-dir=", 11)) {
            value = argv[i] + 11;
            consumed = 1;
        } else {
            continue;
        }

        if (!value[0]) {
            fprintf(stderr, "%s: --base-dir requires a directory\n", argv[0]);
            return 0;
        }

        *out = value;
        for (int j = i; j + consumed < *argc; j++)
            argv[j] = argv[j + consumed];
        *argc -= consumed;
        i--;
    }
    return 1;
}

typedef struct {
    const char *name;
    const char *slug;
    const char *url;
    const char *build;
    const char *install;
} RepoDef;

/* Each checkout lives under <base-dir>/<slug>, where the base directory
 * defaults to ~/jtupdate/repos and can be overridden with --base-dir.  An
 * existing git checkout is pulled; changed repositories are built and
 * installed. */
static const RepoDef g_repos[REPO_COUNT] = {
    { "JTLab",       "JTLab",
      "https://github.com/arch-user-sudo/JTLab.git",       "make -j2", "make install" },
    { "jtspeedtest", "jtspeedtest",
      "https://github.com/arch-user-sudo/jtspeedtest.git", "make -j2", "make install" },
    { "jtnetworks",  "jtnetworks",
      "https://github.com/arch-user-sudo/jtnetworks.git",  "make -j2", "make install" },
    { "jtlabwc",     "jtlabwc",
      "https://github.com/arch-user-sudo/jtlabwc.git",     "make -j2", "make install" },
    { "jtupdate",    "jtupdater",
      "https://github.com/arch-user-sudo/jtupdater.git",   "make -j2", "make install" },
    { "jtlX",        "jtlX",
      "https://github.com/arch-user-sudo/jtlX.git",        "make -j2", "make install" },
};

static char g_repo_root[PATH_MAX];
static char g_repo_root_show[PATH_MAX];
static char g_repo_path[REPO_COUNT][PATH_MAX];
static char g_repo_short[REPO_COUNT][160];

/* Expands a leading "~" or "~/" to $HOME.  Other forms are copied as-is. */
static void
expand_tilde(const char *in, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/home/lynch";
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0'))
        snprintf(out, n, "%s%s", home, in + 1);
    else
        snprintf(out, n, "%s", in);
}

/* base_dir is the directory that directly holds the checkouts.  NULL or an
 * empty string keeps the historical ~/jtupdate/repos location.  The bar
 * passes this explicitly so the updater never has to infer it. */
static void
repo_paths_init(const char *base_dir)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/home/lynch";

    if (base_dir && base_dir[0])
        expand_tilde(base_dir, g_repo_root, sizeof g_repo_root);
    else
        snprintf(g_repo_root, sizeof g_repo_root, "%s/jtupdate/repos", home);

    size_t len = strlen(g_repo_root);
    while (len > 1 && g_repo_root[len - 1] == '/')
        g_repo_root[--len] = '\0';
    if (!g_repo_root[0])
        snprintf(g_repo_root, sizeof g_repo_root, "%s/jtupdate/repos", home);

    /* Show the path with ~ substituted when it lives under the home folder. */
    size_t home_len = strlen(home);
    if (home_len > 0 && !strncmp(g_repo_root, home, home_len) &&
        g_repo_root[home_len] == '/')
        snprintf(g_repo_root_show, sizeof g_repo_root_show,
                 "~%s", g_repo_root + home_len);
    else
        snprintf(g_repo_root_show, sizeof g_repo_root_show,
                 "%s", g_repo_root);

    for (int i = 0; i < REPO_COUNT; i++) {
        snprintf(g_repo_path[i], sizeof g_repo_path[i],
                 "%s/%s", g_repo_root, g_repos[i].slug);
        snprintf(g_repo_short[i], sizeof g_repo_short[i],
                 "%s/%s", g_repo_root_show, g_repos[i].slug);
    }
}

static int
mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return 0;
    memcpy(tmp, path, len + 1);

    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return 0;
        *p = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return 0;
    return 1;
}

static int
path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int
path_nonempty(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return 1;

    DIR *d = opendir(path);
    if (!d)
        return 1;

    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int
repo_is_git(const char *path)
{
    char dotgit[PATH_MAX];
    if (snprintf(dotgit, sizeof dotgit, "%s/.git", path) >=
        (int)sizeof dotgit)
        return 0;
    return access(dotgit, F_OK) == 0;
}

static int
ensure_parent(const char *path)
{
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof parent, "%s", path) >= (int)sizeof parent)
        return 0;
    char *slash = strrchr(parent, '/');
    if (!slash)
        return 1;
    *slash = '\0';
    if (!parent[0])
        return 1;
    return mkdir_p(parent);
}

/* Remove a file or directory tree without following symlinks. */
static int
rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return errno == ENOENT;

    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 || errno == ENOENT;

    DIR *d = opendir(path);
    if (!d)
        return 0;

    int ok = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", path, ent->d_name) >=
            (int)sizeof child) {
            ok = 0;
            continue;
        }
        if (!rm_rf(child))
            ok = 0;
    }
    closedir(d);

    if (rmdir(path) < 0 && errno != ENOENT)
        ok = 0;
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Background update state                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    RP_QUEUED,
    RP_CLONING,
    RP_PULLING,
    RP_BUILDING,
    RP_INSTALL,
    RP_DONE,
    RP_CURRENT,
    RP_ERROR,
    RP_SKIP
} RepoPhase;

typedef enum {
    UP_IDLE,
    UP_RUNNING,
    UP_DONE,
    UP_ERROR,
    UP_CANCELLED
} UpdatePhase;

typedef struct {
    RepoPhase phase;
    double progress;
    int updated;
    char detail[160];
} RepoState;

typedef struct {
    pthread_mutex_t lock;
    UpdatePhase phase;
    int running;
    int cancel;
    int current;
    double progress;
    int updated_count;
    int current_count;
    int error_count;
    int skipped_count;
    char status[192];
    RepoState repos[REPO_COUNT];
} UpdateState;

typedef struct {
    UpdatePhase phase;
    int running;
    int current;
    double progress;
    int updated_count;
    int current_count;
    int error_count;
    int skipped_count;
    char status[192];
    RepoState repos[REPO_COUNT];
} UpdateSnap;

typedef struct App App;

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height;
    int busy;
    int retired;
} ShmBuf;

typedef struct {
    struct wl_output *output;
    int32_t scale;
    int entered;
    App *app;
} Output;

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    uint32_t pointer_serial;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct xdg_toplevel_icon_manager_v1 *icon_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    uint32_t cursor_shape;

    int configured;
    int scale;
    int fixed_scale;
    int has_argb;
    ShmBuf bufs[4];

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    Output outputs[8];
    int output_count;

    UpdateState upd;
    pthread_t worker;
    int worker_alive;

    double t0;
    double disp;
    double done_t;
    UpdatePhase prev_phase;

    int hover_primary;
    int press_primary;
    int hover_close;
    int hover_repo;
    int quit;

    double pointer_x, pointer_y;
    double last_draw;

    int ctl_fd;
    int ctl_client_fd;
    char ctl_buf[256];
    size_t ctl_len;
};

static void start_update(App *a);

/* ------------------------------------------------------------------ */
/*  State helpers                                                      */
/* ------------------------------------------------------------------ */

static void
trim_ws(char *s)
{
    if (!s)
        return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);

    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static void
state_init(UpdateState *st)
{
    memset(st, 0, sizeof *st);
    pthread_mutex_init(&st->lock, NULL);
    st->phase = UP_IDLE;
    st->current = -1;
    st->progress = 0.0;
    snprintf(st->status, sizeof st->status, "Ready to check for updates.");
    for (int i = 0; i < REPO_COUNT; i++) {
        st->repos[i].phase = RP_QUEUED;
        st->repos[i].progress = 0.0;
        snprintf(st->repos[i].detail, sizeof st->repos[i].detail,
                 "%s", g_repo_short[i]);
    }
}

static void
state_set_status(UpdateState *st, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&st->lock);
    snprintf(st->status, sizeof st->status, "%s", buf);
    pthread_mutex_unlock(&st->lock);
}

static void
repo_update(UpdateState *st, int i, RepoPhase phase, double progress,
            const char *detail)
{
    if (i < 0 || i >= REPO_COUNT)
        return;
    progress = clampd(progress, 0.0, 1.0);

    pthread_mutex_lock(&st->lock);
    RepoState *r = &st->repos[i];
    r->phase = phase;
    r->progress = progress;
    if (detail)
        snprintf(r->detail, sizeof r->detail, "%s", detail);
    st->progress = ((double)i + progress) / (double)REPO_COUNT;
    pthread_mutex_unlock(&st->lock);
}

static void
finish_repo(UpdateState *st, int i, RepoPhase phase, const char *detail)
{
    if (i < 0 || i >= REPO_COUNT)
        return;

    pthread_mutex_lock(&st->lock);
    RepoState *r = &st->repos[i];
    r->phase = phase;
    r->progress = 1.0;
    r->updated = phase == RP_DONE;
    snprintf(r->detail, sizeof r->detail, "%s", detail ? detail : "");

    switch (phase) {
    case RP_DONE:    st->updated_count++; break;
    case RP_CURRENT: st->current_count++; break;
    case RP_ERROR:   st->error_count++; break;
    case RP_SKIP:    st->skipped_count++; break;
    default: break;
    }
    st->progress = ((double)i + 1.0) / (double)REPO_COUNT;
    pthread_mutex_unlock(&st->lock);
}

static int
state_cancelled(UpdateState *st)
{
    pthread_mutex_lock(&st->lock);
    int cancelled = st->cancel;
    pthread_mutex_unlock(&st->lock);
    return cancelled;
}

static void
state_begin(UpdateState *st)
{
    pthread_mutex_lock(&st->lock);
    st->phase = UP_RUNNING;
    st->running = 1;
    st->cancel = 0;
    st->current = -1;
    st->progress = 0.0;
    st->updated_count = 0;
    st->current_count = 0;
    st->error_count = 0;
    st->skipped_count = 0;
    snprintf(st->status, sizeof st->status, "Checking repositories…");

    for (int i = 0; i < REPO_COUNT; i++) {
        RepoState *r = &st->repos[i];
        r->phase = RP_QUEUED;
        r->progress = 0.0;
        r->updated = 0;
        if (!g_repos[i].url[0] && !repo_is_git(g_repo_path[i]))
            snprintf(r->detail, sizeof r->detail,
                     "Add the repository URL in jtupdate.c");
        else
            snprintf(r->detail, sizeof r->detail, "Waiting to check");
    }
    pthread_mutex_unlock(&st->lock);
}

static void
state_cancel(UpdateState *st)
{
    pthread_mutex_lock(&st->lock);
    if (st->running && !st->cancel) {
        st->cancel = 1;
        snprintf(st->status, sizeof st->status,
                 "Stopping after the current step…");
    }
    pthread_mutex_unlock(&st->lock);
}

static void
state_finish(UpdateState *st, int cancelled)
{
    pthread_mutex_lock(&st->lock);
    st->running = 0;
    st->cancel = 0;

    if (cancelled) {
        st->phase = UP_CANCELLED;
        snprintf(st->status, sizeof st->status, "Update stopped.");
    } else if (st->error_count > 0) {
        st->phase = UP_ERROR;
        st->progress = 1.0;
        snprintf(st->status, sizeof st->status,
                 "Finished with %d issue%s.",
                 st->error_count, st->error_count == 1 ? "" : "s");
    } else if (st->skipped_count == REPO_COUNT) {
        st->phase = UP_DONE;
        st->progress = 1.0;
        snprintf(st->status, sizeof st->status,
                 "Add the repository URLs in jtupdate.c.");
    } else if (st->updated_count > 0) {
        st->phase = UP_DONE;
        st->progress = 1.0;
        if (st->skipped_count > 0)
            snprintf(st->status, sizeof st->status,
                     "Update complete — %d updated, %d skipped.",
                     st->updated_count, st->skipped_count);
        else
            snprintf(st->status, sizeof st->status,
                     "Update complete — %d updated.", st->updated_count);
    } else if (st->skipped_count > 0) {
        st->phase = UP_DONE;
        st->progress = 1.0;
        snprintf(st->status, sizeof st->status,
                 "Finished — %d skipped, %d current.",
                 st->skipped_count, st->current_count);
    } else {
        st->phase = UP_DONE;
        st->progress = 1.0;
        snprintf(st->status, sizeof st->status,
                 "Everything is up to date.");
    }
    pthread_mutex_unlock(&st->lock);
}

static void
state_snapshot(UpdateState *st, UpdateSnap *s)
{
    pthread_mutex_lock(&st->lock);
    s->phase = st->phase;
    s->running = st->running;
    s->current = st->current;
    s->progress = st->progress;
    s->updated_count = st->updated_count;
    s->current_count = st->current_count;
    s->error_count = st->error_count;
    s->skipped_count = st->skipped_count;
    snprintf(s->status, sizeof s->status, "%s", st->status);
    for (int i = 0; i < REPO_COUNT; i++)
        s->repos[i] = st->repos[i];
    pthread_mutex_unlock(&st->lock);
}

/* ------------------------------------------------------------------ */
/*  Child processes                                                    */
/* ------------------------------------------------------------------ */

static pid_t
spawn_pipe(char *const argv[], const char *cwd, int *out_fd)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        setpgid(0, 0);
        if (cwd && chdir(cwd) < 0) {
            dprintf(STDERR_FILENO, "jtupdate: cannot enter %s: %s\n",
                    cwd, strerror(errno));
            _exit(127);
        }
        if (dup2(fds[1], STDOUT_FILENO) < 0 ||
            dup2(fds[1], STDERR_FILENO) < 0)
            _exit(127);
        close(fds[0]);
        close(fds[1]);

        setenv("LC_ALL", "C", 1);
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        setenv("GIT_ASKPASS", "/bin/false", 1);

        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "jtupdate: cannot run %s: %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }

    close(fds[1]);
    *out_fd = fds[0];
    return pid;
}

static int
run_capture(char *const argv[], const char *cwd, char *out, size_t outn)
{
    if (!out || outn == 0)
        return 0;
    out[0] = '\0';

    int fd = -1;
    pid_t pid = spawn_pipe(argv, cwd, &fd);
    if (pid < 0)
        return 0;

    size_t n = 0;
    char buf[512];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r > 0) {
            size_t room = outn - 1 - n;
            if (room > 0) {
                size_t take = (size_t)r < room ? (size_t)r : room;
                memcpy(out + n, buf, take);
                n += take;
            }
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    out[n] = '\0';
    trim_ws(out);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
repo_dirty(const char *path)
{
    char out[1024];
    char *argv[] = { "git", "status", "--porcelain",
                     "--untracked-files=no", NULL };
    if (!run_capture(argv, path, out, sizeof out))
        return 0;
    return out[0] != '\0';
}

static int
repo_head(const char *path, char *out, size_t outn)
{
    char *argv[] = { "git", "rev-parse", "HEAD", NULL };
    if (outn == 0)
        return 0;
    out[0] = '\0';
    return run_capture(argv, path, out, outn) && out[0] != '\0';
}

/* A checkout is only usable once HEAD resolves.  A clone interrupted midway
 * can leave a .git directory behind, so checking for .git alone is not enough. */
static int
repo_valid(const char *path)
{
    char head[80];
    return repo_is_git(path) && repo_head(path, head, sizeof head);
}

static int
line_percent(const char *s, int *pct)
{
    const char *mark = strrchr(s, '%');
    if (!mark)
        return 0;
    const char *p = mark;
    while (p > s && isdigit((unsigned char)p[-1]))
        p--;
    if (p == mark)
        return 0;
    int v = atoi(p);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    *pct = v;
    return 1;
}

static void
command_line(UpdateState *st, int idx, RepoPhase phase,
             const char *raw, double p0, double p1, double *progress)
{
    char line[160];
    snprintf(line, sizeof line, "%s", raw);
    trim_ws(line);
    if (!line[0])
        return;

    int pct;
    if (line_percent(line, &pct))
        *progress = p0 + (p1 - p0) * ((double)pct / 100.0);
    else
        *progress += (p1 - *progress) * 0.060;

    if (*progress > p1 - 0.005)
        *progress = p1 - 0.005;
    if (*progress < p0)
        *progress = p0;

    repo_update(st, idx, phase, *progress, line);
}

/* Returns 1 on success, 0 on failure, -1 when cancelled. */
static int
run_command(App *a, int idx, char *const argv[], const char *cwd,
            RepoPhase phase, double p0, double p1)
{
    UpdateState *st = &a->upd;
    int fd = -1;
    pid_t pid = spawn_pipe(argv, cwd, &fd);
    if (pid < 0) {
        repo_update(st, idx, phase, p0, "Could not start the command");
        return 0;
    }

    double progress = p0;
    char line[1024];
    size_t line_len = 0;
    int cancelled = 0;
    int killed = 0;
    int killed9 = 0;
    double kill_at = 0.0;

    for (;;) {
        if (!cancelled && state_cancelled(st)) {
            cancelled = 1;
            killed = 1;
            kill_at = mono_sec();
            kill(-pid, SIGTERM);
            kill(pid, SIGTERM);
            state_set_status(st, "Stopping…");
        }
        if (killed && !killed9 && mono_sec() - kill_at > 2.0) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            killed9 = 1;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 120);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            double next = progress + (p1 - progress) * 0.012;
            if (next > p1 - 0.01)
                next = p1 - 0.01;
            if (next > progress) {
                progress = next;
                repo_update(st, idx, phase, progress, NULL);
            }
            continue;
        }

        char buf[2048];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_len > 0) {
                        line[line_len] = '\0';
                        command_line(st, idx, phase, line,
                                     p0, p1, &progress);
                        line_len = 0;
                    }
                } else if (line_len + 1 < sizeof line) {
                    line[line_len++] = c;
                }
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        command_line(st, idx, phase, line, p0, p1, &progress);
    }
    close(fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (cancelled || state_cancelled(st))
        return -1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        repo_update(st, idx, phase, p1, NULL);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Update worker                                                      */
/* ------------------------------------------------------------------ */

static void
worker_repo_error(UpdateState *st, int i, const char *detail)
{
    finish_repo(st, i, RP_ERROR, detail);
}

/* Clone into a temporary sibling first, then move it into place.  A failed or
 * cancelled clone therefore never leaves a half-populated target behind that
 * would block every later update.  An existing non-git folder is moved aside
 * instead of being deleted. */
static int
clone_checkout(App *a, int i, const RepoDef *def, const char *path,
               char *err, size_t errn)
{
    UpdateState *st = &a->upd;
    if (err && errn)
        err[0] = '\0';

    if (!ensure_parent(path)) {
        if (err && errn)
            snprintf(err, errn, "Could not create the target folder");
        return 0;
    }

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.clone-%ld", path, (long)getpid()) >=
        (int)sizeof tmp) {
        if (err && errn)
            snprintf(err, errn, "Repository path is too long");
        return 0;
    }
    rm_rf(tmp);

    state_set_status(st, "Cloning %s…", def->name);
    repo_update(st, i, RP_CLONING, 0.0, "Starting git clone…");
    char *argv[] = { "git", "clone", "--progress",
                     (char *)def->url, tmp, NULL };
    int rc = run_command(a, i, argv, NULL, RP_CLONING, 0.0, 0.55);
    if (rc <= 0) {
        rm_rf(tmp);
        if (rc == 0 && err && errn)
            snprintf(err, errn, "git clone failed");
        return rc;
    }

    if (!repo_is_git(tmp)) {
        /* Some filesystems settle the final directory a moment after git
         * exits; check once more before declaring the clone bad. */
        usleep(100000);
    }
    if (!repo_is_git(tmp) || !repo_valid(tmp)) {
        rm_rf(tmp);
        if (err && errn)
            snprintf(err, errn, "Clone finished without a usable checkout");
        return 0;
    }

    if (path_exists(path)) {
        if (repo_valid(path)) {
            /* Another run got there first; keep its checkout. */
            rm_rf(tmp);
            return 1;
        }
        if (path_nonempty(path)) {
            char old[PATH_MAX];
            if (snprintf(old, sizeof old, "%s.incomplete-%ld-%ld", path,
                         (long)time(NULL), (long)getpid()) >=
                (int)sizeof old ||
                rename(path, old) < 0) {
                rm_rf(tmp);
                if (err && errn)
                    snprintf(err, errn,
                             "Could not move the old checkout aside");
                return 0;
            }
        } else if (rmdir(path) < 0 && errno != ENOENT) {
            rm_rf(tmp);
            if (err && errn)
                snprintf(err, errn, "Could not replace the empty folder");
            return 0;
        }
    }

    if (rename(tmp, path) < 0) {
        rm_rf(tmp);
        if (err && errn)
            snprintf(err, errn, "Could not move the clone into place");
        return 0;
    }
    return 1;
}

/* True when an executable can be found through PATH. */
static int
command_exists(const char *name)
{
    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy)
        return 0;

    int found = 0;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", dir, name) >=
            (int)sizeof full)
            continue;
        if (access(full, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

/* A cached doas/sudo timestamp lets the install run without a terminal. */
static int
elevated_cached(const char *tool)
{
    char out[64];
    char *argv[] = { (char *)tool, "-n", "true", NULL };
    return run_capture(argv, NULL, out, sizeof out);
}

static int
run_build(App *a, int i, const RepoDef *def, const char *path,
          double p0, double p1)
{
    if (!def->build || !def->build[0])
        return 1;
    state_set_status(&a->upd, "Building %s…", def->name);
    repo_update(&a->upd, i, RP_BUILDING, p0, "Running the build…");
    char *argv[] = { "/bin/sh", "-c", (char *)def->build, NULL };
    return run_command(a, i, argv, path, RP_BUILDING, p0, p1);
}

/* The helper installed next to jtupdate performs all of the actual installs.
 * It is also the script that `make install` puts in the updater source tree. */
static int
find_install_helper(char *out, size_t n)
{
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            const char *names[] = {
                "jtupdate-install",
                "jtupdate-install.sh",
            };
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
                int k = snprintf(out, n, "%s/%s", exe, names[i]);
                if (k > 0 && (size_t)k < n && access(out, X_OK) == 0)
                    return 1;
            }
        }
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        const char *paths[] = {
            "%s/jtupdate/jtupdate-install.sh",
            "%s/git/jtupdater/jtupdate-install.sh",
        };
        for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
            int k = snprintf(out, n, paths[i], home);
            if (k > 0 && (size_t)k < n && access(out, X_OK) == 0)
                return 1;
        }
    }
    return 0;
}

/* When jtupdate is running from its source tree, install that tree instead of
 * the cached jtupdater checkout.  This keeps a source build from being replaced
 * by an older clone of the same repository. */
static int
find_self_source_dir(char *out, size_t n)
{
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len <= 0)
        return 0;
    exe[len] = '\0';

    char *slash = strrchr(exe, '/');
    if (!slash || slash == exe)
        return 0;
    *slash = '\0';

    char check[PATH_MAX];
    int k = snprintf(check, sizeof check, "%s/Makefile", exe);
    if (k <= 0 || (size_t)k >= sizeof check || access(check, F_OK) != 0)
        return 0;
    k = snprintf(check, sizeof check, "%s/jtupdate.c", exe);
    if (k <= 0 || (size_t)k >= sizeof check || access(check, F_OK) != 0)
        return 0;
    if (strlen(exe) >= n)
        return 0;
    snprintf(out, n, "%s", exe);
    return 1;
}

/* "" means we are root.  "doas"/"sudo" means that tool has a cached
 * timestamp, so the helper can run without a terminal.  NULL means the helper
 * must be started in a terminal so it can authenticate. */
static const char *
cached_elevation(void)
{
    if (geteuid() == 0)
        return "";
    if (command_exists("doas") && elevated_cached("doas"))
        return "doas";
    if (command_exists("sudo") && elevated_cached("sudo"))
        return "sudo";
    return NULL;
}

static void
remove_install_temp(const char *plan, const char *statusdir, int count)
{
    if (plan)
        unlink(plan);
    if (statusdir) {
        for (int p = 0; p < count; p++) {
            char status[PATH_MAX];
            snprintf(status, sizeof status, "%s/%d.status", statusdir, p);
            unlink(status);
        }
        rmdir(statusdir);
    }
}

/* Run one helper for every queued repository.  The helper checks doas, sudo,
 * then pkexec and re-executes itself once, so all installs share one
 * authorization prompt and one terminal window.
 * Per-repo exit codes are read back from status files. */
static int
run_helper_installs(App *a, const int *idxs, const int *cloned, int count,
                    const char *elev)
{
    UpdateState *st = &a->upd;
    if (count <= 0)
        return 1;

    char helper[PATH_MAX];
    if (!find_install_helper(helper, sizeof helper)) {
        for (int p = 0; p < count; p++)
            finish_repo(st, idxs[p], RP_ERROR,
                        "Install helper missing — run make install");
        return 0;
    }

    if (!mkdir_p(g_repo_root)) {
        for (int p = 0; p < count; p++)
            finish_repo(st, idxs[p], RP_ERROR,
                        "Could not create the repos folder");
        return 0;
    }

    char plan[PATH_MAX];
    char statusdir[PATH_MAX];
    snprintf(plan, sizeof plan, "%s/.install-%ld.plan",
             g_repo_root, (long)getpid());
    snprintf(statusdir, sizeof statusdir, "%s/.install-%ld.status",
             g_repo_root, (long)getpid());
    unlink(plan);
    mkdir(statusdir, 0755);

    FILE *f = fopen(plan, "w");
    if (!f) {
        for (int p = 0; p < count; p++)
            finish_repo(st, idxs[p], RP_ERROR,
                        "Could not create the install plan");
        remove_install_temp(NULL, statusdir, count);
        return 0;
    }
    char self_source[PATH_MAX];
    int have_self_source = find_self_source_dir(self_source,
                                                sizeof self_source);
    for (int p = 0; p < count; p++) {
        const char *path = g_repo_path[idxs[p]];
        if (have_self_source &&
            strcmp(g_repos[idxs[p]].slug, "jtupdater") == 0)
            path = self_source;
        fprintf(f, "%s\n", path);
    }
    fclose(f);

    for (int p = 0; p < count; p++) {
        char status[PATH_MAX];
        snprintf(status, sizeof status, "%s/%d.status", statusdir, p);
        unlink(status);
        repo_update(st, idxs[p], RP_INSTALL, 0.90,
                    "Waiting for authorization");
    }

    const char *term = NULL;
    if (command_exists("foot"))
        term = "foot";
    else if (command_exists("footclient"))
        term = "footclient";

    char *argv[12];
    int ai = 0;
    int terminal = 0;

    if (elev && !elev[0]) {
        argv[ai++] = helper;
    } else if (elev) {
        argv[ai++] = (char *)elev;
        argv[ai++] = "-n";
        argv[ai++] = helper;
    } else {
        if (!term) {
            remove_install_temp(plan, statusdir, count);
            for (int p = 0; p < count; p++)
                finish_repo(st, idxs[p], RP_ERROR,
                            "Install needs doas/sudo and a terminal");
            return 0;
        }
        argv[ai++] = (char *)term;
        argv[ai++] = "-T";
        argv[ai++] = "jtupdate install";
        argv[ai++] = helper;
        terminal = 1;
    }
    argv[ai++] = "--plan";
    argv[ai++] = plan;
    argv[ai++] = "--status-dir";
    argv[ai++] = statusdir;
    argv[ai] = NULL;

    if (terminal)
        state_set_status(st, "Installing %d repositories — authorize in "
                         "the terminal…", count);
    else
        state_set_status(st, "Installing %d repositories…", count);

    for (int p = 0; p < count; p++)
        repo_update(st, idxs[p], RP_INSTALL, 0.92, "Installing binaries…");

    int rc = run_command(a, idxs[0], argv, NULL, RP_INSTALL, 0.92, 1.0);
    if (rc < 0) {
        remove_install_temp(plan, statusdir, count);
        for (int p = 0; p < count; p++)
            finish_repo(st, idxs[p], RP_SKIP, "Install cancelled");
        return -1;
    }

    for (int p = 0; p < count; p++) {
        int code = -1;
        char status[PATH_MAX];
        snprintf(status, sizeof status, "%s/%d.status", statusdir, p);
        FILE *sf = fopen(status, "r");
        if (sf) {
            if (fscanf(sf, "%d", &code) != 1)
                code = -1;
            fclose(sf);
        }
        unlink(status);

        int i = idxs[p];
        if (code == 0) {
            finish_repo(st, i, RP_DONE, cloned[p]
                        ? "Cloned, built and installed"
                        : "Pulled, built and installed");
        } else {
            finish_repo(st, i, RP_ERROR, "Install failed");
        }
    }
    remove_install_temp(plan, statusdir, count);
    return 1;
}

/* Queue a changed repository for the shared install helper. */
static int
finish_after_build(App *a, int i, const RepoDef *def, int cloned,
                   int *pending, int *pending_cloned, int *pending_count)
{
    if (!def->install || !def->install[0]) {
        finish_repo(&a->upd, i, RP_DONE,
                    cloned ? "Cloned and built" : "Pulled and built");
        return 1;
    }

    if (cloned && strcmp(def->slug, "jtupdater") == 0) {
        char self_source[PATH_MAX];
        if (!find_self_source_dir(self_source, sizeof self_source)) {
            finish_repo(&a->upd, i, RP_DONE,
                        "Cloned and built — install manually");
            return 1;
        }
    }

    pending[*pending_count] = i;
    pending_cloned[*pending_count] = cloned;
    (*pending_count)++;
    repo_update(&a->upd, i, RP_INSTALL, 0.85,
                "Built — queued for install");
    return 1;
}

static void *
update_worker(void *arg)
{
    App *a = arg;
    UpdateState *st = &a->upd;
    int pending[REPO_COUNT];
    int pending_cloned[REPO_COUNT];
    int pending_count = 0;

    for (int i = 0; i < REPO_COUNT; i++) {
        if (state_cancelled(st))
            goto out;

        const RepoDef *def = &g_repos[i];
        const char *path = g_repo_path[i];

        pthread_mutex_lock(&st->lock);
        st->current = i;
        pthread_mutex_unlock(&st->lock);

        state_set_status(st, "Checking %s…", def->name);
        repo_update(st, i, RP_QUEUED, 0.0, "Checking the checkout…");

        int is_git = repo_valid(path);

        if (!is_git) {
            if (!def->url[0]) {
                finish_repo(st, i, RP_SKIP,
                            "No URL configured in jtupdate.c");
                continue;
            }

            char err[160] = "";
            int rc = clone_checkout(a, i, def, path, err, sizeof err);
            if (rc < 0)
                goto out;
            if (rc == 0) {
                worker_repo_error(st, i,
                                  err[0] ? err : "git clone failed");
                continue;
            }

            is_git = repo_valid(path);
            if (!is_git) {
                worker_repo_error(st, i, "Clone finished without .git");
                continue;
            }

            rc = run_build(a, i, def, path, 0.55, 0.85);
            if (rc < 0)
                goto out;
            if (rc == 0) {
                worker_repo_error(st, i, "Build failed");
                continue;
            }
            if (finish_after_build(a, i, def, 1, pending, pending_cloned,
                                   &pending_count) < 0)
                goto out;
            continue;
        }

        if (repo_dirty(path)) {
            finish_repo(st, i, RP_CURRENT,
                        "Local changes — fast-forward pull skipped");
            continue;
        }

        char before[80] = "";
        repo_head(path, before, sizeof before);

        state_set_status(st, "Pulling %s…", def->name);
        repo_update(st, i, RP_PULLING, 0.0, "Pulling from origin…");
        char *pargv[] = { "git", "pull", "--ff-only", "--progress", NULL };
        int rc = run_command(a, i, pargv, path, RP_PULLING, 0.0, 0.55);
        if (rc < 0)
            goto out;
        if (rc == 0) {
            worker_repo_error(st, i, "git pull failed");
            continue;
        }

        char after[80] = "";
        repo_head(path, after, sizeof after);
        int changed = before[0] && after[0] && strcmp(before, after) != 0;

        if (!changed) {
            finish_repo(st, i, RP_CURRENT, "Already up to date");
            continue;
        }

        rc = run_build(a, i, def, path, 0.55, 0.85);
        if (rc < 0)
            goto out;
        if (rc == 0) {
            worker_repo_error(st, i, "Build failed");
            continue;
        }
        if (finish_after_build(a, i, def, 0, pending, pending_cloned,
                               &pending_count) < 0)
            goto out;
    }

    if (run_helper_installs(a, pending, pending_cloned, pending_count,
                            cached_elevation()) < 0) {
        pending_count = 0;
        goto out;
    }

out:
    if (pending_count > 0 && state_cancelled(st)) {
        for (int p = 0; p < pending_count; p++)
            finish_repo(st, pending[p], RP_SKIP, "Install cancelled");
    }
    state_finish(st, state_cancelled(st));
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

typedef struct {
    int key;
    PangoFontDescription *fd;
} FontEnt;

#define FONT_CACHE 256
static FontEnt g_fonts[FONT_CACHE];
static int g_font_count;

static PangoFontDescription *
font_get(double size, int weight)
{
    int key = (int)lround(size * 10.0) * 10000 + weight;

    for (int i = 0; i < g_font_count; i++)
        if (g_fonts[i].key == key)
            return g_fonts[i].fd;

    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);

    if (g_font_count < FONT_CACHE) {
        g_fonts[g_font_count].key = key;
        g_fonts[g_font_count].fd = fd;
        g_font_count++;
    }
    return fd;
}

static PangoLayout *
layout_make(cairo_t *cr, const char *text, double size, int weight,
            double spacing)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, font_get(size, weight));
    pango_layout_set_text(l, text, -1);

    if (spacing > 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_letter_spacing_new((int)lround(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static double
text_width(cairo_t *cr, const char *text, double size, int weight,
           double spacing)
{
    if (!text || !*text)
        return 0.0;
    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    g_object_unref(l);
    return w;
}

static double
text_draw(cairo_t *cr, const char *text, double size, int weight,
          unsigned hex, double alpha, double x, double y,
          int align, double spacing)
{
    if (!text || !*text)
        return 0.0;

    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);

    double w = logical.width;
    if (align == ALIGN_CENTER)
        x -= w / 2.0;
    else if (align == ALIGN_RIGHT)
        x -= w;

    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

static void
text_draw_wrapped(cairo_t *cr, const char *text, double size, int weight,
                  unsigned hex, double alpha, double x, double y,
                  double width, double spacing)
{
    if (!text || !*text)
        return;
    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    pango_layout_set_width(l, (int)lround(width * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const UpdateSnap *s;
    double t;
    double disp;
    int hover_primary;
    int press_primary;
    int hover_close;
    int hover_repo;
    double done_t;
} View;

#define HERO_CX 96.0
#define HERO_CY 134.0
#define HERO_R  42.0

static int
repo_active(RepoPhase ph)
{
    return ph == RP_CLONING || ph == RP_PULLING || ph == RP_BUILDING ||
           ph == RP_INSTALL;
}

static unsigned
repo_color(RepoPhase ph)
{
    switch (ph) {
    case RP_CLONING:
    case RP_PULLING:
    case RP_BUILDING:
    case RP_INSTALL:
        return C_ACCENT;
    case RP_DONE:
    case RP_CURRENT:
        return C_GREEN;
    case RP_ERROR:
        return C_RED;
    case RP_SKIP:
        return C_FAINT;
    case RP_QUEUED:
    default:
        return C_MUTED;
    }
}

static const char *
repo_pill(RepoPhase ph)
{
    switch (ph) {
    case RP_CLONING: return "CLONE";
    case RP_PULLING: return "PULL";
    case RP_BUILDING: return "BUILD";
    case RP_INSTALL: return "INSTALL";
    case RP_DONE: return "UPDATED";
    case RP_CURRENT: return "CURRENT";
    case RP_ERROR: return "ERROR";
    case RP_SKIP: return "NO URL";
    case RP_QUEUED:
    default: return "QUEUED";
    }
}

static void
paint_ambient(cairo_t *cr, const View *v, int running)
{
    double breath = 0.5 + 0.5 * sin(v->t * 0.55);
    double glow = (running ? 0.075 : 0.038) * (0.72 + 0.28 * breath);

    cairo_pattern_t *pat = cairo_pattern_create_radial(
        HERO_CX, HERO_CY, 12.0, HERO_CX, HERO_CY, 218.0);
    cairo_pattern_add_color_stop_rgba(pat, 0.00,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), glow);
    cairo_pattern_add_color_stop_rgba(pat, 0.55,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), glow * 0.28);
    cairo_pattern_add_color_stop_rgba(pat, 1.00,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.0);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);

    for (int k = 0; k < 3; k++) {
        double r = HERO_R + 9.0 + k * 10.0 +
                   sin(v->t * 0.5 + k) * 2.2;
        double a = (0.030 + 0.020 * (0.5 + 0.5 *
                   sin(v->t * 0.55 + k * 1.2))) * (running ? 1.7 : 1.0);
        rgba(cr, k == 1 ? C_FG : C_ACCENT, a);
        cairo_set_line_width(cr, 1.0);
        cairo_arc(cr, HERO_CX, HERO_CY, r, 0.0, 2.0 * PI);
        cairo_stroke(cr);
    }

    for (int i = 0; i < 16; i++) {
        double seed = (double)(i + 1) * 12.9898;
        double x = 26.0 + fmod(seed * 37.719, 700.0);
        double y = 78.0 + fmod(seed * 19.19, 110.0) +
                   sin(v->t * 0.33 + i * 1.7) * 4.0;
        double a = (0.025 + 0.055 * (0.5 + 0.5 *
                   sin(v->t * 0.5 + i * 2.1))) * (running ? 1.5 : 1.0);
        unsigned col = (i % 6 == 2) ? C_ACCENT : C_FG;
        rgba(cr, col, a);
        cairo_arc(cr, x, y, 0.7 + fmod(seed * 5.7, 1.1), 0.0, 2.0 * PI);
        cairo_fill(cr);
    }
}

static void
paint_hero_icon(cairo_t *cr, const View *v, int running)
{
    (void)v;
    cairo_pattern_t *fill = cairo_pattern_create_radial(
        HERO_CX, HERO_CY - 12.0, 4.0, HERO_CX, HERO_CY, HERO_R);
    cairo_pattern_add_color_stop_rgba(fill, 0.0,
        R_(C_CARD_HI), G_(C_CARD_HI), B_(C_CARD_HI), 0.98);
    cairo_pattern_add_color_stop_rgba(fill, 1.0,
        R_(0x08090A), G_(0x08090A), B_(0x08090A), 0.98);
    cairo_set_source(cr, fill);
    cairo_arc(cr, HERO_CX, HERO_CY, HERO_R, 0.0, 2.0 * PI);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);

    rgba(cr, C_ACCENT, running ? 0.58 : 0.40);
    cairo_set_line_width(cr, 1.2);
    cairo_arc(cr, HERO_CX, HERO_CY, HERO_R - 0.6, 0.0, 2.0 * PI);
    cairo_stroke(cr);

    /* A small git branch mark. */
    double cx = HERO_CX;
    double cy = HERO_CY;
    rgba(cr, C_ACCENT, 0.92);
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_move_to(cr, cx - 11.0, cy - 16.0);
    cairo_line_to(cr, cx - 11.0, cy + 16.0);
    cairo_stroke(cr);

    cairo_move_to(cr, cx - 11.0, cy + 4.0);
    cairo_curve_to(cr, cx - 11.0, cy - 4.0,
                   cx + 2.0, cy - 5.0,
                   cx + 11.0, cy - 10.0);
    cairo_stroke(cr);

    struct { double x, y; } nodes[3] = {
        { cx - 11.0, cy - 16.0 },
        { cx - 11.0, cy + 16.0 },
        { cx + 11.0, cy - 10.0 },
    };
    for (int i = 0; i < 3; i++) {
        rgba(cr, C_ACCENT, 0.16);
        cairo_arc(cr, nodes[i].x, nodes[i].y, 6.0, 0.0, 2.0 * PI);
        cairo_fill(cr);
        rgba(cr, C_ACCENT, 0.96);
        cairo_arc(cr, nodes[i].x, nodes[i].y, 3.3, 0.0, 2.0 * PI);
        cairo_fill(cr);
        if (i == 0) {
            rgba(cr, C_CARD, 0.95);
            cairo_arc(cr, nodes[i].x, nodes[i].y, 1.1, 0.0, 2.0 * PI);
            cairo_fill(cr);
        }
    }
}

static void
paint_header(cairo_t *cr, const View *v)
{
    for (int i = 0; i < 4; i++) {
        double h = 6.0 + i * 3.0;
        rgba(cr, C_ACCENT, 0.55 + i * 0.12);
        rounded_rect(cr, 24.0 + i * 5.0, 36.0 - h, 3.0, h, 1.4);
        cairo_fill(cr);
    }

    double w1 = text_draw(cr, "JTLab", 15.0, 700, C_FG, 0.96,
                          49.0, 19.0, ALIGN_LEFT, 0.1);
    text_draw(cr, "Updater", 15.0, 400, C_DIM, 0.86,
              49.0 + w1 + 5.0, 19.0, ALIGN_LEFT, 0.1);
    text_draw(cr, "repositories · git · build · install", 9.0, 400, C_FAINT, 0.75,
              49.0, 39.0, ALIGN_LEFT, 0.55);

    rgba(cr, C_FG, 0.045);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 24.0, 56.5);
    cairo_line_to(cr, WIN_W - 24.0, 56.5);
    cairo_stroke(cr);

    double ccx = WIN_W - 30.0;
    double ccy = 28.0;
    if (v->hover_close) {
        rgba(cr, C_FG, 0.07);
        cairo_arc(cr, ccx, ccy, 15.0, 0.0, 2.0 * PI);
        cairo_fill(cr);
    }
    rgba(cr, v->hover_close ? C_FG : C_DIM, v->hover_close ? 0.9 : 0.62);
    cairo_set_line_width(cr, 1.35);
    cairo_move_to(cr, ccx - 5.0, ccy - 5.0);
    cairo_line_to(cr, ccx + 5.0, ccy + 5.0);
    cairo_move_to(cr, ccx + 5.0, ccy - 5.0);
    cairo_line_to(cr, ccx - 5.0, ccy + 5.0);
    cairo_stroke(cr);
}

static void
paint_hero(cairo_t *cr, const View *v, int running)
{
    const UpdateSnap *s = v->s;
    paint_hero_icon(cr, v, running);

    const char *title = "Ready to update";
    if (s->phase == UP_RUNNING)
        title = "Updating repositories";
    else if (s->phase == UP_DONE)
        title = s->skipped_count == REPO_COUNT
                ? "Add repository URLs" : "Update complete";
    else if (s->phase == UP_ERROR)
        title = "Finished with issues";
    else if (s->phase == UP_CANCELLED)
        title = "Update stopped";

    double tx = 164.0;
    text_draw(cr, "REPOSITORY UPDATER", 9.0, 600, C_ACCENT, 0.90,
              tx, 90.0, ALIGN_LEFT, 2.2);
    text_draw(cr, title, 24.0, 700, C_FG, 0.97,
              tx, 106.0, ALIGN_LEFT, 0.0);

    char sub[256];
    if (s->phase == UP_RUNNING) {
        snprintf(sub, sizeof sub, "%s", s->status);
    } else if (s->phase == UP_DONE) {
        if (s->skipped_count == REPO_COUNT)
            snprintf(sub, sizeof sub,
                     "Fill in the URLs at the top of jtupdate.c, then check again.");
        else
            snprintf(sub, sizeof sub,
                     "%d updated · %d current · %d skipped",
                     s->updated_count, s->current_count, s->skipped_count);
    } else if (s->phase == UP_ERROR) {
        snprintf(sub, sizeof sub, "%d updated · %d failed · %d current",
                 s->updated_count, s->error_count, s->current_count);
    } else if (s->phase == UP_CANCELLED) {
        snprintf(sub, sizeof sub, "Progress was stopped. You can start again.");
    } else {
        snprintf(sub, sizeof sub,
                 "Clones live in %s. Pulls are fast-forward only; "
                 "changed repositories are built and installed.",
                 g_repo_root_show);
    }
    text_draw_wrapped(cr, sub, 10.8, 400, C_DIM, 0.94,
                      tx, 145.0, 548.0, 0.12);

    char meta[128];
    snprintf(meta, sizeof meta, "%d REPOS   ·   %d UPDATED   ·   %d ISSUES",
             REPO_COUNT, s->updated_count, s->error_count);
    text_draw(cr, meta, 8.8, 600, C_FAINT, 0.90,
              tx, 182.0, ALIGN_LEFT, 1.15);
}

static void
paint_repo_icon(cairo_t *cr, const View *v, int i, double sx, double sy)
{
    RepoPhase ph = v->s->repos[i].phase;
    unsigned col = repo_color(ph);

    if (repo_active(ph)) {
        double a0 = fmod(v->t * 2.4, 2.0 * PI);
        rgba(cr, col, 0.16);
        cairo_set_line_width(cr, 2.0);
        cairo_arc(cr, sx, sy, 7.0, 0.0, 2.0 * PI);
        cairo_stroke(cr);
        rgba(cr, col, 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_arc(cr, sx, sy, 7.0, a0, a0 + 1.65);
        cairo_stroke(cr);
        return;
    }

    if (ph == RP_DONE || ph == RP_CURRENT) {
        rgba(cr, col, 0.16);
        cairo_arc(cr, sx, sy, 8.5, 0.0, 2.0 * PI);
        cairo_fill(cr);
        rgba(cr, col, 0.95);
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, sx - 3.6, sy);
        cairo_line_to(cr, sx - 0.9, sy + 2.7);
        cairo_line_to(cr, sx + 4.2, sy - 3.2);
        cairo_stroke(cr);
        return;
    }

    if (ph == RP_ERROR) {
        rgba(cr, col, 0.15);
        cairo_arc(cr, sx, sy, 8.5, 0.0, 2.0 * PI);
        cairo_fill(cr);
        rgba(cr, col, 0.95);
        cairo_set_line_width(cr, 1.8);
        cairo_move_to(cr, sx - 3.4, sy - 3.4);
        cairo_line_to(cr, sx + 3.4, sy + 3.4);
        cairo_move_to(cr, sx + 3.4, sy - 3.4);
        cairo_line_to(cr, sx - 3.4, sy + 3.4);
        cairo_stroke(cr);
        return;
    }

    if (ph == RP_SKIP) {
        rgba(cr, col, 0.65);
        cairo_set_line_width(cr, 1.5);
        cairo_arc(cr, sx, sy, 5.2, 0.0, 2.0 * PI);
        cairo_stroke(cr);
        return;
    }

    rgba(cr, col, 0.82);
    cairo_arc(cr, sx, sy, 4.1, 0.0, 2.0 * PI);
    cairo_fill(cr);
}

static void
paint_repo_row(cairo_t *cr, const View *v, int i)
{
    const RepoState *r = &v->s->repos[i];
    double x = MARGIN;
    double y = LIST_Y + i * (ROW_H + ROW_GAP);
    double w = WIN_W - MARGIN * 2.0;
    int active = repo_active(r->phase);
    int hover = v->hover_repo == i;

    cairo_pattern_t *fill = cairo_pattern_create_linear(x, y, x, y + ROW_H);
    cairo_pattern_add_color_stop_rgba(fill, 0.0,
        R_(C_CARD_HI), G_(C_CARD_HI), B_(C_CARD_HI), 0.94);
    cairo_pattern_add_color_stop_rgba(fill, 1.0,
        R_(0x090A0B), G_(0x090A0B), B_(0x090A0B), 0.94);
    cairo_set_source(cr, fill);
    rounded_rect(cr, x, y, w, ROW_H, 10.0);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);

    unsigned border = C_BORDER;
    double border_a = 0.42;
    if (active) {
        border = C_ACCENT;
        border_a = 0.32 + 0.11 * sin(v->t * 2.2);
    } else if (r->phase == RP_ERROR) {
        border = C_RED;
        border_a = 0.38;
    } else if (r->phase == RP_DONE || r->phase == RP_CURRENT) {
        border = C_GREEN;
        border_a = 0.20;
    } else if (hover) {
        border = C_FG;
        border_a = 0.10;
    }
    rgba(cr, border, border_a);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, ROW_H - 1.0, 9.5);
    cairo_stroke(cr);

    paint_repo_icon(cr, v, i, x + 22.0, y + ROW_H / 2.0);

    text_draw(cr, g_repos[i].name, 14.5, 600, C_FG, 0.97,
              x + 40.0, y + 12.0, ALIGN_LEFT, 0.1);

    unsigned dcol = C_MUTED;
    double da = 0.92;
    if (active) {
        dcol = C_DIM;
        da = 0.96;
    } else if (r->phase == RP_ERROR) {
        dcol = C_RED;
        da = 0.80;
    } else if (r->phase == RP_DONE || r->phase == RP_CURRENT) {
        dcol = C_DIM;
        da = 0.88;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x + 40.0, y + 31.0, w - 150.0, 19.0);
    cairo_clip(cr);
    text_draw(cr, r->detail, 9.8, 400, dcol, da,
              x + 40.0, y + 32.0, ALIGN_LEFT, 0.12);
    cairo_restore(cr);

    unsigned pcol = repo_color(r->phase);
    double pw = 78.0;
    double ph = 22.0;
    double px = x + w - 16.0 - pw;
    double py = y + (ROW_H - ph) / 2.0;
    rgba(cr, pcol, active ? 0.14 : 0.075);
    rounded_rect(cr, px, py, pw, ph, ph / 2.0);
    cairo_fill(cr);
    rgba(cr, pcol, active ? 0.44 : 0.28);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, px + 0.5, py + 0.5, pw - 1.0, ph - 1.0,
                 (ph - 1.0) / 2.0);
    cairo_stroke(cr);
    text_draw(cr, repo_pill(r->phase), 8.3, 600, pcol, 0.96,
              px + pw / 2.0, py + 6.2, ALIGN_CENTER, 0.85);

    if (active) {
        double bx = x + 40.0;
        double by = y + ROW_H - 6.0;
        double bw = w - 56.0;
        rgba(cr, C_FG, 0.045);
        rounded_rect(cr, bx, by, bw, 2.4, 1.2);
        cairo_fill(cr);

        double fw = bw * clampd(r->progress, 0.0, 1.0);
        if (fw > 0.5) {
            rgba(cr, pcol, 0.10);
            rounded_rect(cr, bx - 1.0, by - 2.5, fw + 2.0, 7.4, 3.7);
            cairo_fill(cr);
            rgba(cr, pcol, 0.95);
            rounded_rect(cr, bx, by, fw, 2.4, 1.2);
            cairo_fill(cr);
        }
    }
}

static void
paint_repo_list(cairo_t *cr, const View *v)
{
    text_draw(cr, "REPOSITORIES", 9.0, 600, C_DIM, 0.82,
              MARGIN, LIST_LABEL_Y, ALIGN_LEFT, 2.0);
    char right[64];
    snprintf(right, sizeof right, "%d REPOS", REPO_COUNT);
    text_draw(cr, right, 8.8, 600, C_FAINT, 0.78,
              WIN_W - MARGIN, LIST_LABEL_Y, ALIGN_RIGHT, 1.2);

    for (int i = 0; i < REPO_COUNT; i++)
        paint_repo_row(cr, v, i);
}

static void
paint_progress(cairo_t *cr, const View *v, int running)
{
    const UpdateSnap *s = v->s;
    double x = MARGIN;
    double y = PROGRESS_Y;
    double w = WIN_W - MARGIN * 2.0;

    cairo_pattern_t *fill = cairo_pattern_create_linear(x, y, x, y + PROGRESS_H);
    cairo_pattern_add_color_stop_rgba(fill, 0.0,
        R_(C_CARD_HI), G_(C_CARD_HI), B_(C_CARD_HI), 0.92);
    cairo_pattern_add_color_stop_rgba(fill, 1.0,
        R_(0x090A0B), G_(0x090A0B), B_(0x090A0B), 0.92);
    cairo_set_source(cr, fill);
    rounded_rect(cr, x, y, w, PROGRESS_H, 10.0);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);

    rgba(cr, running ? C_ACCENT : C_BORDER, running ? 0.28 : 0.38);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, PROGRESS_H - 1.0, 9.5);
    cairo_stroke(cr);

    unsigned col = C_ACCENT;
    if (s->phase == UP_ERROR)
        col = C_RED;
    else if (s->phase == UP_DONE)
        col = C_GREEN;

    double p = clampd(v->disp, 0.0, 1.0);
    char pct[32];
    snprintf(pct, sizeof pct, "%d%%", (int)lround(p * 100.0));

    text_draw(cr, "OVERALL PROGRESS", 8.8, 600,
              running ? C_ACCENT : C_DIM, running ? 0.90 : 0.78,
              x + 16.0, y + 11.0, ALIGN_LEFT, 1.7);
    text_draw(cr, pct, 11.0, 700, col, 0.96,
              x + w - 16.0, y + 9.0, ALIGN_RIGHT, 0.2);

    double bx = x + 16.0;
    double by = y + 32.0;
    double bw = w - 32.0;
    double bh = 8.0;
    rgba(cr, C_FG, 0.050);
    rounded_rect(cr, bx, by, bw, bh, bh / 2.0);
    cairo_fill(cr);

    if (p > 0.001) {
        double fw = bw * p;
        rgba(cr, col, running ? 0.11 : 0.07);
        rounded_rect(cr, bx - 1.0, by - 3.0, fw + 2.0, bh + 6.0,
                     (bh + 6.0) / 2.0);
        cairo_fill(cr);
        rgba(cr, col, 0.96);
        rounded_rect(cr, bx, by, fw, bh, bh / 2.0);
        cairo_fill(cr);

        if (running && p < 0.999) {
            double ex = bx + fw;
            double pulse = 0.5 + 0.5 * sin(v->t * 3.2);
            rgba(cr, col, 0.12 + 0.08 * pulse);
            cairo_arc(cr, ex, by + bh / 2.0, 7.0, 0.0, 2.0 * PI);
            cairo_fill(cr);
            rgba(cr, C_FG, 0.85);
            cairo_arc(cr, ex, by + bh / 2.0, 1.35, 0.0, 2.0 * PI);
            cairo_fill(cr);
        }
    }

    cairo_save(cr);
    cairo_rectangle(cr, x + 16.0, y + 47.0, w - 32.0, 18.0);
    cairo_clip(cr);
    text_draw(cr, s->status, 9.8, 400, C_DIM, 0.92,
              x + 16.0, y + 47.0, ALIGN_LEFT, 0.12);
    cairo_restore(cr);
}

static void
paint_button(cairo_t *cr, const View *v, int running)
{
    double bx = (WIN_W - BUTTON_W) / 2.0;
    double by = BUTTON_Y;

    for (int i = 3; i >= 1; i--) {
        rgba(cr, C_ACCENT, running ? 0.010 : 0.018);
        rounded_rect(cr, bx - i * 3.0, by - i * 3.0,
                     BUTTON_W + i * 6.0, BUTTON_H + i * 6.0,
                     BUTTON_H / 2.0 + i * 3.0);
        cairo_fill(cr);
    }

    unsigned ink;
    if (running) {
        rgba(cr, C_ACCENT, v->press_primary ? 0.16 :
                           (v->hover_primary ? 0.11 : 0.055));
        rounded_rect(cr, bx, by, BUTTON_W, BUTTON_H, BUTTON_H / 2.0);
        cairo_fill(cr);

        rgba(cr, C_ACCENT, 0.90);
        cairo_set_line_width(cr, 1.2);
        rounded_rect(cr, bx + 0.6, by + 0.6,
                     BUTTON_W - 1.2, BUTTON_H - 1.2,
                     BUTTON_H / 2.0 - 0.6);
        cairo_stroke(cr);
        ink = C_ACCENT;
    } else {
        rgba(cr, C_ACCENT, v->press_primary ? 0.86 :
                           (v->hover_primary ? 0.97 : 1.0));
        rounded_rect(cr, bx, by, BUTTON_W, BUTTON_H, BUTTON_H / 2.0);
        cairo_fill(cr);
        if (v->hover_primary) {
            rgba(cr, C_FG, 0.20);
            cairo_set_line_width(cr, 1.0);
            rounded_rect(cr, bx + 0.5, by + 0.5,
                         BUTTON_W - 1.0, BUTTON_H - 1.0,
                         BUTTON_H / 2.0 - 0.5);
            cairo_stroke(cr);
        }
        ink = C_ON_ACCENT;
    }

    const char *label = running ? "Stop update" : "Check for updates";
    if (!running && v->s->phase == UP_DONE)
        label = "Check again";
    else if (!running && v->s->phase == UP_ERROR)
        label = "Try again";

    double tw = text_width(cr, label, 13.5, 600, 0.25);
    double iconw = 12.0;
    double gap = 10.0;
    double gx = (WIN_W - (iconw + gap + tw)) / 2.0;
    double cy = by + BUTTON_H / 2.0;

    rgba(cr, ink, 0.98);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (running) {
        rounded_rect(cr, gx + 1.0, cy - 5.0, 10.0, 10.0, 2.0);
        cairo_fill(cr);
    } else {
        cairo_move_to(cr, gx + 6.0, cy - 7.0);
        cairo_line_to(cr, gx + 6.0, cy + 1.0);
        cairo_move_to(cr, gx + 1.8, cy - 3.0);
        cairo_line_to(cr, gx + 6.0, cy + 1.2);
        cairo_line_to(cr, gx + 10.2, cy - 3.0);
        cairo_move_to(cr, gx + 1.0, cy + 7.0);
        cairo_line_to(cr, gx + 11.0, cy + 7.0);
        cairo_stroke(cr);
    }

    text_draw(cr, label, 13.5, 600, ink, 1.0,
              gx + iconw + gap, cy - 9.4, ALIGN_LEFT, 0.25);
}

static void
paint_footer(cairo_t *cr, const View *v, int running)
{
    (void)running;
    char clones[PATH_MAX + 16];
    snprintf(clones, sizeof clones, "CLONES IN %s", g_repo_root_show);
    for (char *p = clones; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    text_draw(cr, clones, 8.8, 400, C_FAINT, 0.78,
              MARGIN, FOOTER_Y, ALIGN_LEFT, 0.35);
    text_draw(cr, "SPACE  CHECK     ESC  QUIT", 8.8, 600, C_FAINT, 0.62,
              WIN_W - MARGIN, FOOTER_Y, ALIGN_RIGHT, 1.0);
    (void)v;
}

static void
paint_content(cairo_t *cr, const View *v)
{
    const UpdateSnap *s = v->s;
    int running = s->phase == UP_RUNNING;

    cairo_save(cr);
    rounded_rect(cr, 0.5, 0.5, WIN_W - 1.0, WIN_H - 1.0, 12.0);
    cairo_clip(cr);

    cairo_pattern_t *bg = cairo_pattern_create_linear(0.0, 0.0, 0.0, WIN_H);
    cairo_pattern_add_color_stop_rgba(bg, 0.0,
        R_(C_BG_TOP), G_(C_BG_TOP), B_(C_BG_TOP), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 0.55,
        R_(0x050506), G_(0x050506), B_(0x050506), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 1.0,
        R_(C_BG), G_(C_BG), B_(C_BG), 1.0);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    paint_ambient(cr, v, running);
    paint_header(cr, v);
    paint_hero(cr, v, running);
    paint_repo_list(cr, v);
    paint_progress(cr, v, running);
    paint_button(cr, v, running);
    paint_footer(cr, v, running);

    cairo_restore(cr);

    rgba(cr, C_BORDER, 1.0);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, 0.5, 0.5, WIN_W - 1.0, WIN_H - 1.0, 12.0);
    cairo_stroke(cr);
}

/* ------------------------------------------------------------------ */
/*  Application actions                                                */
/* ------------------------------------------------------------------ */

static int
hit_primary(double x, double y)
{
    double bx = (WIN_W - BUTTON_W) / 2.0;
    return x >= bx && x <= bx + BUTTON_W &&
           y >= BUTTON_Y && y <= BUTTON_Y + BUTTON_H;
}

static int
hit_close(double x, double y)
{
    return x >= WIN_W - 48.0 && y <= 56.0;
}

static int
repo_at(double x, double y)
{
    double x0 = MARGIN;
    double x1 = WIN_W - MARGIN;
    if (x < x0 || x > x1)
        return -1;
    for (int i = 0; i < REPO_COUNT; i++) {
        double y0 = LIST_Y + i * (ROW_H + ROW_GAP);
        if (y >= y0 && y <= y0 + ROW_H)
            return i;
    }
    return -1;
}

static int
update_is_running(App *a)
{
    pthread_mutex_lock(&a->upd.lock);
    int running = a->upd.running;
    pthread_mutex_unlock(&a->upd.lock);
    return running;
}

static void
start_update(App *a)
{
    if (a->worker_alive) {
        if (update_is_running(a))
            return;
        pthread_join(a->worker, NULL);
        a->worker_alive = 0;
    }

    state_begin(&a->upd);
    a->disp = 0.0;
    a->done_t = -1.0;
    a->prev_phase = UP_RUNNING;

    if (pthread_create(&a->worker, NULL, update_worker, a) == 0) {
        a->worker_alive = 1;
    } else {
        pthread_mutex_lock(&a->upd.lock);
        a->upd.running = 0;
        a->upd.phase = UP_ERROR;
        snprintf(a->upd.status, sizeof a->upd.status,
                 "Could not start the update thread.");
        pthread_mutex_unlock(&a->upd.lock);
    }
}

static void
stop_update(App *a)
{
    state_cancel(&a->upd);
}

static void
toggle_update(App *a)
{
    if (update_is_running(a))
        stop_update(a);
    else
        start_update(a);
}

static void
app_reap(App *a)
{
    if (a->worker_alive && !update_is_running(a)) {
        pthread_join(a->worker, NULL);
        a->worker_alive = 0;
    }
}

static void
update_cursor(App *a)
{
    if (!a->cursor_shape_manager || !a->pointer)
        return;
    if (!a->cursor_shape_device) {
        a->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
            a->cursor_shape_manager, a->pointer);
        a->cursor_shape = 0;
    }

    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (a->hover_close || a->hover_primary) {
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    } else if (a->pointer_x >= 0.0 && a->pointer_y >= 0.0 &&
               a->pointer_y < 58.0) {
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
    }

    if (shape != a->cursor_shape) {
        wp_cursor_shape_device_v1_set_shape(a->cursor_shape_device,
                                            a->pointer_serial, shape);
        a->cursor_shape = shape;
    }
}

/* ------------------------------------------------------------------ */
/*  Wayland backend                                                    */
/* ------------------------------------------------------------------ */

#define NBUFS 4
#define MAX_OUTPUTS 8
#define BTN_LEFT 0x110

static int
create_anon_file(size_t size)
{
    int fd = memfd_create("jtupdate", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0)
            return fd;
        close(fd);
    }

    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/jtupdate-XXXXXX",
             (dir && *dir) ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void
shm_buffer_destroy(ShmBuf *b)
{
    if (b->buffer) {
        wl_buffer_destroy(b->buffer);
        b->buffer = NULL;
    }
    if (b->data) {
        munmap(b->data, b->size);
        b->data = NULL;
    }
    b->size = 0;
    b->width = b->height = 0;
    b->busy = b->retired = 0;
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ShmBuf *b = data;
    b->busy = 0;
    if (b->retired)
        shm_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int
shm_buffer_create(App *a, ShmBuf *b, int w, int h)
{
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = create_anon_file(size);
    if (fd < 0)
        return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    if (!pool) {
        munmap(data, size);
        close(fd);
        return -1;
    }

    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!b->buffer) {
        munmap(data, size);
        return -1;
    }

    b->data = data;
    b->size = size;
    b->width = w;
    b->height = h;
    b->busy = 0;
    b->retired = 0;
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    return 0;
}

static ShmBuf *
next_buffer(App *a, int w, int h)
{
    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired &&
            b->width == w && b->height == h)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer && shm_buffer_create(a, b, w, h) == 0)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired) {
            shm_buffer_destroy(b);
            if (shm_buffer_create(a, b, w, h) == 0)
                return b;
        }
    }

    return NULL;
}

static void
app_draw(App *a, const UpdateSnap *s)
{
    if (!a->configured || !a->surface || !a->shm)
        return;

    int w = (int)lround(WIN_W * a->scale);
    int h = (int)lround(WIN_H * a->scale);
    if (w < 1 || h < 1)
        return;

    ShmBuf *b = next_buffer(a, w, h);
    if (!b)
        return;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, a->scale, a->scale);

    View v;
    memset(&v, 0, sizeof v);
    v.s = s;
    v.t = mono_sec() - a->t0;
    v.disp = a->disp;
    v.hover_primary = a->hover_primary;
    v.press_primary = a->press_primary;
    v.hover_close = a->hover_close;
    v.hover_repo = a->hover_repo;
    v.done_t = a->done_t;

    paint_content(cr, &v);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_attach(a->surface, b->buffer, 0, 0);
    wl_surface_damage(a->surface, 0, 0, (int)WIN_W, (int)WIN_H);
    wl_surface_commit(a->surface);
    b->busy = 1;
}

static void
recalc_scale(App *a)
{
    if (a->fixed_scale)
        return;

    int s = 0;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].entered && a->outputs[i].scale > s)
            s = a->outputs[i].scale;
    }
    if (s < 1)
        s = 1;
    if (s > 3)
        s = 3;
    if (s == a->scale)
        return;

    a->scale = s;
    if (a->surface && wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, s);

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer)
            continue;
        if (b->busy)
            b->retired = 1;
        else
            shm_buffer_destroy(b);
    }
}

/* ---- protocol listeners ------------------------------------------ */

static void
shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    (void)shm;
    App *a = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        a->has_argb = 1;
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void
wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    App *a = data;
    xdg_surface_ack_configure(xs, serial);
    a->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *t,
                   int32_t width, int32_t height, struct wl_array *states)
{
    (void)data; (void)t; (void)width; (void)height; (void)states;
}

static void
toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    App *a = data;
    a->quit = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void
decoration_configure(void *data,
                     struct zxdg_toplevel_decoration_v1 *dec, uint32_t mode)
{
    (void)data; (void)dec; (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void
output_geometry(void *data, struct wl_output *output,
                int32_t x, int32_t y, int32_t pw, int32_t ph,
                int32_t subpixel, const char *make,
                const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void
output_mode(void *data, struct wl_output *output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags;
    (void)width; (void)height; (void)refresh;
}

static void
output_done(void *data, struct wl_output *output)
{
    (void)data; (void)output;
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    Output *o = data;
    if (factor < 1)
        factor = 1;
    o->scale = factor;
    if (o->entered)
        recalc_scale(o->app);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void
surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 1;
            break;
        }
    }
    recalc_scale(a);
}

static void
surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 0;
            break;
        }
    }
    recalc_scale(a);
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
              struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)surface;
    App *a = data;
    a->pointer_serial = serial;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    a->cursor_shape = 0;
    a->hover_primary = hit_primary(a->pointer_x, a->pointer_y);
    a->hover_close = hit_close(a->pointer_x, a->pointer_y);
    a->hover_repo = repo_at(a->pointer_x, a->pointer_y);
    update_cursor(a);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
              struct wl_surface *surface)
{
    (void)pointer; (void)serial; (void)surface;
    App *a = data;
    a->pointer_x = -1;
    a->pointer_y = -1;
    a->hover_primary = 0;
    a->hover_close = 0;
    a->hover_repo = -1;
    a->press_primary = 0;
    update_cursor(a);
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
               wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    App *a = data;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    a->hover_primary = hit_primary(a->pointer_x, a->pointer_y);
    a->hover_close = hit_close(a->pointer_x, a->pointer_y);
    a->hover_repo = repo_at(a->pointer_x, a->pointer_y);
    update_cursor(a);
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
               uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer; (void)time;
    App *a = data;

    if (button != BTN_LEFT)
        return;

    double x = a->pointer_x;
    double y = a->pointer_y;
    a->hover_primary = hit_primary(x, y);
    a->hover_close = hit_close(x, y);
    a->hover_repo = repo_at(x, y);

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (hit_close(x, y)) {
            a->quit = 1;
        } else if (hit_primary(x, y)) {
            a->press_primary = 1;
        } else if (y < 58.0) {
            if (a->toplevel && a->seat)
                xdg_toplevel_move(a->toplevel, a->seat, serial);
        }
    } else if (a->press_primary) {
        a->press_primary = 0;
        if (hit_primary(x, y))
            toggle_update(a);
    }
    update_cursor(a);
}

static void
pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
             uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                  uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                      int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void
keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    App *a = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km)
        return;

    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }

    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->xkb_keymap);
    a->xkb_keymap = km;
    a->xkb_state = st;
}

static void
keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys)
{
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void
keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)serial; (void)surface;
}

static void
keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard; (void)serial; (void)time;
    App *a = data;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !a->xkb_state)
        return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);
    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q) {
        a->quit = 1;
    } else if (sym == XKB_KEY_space || sym == XKB_KEY_Return ||
               sym == XKB_KEY_KP_Enter) {
        toggle_update(a);
        update_cursor(a);
    }
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                   uint32_t dep, uint32_t latched, uint32_t locked,
                   uint32_t group)
{
    (void)keyboard; (void)serial;
    App *a = data;
    if (a->xkb_state)
        xkb_state_update_mask(a->xkb_state, dep, latched, locked, 0, 0, group);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                     int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    App *a = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        if (a->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
            a->cursor_shape_device = NULL;
            a->cursor_shape = 0;
        }
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
    App *a = data;

    if (!strcmp(interface, wl_compositor_interface.name)) {
        a->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface,
                                         version < 4 ? version : 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(a->shm, &shm_listener, a);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        a->wm_base = wl_registry_bind(registry, name,
                                      &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(a->wm_base, &wm_base_listener, a);
    } else if (!strcmp(interface,
                       zxdg_decoration_manager_v1_interface.name)) {
        a->deco_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       xdg_toplevel_icon_manager_v1_interface.name)) {
        a->icon_manager = wl_registry_bind(
            registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       wp_cursor_shape_manager_v1_interface.name)) {
        a->cursor_shape_manager = wl_registry_bind(
            registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        a->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        if (a->output_count < MAX_OUTPUTS) {
            Output *o = &a->outputs[a->output_count];
            o->output = wl_registry_bind(registry, name,
                                         &wl_output_interface,
                                         version < 2 ? version : 2);
            o->scale = 1;
            o->entered = 0;
            o->app = a;
            wl_output_add_listener(o->output, &output_listener, o);
            a->output_count++;
        }
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int
wayland_start(App *a)
{
    a->display = wl_display_connect(NULL);
    if (!a->display) {
        fprintf(stderr, "jtupdate: cannot connect to a Wayland display\n");
        return 0;
    }

    a->registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(a->registry, &registry_listener, a);
    if (wl_display_roundtrip(a->display) < 0 ||
        wl_display_roundtrip(a->display) < 0) {
        fprintf(stderr, "jtupdate: Wayland roundtrip failed\n");
        return 0;
    }

    if (!a->compositor || !a->shm || !a->wm_base) {
        fprintf(stderr, "jtupdate: missing wl_compositor, wl_shm "
                        "or xdg_wm_base\n");
        return 0;
    }
    if (!a->has_argb) {
        fprintf(stderr, "jtupdate: compositor has no ARGB8888 shm format\n");
        return 0;
    }

    a->surface = wl_compositor_create_surface(a->compositor);
    if (!a->surface) {
        fprintf(stderr, "jtupdate: cannot create wl_surface\n");
        return 0;
    }
    wl_surface_add_listener(a->surface, &surface_listener, a);

    if (wl_surface_get_version(a->surface) < 3) {
        a->scale = 1;
        a->fixed_scale = 1;
    }

    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, a->surface);
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);
    a->toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    xdg_toplevel_add_listener(a->toplevel, &toplevel_listener, a);
    xdg_toplevel_set_title(a->toplevel, "JTLab Updater");
    xdg_toplevel_set_app_id(a->toplevel, "jtupdate");
    xdg_toplevel_set_min_size(a->toplevel, (int)WIN_W, (int)WIN_H);
    xdg_toplevel_set_max_size(a->toplevel, (int)WIN_W, (int)WIN_H);

    if (a->deco_manager) {
        a->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, a->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(
            a->decoration, &decoration_listener, a);
        zxdg_toplevel_decoration_v1_set_mode(
            a->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }

    if (a->icon_manager) {
        struct xdg_toplevel_icon_v1 *icon =
            xdg_toplevel_icon_manager_v1_create_icon(a->icon_manager);
        if (icon) {
            xdg_toplevel_icon_v1_set_name(icon, "jtupdate");
            xdg_toplevel_icon_manager_v1_set_icon(a->icon_manager,
                                                  a->toplevel, icon);
            xdg_toplevel_icon_v1_destroy(icon);
        }
    }

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_commit(a->surface);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Live control socket                                                */
/* ------------------------------------------------------------------ */

static void
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0])
        rt = "/tmp";
    snprintf(buf, n, "%s/jtupdate-ctl.sock", rt);
}

static void
ctl_handle_line(App *a, char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';

    if (!strcmp(line, "quit")) {
        a->quit = 1;
        return;
    }

    if (!strcmp(line, "update")) {
        if (!update_is_running(a))
            start_update(a);
        a->last_draw = -1.0;
        return;
    }

    if (!strcmp(line, "stop")) {
        stop_update(a);
        a->last_draw = -1.0;
        return;
    }

    if (strncmp(line, "accent", 6) == 0 &&
        (line[6] == ' ' || line[6] == '\t')) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        unsigned rgb;
        if (accent_parse(arg, &rgb)) {
            accent_set(rgb);
            a->last_draw = -1.0;
        }
    }
}

static void
ctl_close_client(App *a)
{
    if (a->ctl_client_fd >= 0) {
        close(a->ctl_client_fd);
        a->ctl_client_fd = -1;
    }
    a->ctl_len = 0;
}

static void
ctl_process(App *a, int flush)
{
    size_t start = 0;
    for (size_t i = 0; i < a->ctl_len; i++) {
        if (a->ctl_buf[i] == '\n') {
            a->ctl_buf[i] = '\0';
            ctl_handle_line(a, a->ctl_buf + start);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(a->ctl_buf, a->ctl_buf + start, a->ctl_len - start);
        a->ctl_len -= start;
    }
    if (flush && a->ctl_len > 0) {
        a->ctl_buf[a->ctl_len] = '\0';
        ctl_handle_line(a, a->ctl_buf);
        a->ctl_len = 0;
    }
}

static void
ctl_read_client(App *a)
{
    char buf[128];
    for (;;) {
        ssize_t n = read(a->ctl_client_fd, buf, sizeof(buf));
        if (n > 0) {
            size_t room = sizeof(a->ctl_buf) - 1 - a->ctl_len;
            if ((size_t)n > room) {
                ctl_close_client(a);
                return;
            }
            memcpy(a->ctl_buf + a->ctl_len, buf, (size_t)n);
            a->ctl_len += (size_t)n;
            ctl_process(a, 0);
            continue;
        }
        if (n == 0) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        return;
    }
}

static void
ctl_accept(App *a)
{
    for (;;) {
        int fd = accept4(a->ctl_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (a->ctl_client_fd >= 0) {
            close(fd);
            continue;
        }
        a->ctl_client_fd = fd;
        a->ctl_len = 0;
    }
}

static int
ctl_start(App *a)
{
    char path[256];
    ctl_path(path, sizeof(path));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "jtupdate: control socket path too long: %s\n", path);
        return 0;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("jtupdate: control socket");
        return 0;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "jtupdate: cannot bind %s: %s\n",
                path, strerror(errno));
        close(fd);
        return 0;
    }
    if (listen(fd, 4) < 0) {
        fprintf(stderr, "jtupdate: cannot listen on %s: %s\n",
                path, strerror(errno));
        close(fd);
        unlink(path);
        return 0;
    }

    a->ctl_fd = fd;
    return 1;
}

static void
ctl_cleanup(App *a)
{
    ctl_close_client(a);
    if (a->ctl_fd >= 0) {
        close(a->ctl_fd);
        a->ctl_fd = -1;
        char path[256];
        ctl_path(path, sizeof(path));
        unlink(path);
    }
}

/* Returns < 0 on a fatal display error, 0 otherwise. */
static int
pump_events(App *a, int timeout_ms)
{
    if (wl_display_dispatch_pending(a->display) < 0)
        return -1;

    int want_write = 0;
    if (wl_display_flush(a->display) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        want_write = 1;
    }

    struct pollfd pfd[3];
    pfd[0].fd = wl_display_get_fd(a->display);
    pfd[0].events = POLLIN | (want_write ? POLLOUT : 0);
    pfd[0].revents = 0;

    int nfds = 1;
    int ctl_idx = -1;
    int client_idx = -1;
    if (a->ctl_fd >= 0) {
        ctl_idx = nfds;
        pfd[nfds].fd = a->ctl_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if (a->ctl_client_fd >= 0) {
        client_idx = nfds;
        pfd[nfds].fd = a->ctl_client_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }

    int pr = poll(pfd, (nfds_t)nfds, timeout_ms);
    if (pr < 0)
        return (errno == EINTR) ? 0 : -1;
    if (pr == 0)
        return 0;

    if (pfd[0].revents & POLLOUT) {
        if (wl_display_flush(a->display) < 0 && errno != EAGAIN)
            return -1;
    }
    if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    if (pfd[0].revents & POLLIN) {
        if (wl_display_dispatch(a->display) < 0)
            return -1;
    }
    if (ctl_idx >= 0 && (pfd[ctl_idx].revents & POLLIN))
        ctl_accept(a);
    if (client_idx >= 0 && pfd[client_idx].revents)
        ctl_read_client(a);
    return 0;
}

static volatile sig_atomic_t g_signal_quit;

static void
on_signal(int sig)
{
    (void)sig;
    g_signal_quit = 1;
}

static void
app_cleanup(App *a)
{
    ctl_cleanup(a);

    if (a->worker_alive) {
        pthread_mutex_lock(&a->upd.lock);
        a->upd.cancel = 1;
        pthread_mutex_unlock(&a->upd.lock);
        pthread_join(a->worker, NULL);
        a->worker_alive = 0;
    }

    if (a->keyboard)
        wl_keyboard_destroy(a->keyboard);
    if (a->cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
        a->cursor_shape_device = NULL;
    }
    if (a->pointer)
        wl_pointer_destroy(a->pointer);
    if (a->cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(a->cursor_shape_manager);
    if (a->decoration)
        zxdg_toplevel_decoration_v1_destroy(a->decoration);
    if (a->toplevel)
        xdg_toplevel_destroy(a->toplevel);
    if (a->xdg_surface)
        xdg_surface_destroy(a->xdg_surface);
    if (a->surface)
        wl_surface_destroy(a->surface);
    if (a->seat)
        wl_seat_destroy(a->seat);
    if (a->deco_manager)
        zxdg_decoration_manager_v1_destroy(a->deco_manager);
    if (a->icon_manager)
        xdg_toplevel_icon_manager_v1_destroy(a->icon_manager);
    if (a->wm_base)
        xdg_wm_base_destroy(a->wm_base);
    if (a->shm)
        wl_shm_destroy(a->shm);
    if (a->compositor)
        wl_compositor_destroy(a->compositor);

    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output)
            wl_output_destroy(a->outputs[i].output);
    }
    if (a->registry)
        wl_registry_destroy(a->registry);

    if (a->xkb_state)
        xkb_state_unref(a->xkb_state);
    if (a->xkb_keymap)
        xkb_keymap_unref(a->xkb_keymap);
    if (a->xkb_ctx)
        xkb_context_unref(a->xkb_ctx);

    if (a->display) {
        for (int i = 0; i < NBUFS; i++) {
            ShmBuf *b = &a->bufs[i];
            if (b->buffer && !b->busy) {
                wl_buffer_destroy(b->buffer);
                b->buffer = NULL;
            }
        }
        wl_display_flush(a->display);
        wl_display_disconnect(a->display);
        a->display = NULL;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->data) {
            munmap(b->data, b->size);
            b->data = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Screenshot mode (also a useful preview)                            */
/* ------------------------------------------------------------------ */

static int
write_screenshot(const char *path)
{
    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, (int)WIN_W, (int)WIN_H);
    cairo_t *cr = cairo_create(surf);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    rgba(cr, C_BG, 1.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    UpdateSnap s;
    memset(&s, 0, sizeof s);
    s.phase = UP_RUNNING;
    s.running = 1;
    s.current = 1;
    s.progress = 0.61;
    s.updated_count = 1;
    snprintf(s.status, sizeof s.status,
             "Building jtspeedtest — make -j2");

    s.repos[0].phase = RP_DONE;
    s.repos[0].progress = 1.0;
    s.repos[0].updated = 1;
    snprintf(s.repos[0].detail, sizeof s.repos[0].detail,
             "Pulled and built · 3D6A1C2");

    s.repos[1].phase = RP_BUILDING;
    s.repos[1].progress = 0.64;
    snprintf(s.repos[1].detail, sizeof s.repos[1].detail,
             "Compiling source files…");

    s.repos[2].phase = RP_QUEUED;
    snprintf(s.repos[2].detail, sizeof s.repos[2].detail,
             "Waiting to check");

    s.repos[3].phase = RP_QUEUED;
    snprintf(s.repos[3].detail, sizeof s.repos[3].detail,
             "Waiting to check");

    View v;
    memset(&v, 0, sizeof v);
    v.s = &s;
    v.t = 2.4;
    v.disp = 0.61;
    v.hover_primary = 0;
    v.press_primary = 0;
    v.hover_close = 0;
    v.hover_repo = -1;
    v.done_t = -1.0;

    paint_content(cr, &v);

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);

    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "jtupdate: could not write %s: %s\n",
                path, cairo_status_to_string(st));
        return 1;
    }
    printf("wrote %s\n", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    const char *base_dir = NULL;
    if (!base_dir_cli(&argc, argv, &base_dir))
        return 2;
    if (!accent_cli(&argc, argv))
        return 2;

    repo_paths_init(base_dir);

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: %s [ACCENT] [--base-dir DIR] [--screenshot PATH]\n\n"
               "  ACCENT              #RRGGBB or RRGGBB, for example '#378484'\n"
               "  --accent COLOR      explicit form of ACCENT\n"
               "  --base-dir DIR      directory that holds the checkouts\n"
               "                      (default: ~/jtupdate/repos)\n"
               "  --screenshot PATH   render a demo frame to PATH and exit\n\n"
               "Repositories are cloned into %s/<slug>.\n"
               "Add their URLs to the g_repos table at the top of jtupdate.c.\n\n"
               "controls: Space/Enter check or stop, Esc/q quit\n"
               "live accent: jtupdatectl accent '#RRGGBB'\n"
               "requires a Wayland compositor supporting xdg-shell\n",
               argv[0], g_repo_root_show);
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "--screenshot")) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s --screenshot PATH\n", argv[0]);
            return 2;
        }
        return write_screenshot(argv[2]);
    }

    App a;
    memset(&a, 0, sizeof a);
    a.done_t = -1.0;
    a.prev_phase = UP_IDLE;
    a.scale = 1;
    a.ctl_fd = -1;
    a.ctl_client_fd = -1;
    a.hover_repo = -1;
    a.pointer_x = -1.0;
    a.pointer_y = -1.0;

    repo_paths_init(base_dir);
    state_init(&a.upd);

    const char *scale_env = getenv("JTUPDATE_SCALE");
    if (scale_env && *scale_env) {
        int s = (int)lround(atof(scale_env));
        if (s < 1) s = 1;
        if (s > 3) s = 3;
        a.scale = s;
        a.fixed_scale = 1;
    }

    a.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!a.xkb_ctx) {
        fprintf(stderr, "jtupdate: cannot create xkb context\n");
        pthread_mutex_destroy(&a.upd.lock);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!wayland_start(&a)) {
        app_cleanup(&a);
        pthread_mutex_destroy(&a.upd.lock);
        return 1;
    }

    ctl_start(&a);

    a.t0 = mono_sec();
    double last = mono_sec();

    while (!a.quit && !g_signal_quit) {
        app_reap(&a);

        UpdateSnap s;
        state_snapshot(&a.upd, &s);

        double now = mono_sec();
        double dt = now - last;
        if (dt > 0.12) dt = 0.12;
        if (dt < 0.0) dt = 0.0;
        last = now;

        if (s.phase != a.prev_phase) {
            if (s.phase == UP_DONE || s.phase == UP_ERROR ||
                s.phase == UP_CANCELLED)
                a.done_t = now - a.t0;
            a.prev_phase = s.phase;
        }

        double target = s.progress;
        double k = 1.0 - exp(-dt * 5.0);
        a.disp += (target - a.disp) * k;
        if (fabs(target - a.disp) < 0.001)
            a.disp = target;

        double interval = s.running ? 1.0 / 60.0 : 1.0 / 30.0;
        if (a.configured && now - a.last_draw >= interval) {
            app_draw(&a, &s);
            a.last_draw = now;
        }

        int timeout;
        if (!a.configured) {
            timeout = 100;
        } else {
            double wait = interval - (mono_sec() - a.last_draw);
            if (wait < 0.0)
                wait = 0.0;
            timeout = (int)(wait * 1000.0) + 1;
        }

        if (pump_events(&a, timeout) < 0)
            break;
    }

    app_cleanup(&a);
    pthread_mutex_destroy(&a.upd.lock);
    return 0;
}

