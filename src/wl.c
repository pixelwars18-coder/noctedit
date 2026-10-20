/* Native Wayland platform layer: wl_shm software buffers (no GPU stack),
 * xkbcommon keyboard + compose, clipboard / primary selection / file drops,
 * fractional scaling and cursor-shape. Everything runs on one poll() loop. */
#define _GNU_SOURCE
#include "noctedit.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

typedef struct {
    struct wl_buffer *wb;
    uint32_t *data;
    int w, h;
    size_t size;
    bool busy;
    Rect stale; /* region not yet copied from the front buffer */
} Buf;

typedef struct { bool utf8, text, uris; } OfferInfo;

typedef struct { struct wl_output *o; int scale; uint32_t name; } Output;

static struct {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    uint32_t comp_ver;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wl_seat *seat;
    struct wl_data_device_manager *ddm;
    struct wl_data_device *dd;
    struct zwp_primary_selection_device_manager_v1 *psm;
    struct zwp_primary_selection_device_v1 *psd;
    struct zxdg_decoration_manager_v1 *decoman;
    struct zxdg_toplevel_decoration_v1 *deco;
    struct wp_cursor_shape_manager_v1 *csm;
    struct wp_cursor_shape_device_v1 *csd;
    struct wp_fractional_scale_manager_v1 *fsm;
    struct wp_fractional_scale_v1 *fs;
    struct wp_viewporter *vp;
    struct wp_viewport *viewport;
    Output outputs[8];
    int noutputs;

    struct wl_surface *surf;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *top;
    struct wl_pointer *ptr;
    struct wl_keyboard *kbd;

    int w, h, pend_w, pend_h;
    int pw, ph;
    double frac; /* fractional scale (0 = unsupported) */
    int int_scale, out_scale;
    bool configured, ssd, maximized, running;

    Buf bufs[2];
    Buf *front;
    Rect damage;
    bool frame_pending, need_draw;

    struct xkb_context *xkb;
    struct xkb_keymap *km;
    struct xkb_state *ks;
    struct xkb_compose_table *ctab;
    struct xkb_compose_state *cst;
    uint32_t mods;
    int rep_rate, rep_delay, rep_fd;
    uint32_t rep_code, rep_sym;
    char rep_utf8[16];

    uint32_t serial, ptr_serial;
    double mx, my;
    int cursor;
    uint64_t last_click;
    double lcx, lcy;
    int clicks;

    double ax, ay;
    int sx, sy;
    bool discrete;

    struct wl_data_offer *sel_offer, *dnd_offer;
    struct zwp_primary_selection_offer_v1 *psel_offer;
    struct wl_data_source *src;
    struct zwp_primary_selection_source_v1 *psrc;
    char *clip[2];
    int clip_len[2];
    bool own[2];

    int timer_fd[TIMER_COUNT];
    struct { int fd; void (*cb)(int, void *); void *ud; } watch[4];
    int nwatch;

    struct wl_cursor_theme *ctheme;
    struct wl_surface *csurf;
} W;

uint64_t plat_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ sizing */
static double eff_scale(void) { return W.frac > 0 ? W.frac : (W.int_scale ? W.int_scale : 1); }

static void recompute_size(void) {
    int pw, ph;
    if (W.frac > 0) {
        pw = (int)lround(W.w * W.frac);
        ph = (int)lround(W.h * W.frac);
    } else {
        int s = W.int_scale ? W.int_scale : 1;
        pw = W.w * s;
        ph = W.h * s;
    }
    if (pw == W.pw && ph == W.ph) return;
    W.pw = pw;
    W.ph = ph;
    if (W.viewport) wp_viewport_set_destination(W.viewport, W.w, W.h);
    else wl_surface_set_buffer_scale(W.surf, W.int_scale ? W.int_scale : 1);
    if (getenv("NOCTEDIT_DEBUG")) fprintf(stderr, "size %dx%d logical, %dx%d px, scale %.3f (frac %.3f int %d)\n", W.w, W.h, pw, ph, eff_scale(), W.frac, W.int_scale);
    app_resize(pw, ph, eff_scale());
    plat_redraw();
}

/* ------------------------------------------------------------------ buffers */
static void buf_release(void *data, struct wl_buffer *wb) { (void)wb; ((Buf *)data)->busy = false; }
static const struct wl_buffer_listener buf_listener = {.release = buf_release};

static void buf_destroy(Buf *b) {
    if (b->wb) wl_buffer_destroy(b->wb);
    if (b->data) munmap(b->data, b->size);
    if (W.front == b) W.front = NULL;
    memset(b, 0, sizeof *b);
}

static bool buf_create(Buf *b, int w, int h) {
    buf_destroy(b);
    size_t size = (size_t)w * h * 4;
    int fd = memfd_create("noctedit", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); return false; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return false; }
    struct wl_shm_pool *pool = wl_shm_create_pool(W.shm, fd, size);
    b->wb = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    b->data = data;
    b->size = size;
    b->w = w;
    b->h = h;
    b->stale = (Rect){0, 0, w, h};
    wl_buffer_add_listener(b->wb, &buf_listener, b);
    return true;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t) {
    (void)data; (void)t;
    wl_callback_destroy(cb);
    W.frame_pending = false;
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void draw_frame(void) {
    if (!W.configured || W.frame_pending || !W.need_draw || W.pw <= 0) return;
    Buf *b = NULL;
    for (int i = 0; i < 2; i++)
        if (W.bufs[i].wb && !W.bufs[i].busy && W.bufs[i].w == W.pw && W.bufs[i].h == W.ph) { b = &W.bufs[i]; break; }
    if (!b) {
        for (int i = 0; i < 2 && !b; i++)
            if (!W.bufs[i].wb || (!W.bufs[i].busy && (W.bufs[i].w != W.pw || W.bufs[i].h != W.ph))) b = &W.bufs[i];
        if (!b) return; /* both busy: wait for a release */
        if (!buf_create(b, W.pw, W.ph)) return;
    }
    Rect full = {0, 0, W.pw, W.ph};
    Rect dmg = rect_intersect(W.damage, full);
    if (b->stale.w > 0) {
        if (W.front && W.front != b && W.front->w == b->w && W.front->h == b->h) {
            Rect s = b->stale;
            for (int y = s.y; y < s.y + s.h; y++)
                memcpy(b->data + (size_t)y * b->w + s.x, W.front->data + (size_t)y * b->w + s.x, s.w * 4);
        } else {
            dmg = full;
        }
    }
    if (dmg.w <= 0) { W.need_draw = false; return; }
    Canvas cv = {.px = b->data, .w = b->w, .h = b->h, .stride = b->w};
    cv_clip(&cv, dmg);
    W.need_draw = false;
    W.damage = (Rect){0};
    app_render(&cv); /* may call plat_redraw() again to keep an animation going */
    for (int i = 0; i < 2; i++) if (&W.bufs[i] != b && W.bufs[i].wb) W.bufs[i].stale = rect_union(W.bufs[i].stale, dmg);
    b->stale = (Rect){0};
    wl_surface_attach(W.surf, b->wb, 0, 0);
    wl_surface_damage_buffer(W.surf, dmg.x, dmg.y, dmg.w, dmg.h);
    struct wl_callback *cb = wl_surface_frame(W.surf);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    wl_surface_commit(W.surf);
    b->busy = true;
    W.front = b;
    W.frame_pending = true;
}

void plat_trim(void) {
    for (int i = 0; i < 2; i++)
        if (W.bufs[i].wb && &W.bufs[i] != W.front && !W.bufs[i].busy) buf_destroy(&W.bufs[i]);
}

void plat_damage(Rect r) {
    W.damage = rect_union(W.damage, r);
    W.need_draw = true;
}
void plat_redraw(void) { plat_damage((Rect){0, 0, MAX(W.pw, 1), MAX(W.ph, 1)}); }

/* ------------------------------------------------------------------ xdg */
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void xsurf_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    if (W.pend_w > 0) W.w = W.pend_w;
    if (W.pend_h > 0) W.h = W.pend_h;
    W.configured = true;
    recompute_size();
    plat_redraw();
}
static const struct xdg_surface_listener xsurf_listener = {.configure = xsurf_configure};

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void)d; (void)t;
    W.pend_w = w;
    W.pend_h = h;
    bool max = false;
    uint32_t *s;
    wl_array_for_each(s, states) if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED) max = true;
    W.maximized = max;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; app_close_request(); }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = {
    .configure = top_configure, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps};

static void deco_configure(void *d, struct zxdg_toplevel_decoration_v1 *x, uint32_t mode) {
    (void)d; (void)x;
    bool ssd = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    if (ssd != W.ssd) { W.ssd = ssd; plat_redraw(); }
}
static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {.configure = deco_configure};

static void fs_preferred(void *d, struct wp_fractional_scale_v1 *f, uint32_t scale) {
    (void)d; (void)f;
    double s = scale / 120.0;
    if (fabs(s - W.frac) > 1e-6) { W.frac = s; if (W.configured) recompute_size(); }
}
static const struct wp_fractional_scale_v1_listener fs_listener = {.preferred_scale = fs_preferred};

static void apply_int_scale(int s) {
    if (s < 1) s = 1;
    if (s != W.int_scale) { W.int_scale = s; if (W.configured && !W.fs) recompute_size(); }
}
static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) {
    (void)d; (void)s;
    if (W.comp_ver >= 6) return;
    for (int i = 0; i < W.noutputs; i++) if (W.outputs[i].o == o) apply_int_scale(W.outputs[i].scale);
}
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_pref_scale(void *d, struct wl_surface *s, int32_t f) { (void)d; (void)s; apply_int_scale(f); }
static void surf_pref_transform(void *d, struct wl_surface *s, uint32_t t) { (void)d; (void)s; (void)t; }
static const struct wl_surface_listener surf_listener = {
    .enter = surf_enter, .leave = surf_leave,
    .preferred_buffer_scale = surf_pref_scale, .preferred_buffer_transform = surf_pref_transform};

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
                         const char *mk, const char *md, int32_t tr) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)o; ((Output *)d)->scale = f; }
static void out_name(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static void out_desc(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static const struct wl_output_listener out_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale, .name = out_name, .description = out_desc};

/* ------------------------------------------------------------------ cursor */
static void apply_cursor(void) {
    if (!W.ptr) return;
    if (W.csd) {
        uint32_t shape = W.cursor == CUR_TEXT ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT
                       : W.cursor == CUR_POINTER ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                                                 : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
        wp_cursor_shape_device_v1_set_shape(W.csd, W.ptr_serial, shape);
        return;
    }
    int s = W.int_scale ? W.int_scale : (int)ceil(eff_scale());
    if (!W.ctheme) {
        const char *sz = getenv("XCURSOR_SIZE");
        int size = sz ? atoi(sz) : 24;
        if (size <= 0) size = 24;
        W.ctheme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size * s, W.shm);
        if (!W.ctheme) return;
        W.csurf = wl_compositor_create_surface(W.comp);
    }
    const char *names[3][2] = {{"default", "left_ptr"}, {"text", "xterm"}, {"pointer", "hand2"}};
    struct wl_cursor *c = NULL;
    for (int i = 0; i < 2 && !c; i++) c = wl_cursor_theme_get_cursor(W.ctheme, names[W.cursor][i]);
    if (!c || !c->image_count) return;
    struct wl_cursor_image *img = c->images[0];
    wl_surface_set_buffer_scale(W.csurf, s);
    wl_surface_attach(W.csurf, wl_cursor_image_get_buffer(img), 0, 0);
    wl_surface_damage_buffer(W.csurf, 0, 0, img->width, img->height);
    wl_surface_commit(W.csurf);
    wl_pointer_set_cursor(W.ptr, W.ptr_serial, W.csurf, img->hotspot_x / s, img->hotspot_y / s);
}

void plat_set_cursor(int shape) {
    if (shape == W.cursor) return;
    W.cursor = shape;
    apply_cursor();
}

/* ------------------------------------------------------------------ pointer */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)s;
    W.ptr_serial = serial;
    apply_cursor();
    double k = eff_scale();
    W.mx = wl_fixed_to_double(x) * k;
    W.my = wl_fixed_to_double(y) * k;
    app_motion(W.mx, W.my);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void)d; (void)p; (void)serial; (void)s;
    app_pointer_leave();
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t;
    double k = eff_scale();
    W.mx = wl_fixed_to_double(x) * k;
    W.my = wl_fixed_to_double(y) * k;
    app_motion(W.mx, W.my);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)t;
    W.serial = serial;
    int b = button == BTN_LEFT ? 1 : button == BTN_MIDDLE ? 2 : button == BTN_RIGHT ? 3 : 0;
    if (!b) return;
    bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (down && b == 1) {
        uint64_t now = plat_now_ms();
        double tol = 5 * eff_scale();
        if (now - W.last_click < 400 && fabs(W.mx - W.lcx) < tol && fabs(W.my - W.lcy) < tol) W.clicks = W.clicks % 3 + 1;
        else W.clicks = 1;
        W.last_click = now;
        W.lcx = W.mx;
        W.lcy = W.my;
    }
    app_button(b, down, W.mx, W.my, W.mods, b == 1 ? W.clicks : 1);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
    (void)d; (void)p; (void)t;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) W.ay += wl_fixed_to_double(v);
    else W.ax += wl_fixed_to_double(v);
}
static void p_frame(void *d, struct wl_pointer *p) {
    (void)d; (void)p;
    if (W.discrete && (W.sx || W.sy)) app_scroll(W.sx, W.sy, true);
    else if (W.ax != 0 || W.ay != 0) app_scroll(W.ax * eff_scale(), W.ay * eff_scale(), false);
    W.ax = W.ay = 0;
    W.sx = W.sy = 0;
    W.discrete = false;
}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t v) {
    (void)d; (void)p;
    W.discrete = true;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) W.sy += v;
    else W.sx += v;
}
static void p_axis_120(void *d, struct wl_pointer *p, uint32_t axis, int32_t v) {
    (void)d; (void)p; (void)axis; (void)v;
}
static void p_axis_dir(void *d, struct wl_pointer *p, uint32_t axis, uint32_t dir) { (void)d; (void)p; (void)axis; (void)dir; }
static const struct wl_pointer_listener ptr_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
    .frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
    .axis_value120 = p_axis_120, .axis_relative_direction = p_axis_dir};

/* ------------------------------------------------------------------ keyboard */
static void k_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)d; (void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    struct xkb_keymap *km = xkb_keymap_new_from_string(W.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    if (W.ks) xkb_state_unref(W.ks);
    if (W.km) xkb_keymap_unref(W.km);
    W.km = km;
    W.ks = xkb_state_new(km);
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)d; (void)k; (void)s; (void)keys;
    W.serial = serial;
    app_focus(true);
}
static void stop_repeat(void) {
    W.rep_code = 0;
    struct itimerspec its = {0};
    timerfd_settime(W.rep_fd, 0, &its, NULL);
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    (void)d; (void)k; (void)serial; (void)s;
    stop_repeat();
    app_focus(false);
}

static void key_press(uint32_t code) {
    if (!W.ks) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(W.ks, code);
    char utf8[16] = {0};
    bool swallow = false;
    if (W.cst && !(W.mods & (MOD_CTRL | MOD_ALT)) && xkb_compose_state_feed(W.cst, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(W.cst)) {
        case XKB_COMPOSE_COMPOSING: swallow = true; break;
        case XKB_COMPOSE_COMPOSED:
            xkb_compose_state_get_utf8(W.cst, utf8, sizeof utf8);
            sym = xkb_compose_state_get_one_sym(W.cst);
            xkb_compose_state_reset(W.cst);
            break;
        case XKB_COMPOSE_CANCELLED: xkb_compose_state_reset(W.cst); swallow = true; break;
        default: xkb_state_key_get_utf8(W.ks, code, utf8, sizeof utf8); break;
        }
    } else {
        xkb_state_key_get_utf8(W.ks, code, utf8, sizeof utf8);
    }
    if (swallow) return;
    W.rep_sym = sym;
    memcpy(W.rep_utf8, utf8, sizeof utf8);
    app_key(sym, W.mods, utf8);
    if (W.rep_rate > 0 && xkb_keymap_key_repeats(W.km, code)) {
        W.rep_code = code;
        struct itimerspec its = {
            .it_value = {W.rep_delay / 1000, (W.rep_delay % 1000) * 1000000L},
            .it_interval = {0, 1000000000L / W.rep_rate}};
        timerfd_settime(W.rep_fd, 0, &its, NULL);
    } else {
        stop_repeat();
    }
}

static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)t;
    W.serial = serial;
    uint32_t code = key + 8;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) key_press(code);
    else if (code == W.rep_code) stop_repeat();
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    (void)d; (void)k; (void)serial;
    if (!W.ks) return;
    xkb_state_update_mask(W.ks, dep, lat, lock, 0, 0, group);
    W.mods = 0;
    if (xkb_state_mod_name_is_active(W.ks, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) W.mods |= MOD_CTRL;
    if (xkb_state_mod_name_is_active(W.ks, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) W.mods |= MOD_SHIFT;
    if (xkb_state_mod_name_is_active(W.ks, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) W.mods |= MOD_ALT;
    if (xkb_state_mod_name_is_active(W.ks, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) W.mods |= MOD_SUPER;
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d; (void)k;
    W.rep_rate = rate;
    W.rep_delay = delay;
}
static const struct wl_keyboard_listener kbd_listener = {
    .keymap = k_keymap, .enter = k_enter, .leave = k_leave, .key = k_key, .modifiers = k_mods, .repeat_info = k_repeat};

/* ------------------------------------------------------------------ clipboard */
static void offer_mime(void *d, struct wl_data_offer *o, const char *mime) {
    (void)o;
    OfferInfo *i = d;
    if (!strcmp(mime, "text/plain;charset=utf-8")) i->utf8 = true;
    else if (!strcmp(mime, "text/plain") || !strcmp(mime, "UTF8_STRING") || !strcmp(mime, "STRING")) i->text = true;
    else if (!strcmp(mime, "text/uri-list")) i->uris = true;
}
static void offer_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime, .source_actions = offer_actions, .action = offer_action};

static void offer_free(struct wl_data_offer *o) {
    if (!o) return;
    free(wl_data_offer_get_user_data(o));
    wl_data_offer_destroy(o);
}

static void dd_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {
    (void)d; (void)dd;
    wl_data_offer_add_listener(o, &offer_listener, calloc(1, sizeof(OfferInfo)));
}
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *o) {
    (void)d; (void)dd; (void)s; (void)x; (void)y;
    W.dnd_offer = o;
    if (!o) return;
    OfferInfo *i = wl_data_offer_get_user_data(o);
    wl_data_offer_accept(o, serial, i && i->uris ? "text/uri-list" : NULL);
    if (i && i->uris)
        wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
}
static void dd_leave(void *d, struct wl_data_device *dd) {
    (void)d; (void)dd;
    if (W.dnd_offer && W.dnd_offer != W.sel_offer) offer_free(W.dnd_offer);
    W.dnd_offer = NULL;
}
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)dd; (void)t; (void)x; (void)y;
}

static char *read_fd_all(int fd, int *len) {
    size_t cap = 4096, n = 0;
    char *b = xmalloc(cap);
    struct pollfd p = {fd, POLLIN, 0};
    while (poll(&p, 1, 1500) > 0) {
        if (n + 4096 > cap) { cap *= 2; b = xrealloc(b, cap); }
        ssize_t r = read(fd, b + n, cap - n - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += r;
    }
    b[n] = 0;
    if (len) *len = (int)n;
    return b;
}

static char *receive_offer(void *offer, bool primary, const char *mime, int *len) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return NULL;
    if (primary) zwp_primary_selection_offer_v1_receive(offer, mime, p[1]);
    else wl_data_offer_receive(offer, mime, p[1]);
    close(p[1]);
    wl_display_flush(W.dpy);
    char *s = read_fd_all(p[0], len);
    close(p[0]);
    return s;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static void dd_drop(void *d, struct wl_data_device *dd) {
    (void)d; (void)dd;
    struct wl_data_offer *o = W.dnd_offer;
    if (!o) return;
    OfferInfo *i = wl_data_offer_get_user_data(o);
    if (i && i->uris) {
        char *s = receive_offer(o, false, "text/uri-list", NULL);
        wl_data_offer_finish(o);
        char *paths[64];
        int n = 0;
        for (char *line = strtok(s, "\r\n"); line && n < 64; line = strtok(NULL, "\r\n")) {
            if (*line == '#' || strncmp(line, "file://", 7)) continue;
            char *p = line + 7;
            if (*p != '/') p = strchr(p, '/'); /* skip hostname */
            if (!p) continue;
            char *out = xmalloc(strlen(p) + 1), *q = out;
            for (; *p; p++) {
                if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { *q++ = (char)(hexv(p[1]) * 16 + hexv(p[2])); p += 2; }
                else *q++ = *p;
            }
            *q = 0;
            paths[n++] = out;
        }
        free(s);
        if (n) app_drop_files(paths, n);
        for (int k = 0; k < n; k++) free(paths[k]);
    }
    if (o != W.sel_offer) offer_free(o);
    W.dnd_offer = NULL;
}
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {
    (void)d; (void)dd;
    if (W.sel_offer && W.sel_offer != o) offer_free(W.sel_offer);
    W.sel_offer = o;
}
static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_offer, .enter = dd_enter, .leave = dd_leave, .motion = dd_motion, .drop = dd_drop, .selection = dd_selection};

static void poffer_mime(void *d, struct zwp_primary_selection_offer_v1 *o, const char *mime) {
    offer_mime(d, (struct wl_data_offer *)o, mime);
}
static const struct zwp_primary_selection_offer_v1_listener poffer_listener = {.offer = poffer_mime};
static void psd_offer(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o) {
    (void)d; (void)dev;
    zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, calloc(1, sizeof(OfferInfo)));
}
static void psd_selection(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o) {
    (void)d; (void)dev;
    if (W.psel_offer && W.psel_offer != o) {
        free(zwp_primary_selection_offer_v1_get_user_data(W.psel_offer));
        zwp_primary_selection_offer_v1_destroy(W.psel_offer);
    }
    W.psel_offer = o;
}
static const struct zwp_primary_selection_device_v1_listener psd_listener = {.data_offer = psd_offer, .selection = psd_selection};

static void send_text(int which, int fd) {
    const char *s = W.clip[which];
    int n = W.clip_len[which];
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        s += w;
        n -= (int)w;
    }
    close(fd);
}
static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *m, int32_t fd) { (void)d; (void)s; (void)m; send_text(CLIP_CLIPBOARD, fd); }
static void src_cancelled(void *d, struct wl_data_source *s) {
    (void)d;
    wl_data_source_destroy(s);
    if (W.src == s) { W.src = NULL; W.own[CLIP_CLIPBOARD] = false; }
}
static void src_dnd(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener src_listener = {
    .target = src_target, .send = src_send, .cancelled = src_cancelled,
    .dnd_drop_performed = src_dnd, .dnd_finished = src_dnd, .action = src_action};

static void psrc_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *m, int32_t fd) {
    (void)d; (void)s; (void)m;
    send_text(CLIP_PRIMARY, fd);
}
static void psrc_cancelled(void *d, struct zwp_primary_selection_source_v1 *s) {
    (void)d;
    zwp_primary_selection_source_v1_destroy(s);
    if (W.psrc == s) { W.psrc = NULL; W.own[CLIP_PRIMARY] = false; }
}
static const struct zwp_primary_selection_source_v1_listener psrc_listener = {.send = psrc_send, .cancelled = psrc_cancelled};

static const char *text_mimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"};

void plat_set_clipboard(int which, const char *s, int n) {
    free(W.clip[which]);
    W.clip[which] = xstrndup(s, n);
    W.clip_len[which] = n;
    W.own[which] = true;
    if (which == CLIP_CLIPBOARD && W.dd) {
        if (W.src) wl_data_source_destroy(W.src);
        W.src = wl_data_device_manager_create_data_source(W.ddm);
        wl_data_source_add_listener(W.src, &src_listener, NULL);
        for (size_t i = 0; i < sizeof text_mimes / sizeof *text_mimes; i++) wl_data_source_offer(W.src, text_mimes[i]);
        wl_data_device_set_selection(W.dd, W.src, W.serial);
    } else if (which == CLIP_PRIMARY && W.psd) {
        if (W.psrc) zwp_primary_selection_source_v1_destroy(W.psrc);
        W.psrc = zwp_primary_selection_device_manager_v1_create_source(W.psm);
        zwp_primary_selection_source_v1_add_listener(W.psrc, &psrc_listener, NULL);
        for (size_t i = 0; i < sizeof text_mimes / sizeof *text_mimes; i++) zwp_primary_selection_source_v1_offer(W.psrc, text_mimes[i]);
        zwp_primary_selection_device_v1_set_selection(W.psd, W.psrc, W.serial);
    }
}

char *plat_get_clipboard(int which) {
    bool have_device = which == CLIP_CLIPBOARD ? W.dd != NULL : W.psd != NULL;
    if (W.own[which] || !have_device) return W.clip[which] ? xstrndup(W.clip[which], W.clip_len[which]) : NULL;
    void *o = which == CLIP_CLIPBOARD ? (void *)W.sel_offer : (void *)W.psel_offer;
    if (!o) return NULL;
    OfferInfo *i = which == CLIP_CLIPBOARD ? wl_data_offer_get_user_data(o) : zwp_primary_selection_offer_v1_get_user_data(o);
    const char *mime = i && i->utf8 ? "text/plain;charset=utf-8" : i && i->text ? "text/plain" : NULL;
    if (!mime) return NULL;
    return receive_offer(o, which == CLIP_PRIMARY, mime, NULL);
}

/* ------------------------------------------------------------------ seat / registry */
static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !W.ptr) {
        W.ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(W.ptr, &ptr_listener, NULL);
        if (W.csm) W.csd = wp_cursor_shape_manager_v1_get_pointer(W.csm, W.ptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !W.kbd) {
        W.kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(W.kbd, &kbd_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (getenv("NOCTEDIT_DEBUG")) fprintf(stderr, "global %s v%u\n", iface, ver);
#define BIND(var, intf, maxv) var = wl_registry_bind(r, name, &intf, MIN(ver, (uint32_t)(maxv)))
    if (!strcmp(iface, wl_compositor_interface.name)) { W.comp_ver = MIN(ver, 6u); BIND(W.comp, wl_compositor_interface, 6); }
    else if (!strcmp(iface, wl_shm_interface.name)) BIND(W.shm, wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { BIND(W.wm, xdg_wm_base_interface, 5); xdg_wm_base_add_listener(W.wm, &wm_listener, NULL); }
    else if (!strcmp(iface, wl_seat_interface.name) && !W.seat) { BIND(W.seat, wl_seat_interface, 7); wl_seat_add_listener(W.seat, &seat_listener, NULL); }
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) BIND(W.ddm, wl_data_device_manager_interface, 3);
    else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name)) BIND(W.psm, zwp_primary_selection_device_manager_v1_interface, 1);
    else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) BIND(W.decoman, zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) BIND(W.csm, wp_cursor_shape_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) BIND(W.fsm, wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_viewporter_interface.name)) BIND(W.vp, wp_viewporter_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && W.noutputs < 8 && ver >= 2) {
        Output *o = &W.outputs[W.noutputs++];
        o->name = name;
        o->scale = 1;
        o->o = wl_registry_bind(r, name, &wl_output_interface, MIN(ver, 4u));
        wl_output_add_listener(o->o, &out_listener, o);
    }
#undef BIND
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = {.global = reg_global, .global_remove = reg_remove};

bool plat_init(const char *app_id, const char *title, int w, int h) {
    signal(SIGPIPE, SIG_IGN);
    setlocale(LC_CTYPE, "");
    W.dpy = wl_display_connect(NULL);
    if (!W.dpy) { fprintf(stderr, "noctedit: cannot connect to a Wayland display\n"); return false; }
    W.reg = wl_display_get_registry(W.dpy);
    wl_registry_add_listener(W.reg, &reg_listener, NULL);
    wl_display_roundtrip(W.dpy);
    if (!W.comp || !W.shm || !W.wm) { fprintf(stderr, "noctedit: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n"); return false; }
    wl_display_roundtrip(W.dpy); /* seat capabilities, output scales */

    W.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_CTYPE");
    if (!loc || !*loc) loc = getenv("LANG");
    if (!loc || !*loc) loc = "C";
    W.ctab = xkb_compose_table_new_from_locale(W.xkb, loc, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (W.ctab) W.cst = xkb_compose_state_new(W.ctab, XKB_COMPOSE_STATE_NO_FLAGS);
    W.rep_rate = 25;
    W.rep_delay = 400;
    W.rep_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    for (int i = 0; i < TIMER_COUNT; i++) W.timer_fd[i] = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

    if (W.seat && W.ddm) { W.dd = wl_data_device_manager_get_data_device(W.ddm, W.seat); wl_data_device_add_listener(W.dd, &dd_listener, NULL); }
    if (W.seat && W.psm) { W.psd = zwp_primary_selection_device_manager_v1_get_device(W.psm, W.seat); zwp_primary_selection_device_v1_add_listener(W.psd, &psd_listener, NULL); }

    W.surf = wl_compositor_create_surface(W.comp);
    wl_surface_add_listener(W.surf, &surf_listener, NULL);
    if (W.fsm && W.vp) {
        W.fs = wp_fractional_scale_manager_v1_get_fractional_scale(W.fsm, W.surf);
        wp_fractional_scale_v1_add_listener(W.fs, &fs_listener, NULL);
        W.viewport = wp_viewporter_get_viewport(W.vp, W.surf);
    }
    W.int_scale = 1;
    for (int i = 0; i < W.noutputs; i++) W.int_scale = MAX(W.int_scale, W.outputs[i].scale); /* best guess until enter */
    W.xsurf = xdg_wm_base_get_xdg_surface(W.wm, W.surf);
    xdg_surface_add_listener(W.xsurf, &xsurf_listener, NULL);
    W.top = xdg_surface_get_toplevel(W.xsurf);
    xdg_toplevel_add_listener(W.top, &top_listener, NULL);
    xdg_toplevel_set_app_id(W.top, app_id);
    xdg_toplevel_set_title(W.top, title);
    xdg_toplevel_set_min_size(W.top, 320, 200);
    if (W.decoman) {
        W.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(W.decoman, W.top);
        zxdg_toplevel_decoration_v1_add_listener(W.deco, &deco_listener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(W.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    W.w = w;
    W.h = h;
    W.cursor = -1;
    wl_surface_commit(W.surf);
    W.running = true;
    return true;
}

void plat_quit(void) { W.running = false; }
void plat_set_title(const char *t) { if (W.top) xdg_toplevel_set_title(W.top, t); }
void plat_move(void) { if (W.top && W.seat) xdg_toplevel_move(W.top, W.seat, W.serial); }
void plat_toggle_maximize(void) {
    if (!W.top) return;
    if (W.maximized) xdg_toplevel_unset_maximized(W.top);
    else xdg_toplevel_set_maximized(W.top);
}
void plat_minimize(void) { if (W.top) xdg_toplevel_set_minimized(W.top); }
bool plat_csd(void) { return !W.ssd; }
uint32_t plat_mods(void) { return W.mods; }

void plat_timer(int id, int first_ms, int repeat_ms) {
    struct itimerspec its = {
        .it_value = {first_ms / 1000, (first_ms % 1000) * 1000000L},
        .it_interval = {repeat_ms / 1000, (repeat_ms % 1000) * 1000000L}};
    timerfd_settime(W.timer_fd[id], 0, &its, NULL);
}

void plat_watch_fd(int fd, void (*cb)(int, void *), void *ud) {
    if (W.nwatch < 4) W.watch[W.nwatch++] = (typeof(W.watch[0])){fd, cb, ud};
}

int plat_run(void) {
    int dfd = wl_display_get_fd(W.dpy);
    while (W.running) {
        draw_frame();
        while (wl_display_prepare_read(W.dpy) != 0) {
            if (wl_display_dispatch_pending(W.dpy) < 0) return 1;
        }
        bool want_out = false;
        if (wl_display_flush(W.dpy) < 0) {
            if (errno != EAGAIN) { wl_display_cancel_read(W.dpy); return 1; }
            want_out = true;
        }
        struct pollfd fds[2 + TIMER_COUNT + 4];
        int n = 0;
        fds[n++] = (struct pollfd){dfd, POLLIN | (want_out ? POLLOUT : 0), 0};
        fds[n++] = (struct pollfd){W.rep_fd, POLLIN, 0};
        for (int i = 0; i < TIMER_COUNT; i++) fds[n++] = (struct pollfd){W.timer_fd[i], POLLIN, 0};
        for (int i = 0; i < W.nwatch; i++) fds[n++] = (struct pollfd){W.watch[i].fd, POLLIN, 0};
        if (poll(fds, n, -1) < 0) {
            wl_display_cancel_read(W.dpy);
            if (errno == EINTR) continue;
            return 1;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(W.dpy) < 0) return 1;
        } else {
            wl_display_cancel_read(W.dpy);
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) return 1;
        if (wl_display_dispatch_pending(W.dpy) < 0) return 1;
        uint64_t exp;
        if ((fds[1].revents & POLLIN) && read(W.rep_fd, &exp, 8) == 8 && W.rep_code) {
            for (uint64_t k = 0; k < MIN(exp, 4u); k++) app_key(W.rep_sym, W.mods, W.rep_utf8);
        }
        for (int i = 0; i < TIMER_COUNT; i++)
            if ((fds[2 + i].revents & POLLIN) && read(W.timer_fd[i], &exp, 8) == 8) app_timer(i);
        for (int i = 0; i < W.nwatch; i++)
            if (fds[2 + TIMER_COUNT + i].revents & POLLIN) W.watch[i].cb(W.watch[i].fd, W.watch[i].ud);
    }
    for (int i = 0; i < 2; i++) buf_destroy(&W.bufs[i]);
    if (W.csd) wp_cursor_shape_device_v1_destroy(W.csd);
    if (W.ctheme) wl_cursor_theme_destroy(W.ctheme);
    if (W.deco) zxdg_toplevel_decoration_v1_destroy(W.deco);
    if (W.viewport) wp_viewport_destroy(W.viewport);
    if (W.fs) wp_fractional_scale_v1_destroy(W.fs);
    xdg_toplevel_destroy(W.top);
    xdg_surface_destroy(W.xsurf);
    wl_surface_destroy(W.surf);
    wl_display_flush(W.dpy);
    wl_display_disconnect(W.dpy);
    return 0;
}
