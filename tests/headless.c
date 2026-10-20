/* Headless platform for tests: replaces wl.c, runs a key script, writes PNG snapshots.
 * Script (argv after --script): tokens separated by spaces:
 *   type:TEXT     (\n, \t, \s escapes)    key:NAME[+mods]   snap:FILE.png   click:X,Y[,N]
 *   size:WxH@S    drag:X,Y-X2,Y2          wheel:N            check:EXPR (see below) */
#include "../src/noctedit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>
#include <zlib.h>

static int W = 1100, H = 720;
static double SC = 1;
static char *clip[2];
static bool csd = false;
static int script_argc;
static char **script_argv;

uint64_t plat_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
bool plat_init(const char *a, const char *t, int w, int h) { (void)a; (void)t; (void)w; (void)h; return true; }
void plat_quit(void) { printf("QUIT\n"); }
void plat_damage(Rect r) { (void)r; }
void plat_redraw(void) {}
static char title[512];
void plat_set_title(const char *t) { snprintf(title, sizeof title, "%s", t); }
void plat_set_cursor(int s) { (void)s; }
void plat_set_clipboard(int w, const char *s, int n) { free(clip[w]); clip[w] = xstrndup(s, n); }
char *plat_get_clipboard(int w) { return clip[w] ? xstrdup(clip[w]) : NULL; }
void plat_timer(int id, int a, int b) { (void)id; (void)a; (void)b; }
void plat_move(void) {}
void plat_toggle_maximize(void) {}
void plat_minimize(void) {}
bool plat_csd(void) { return csd; }
uint32_t plat_mods(void) { return 0; }
void plat_watch_fd(int fd, void (*cb)(int, void *), void *ud) { (void)fd; (void)cb; (void)ud; }
void plat_trim(void) {}

static void png_write(const char *path, uint32_t *px, int w, int h) {
    size_t rawn = (size_t)h * (w * 3 + 1);
    unsigned char *raw = malloc(rawn), *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) { uint32_t v = px[(size_t)y * w + x]; *p++ = v >> 16; *p++ = v >> 8; *p++ = v; }
    }
    uLongf zn = compressBound(rawn);
    unsigned char *z = malloc(zn);
    compress2(z, &zn, raw, rawn, 6);
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    void chunk(const char *type, const unsigned char *d, uint32_t n) {
        unsigned char hdr[8] = {n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3]};
        fwrite(hdr, 1, 8, f);
        if (n) fwrite(d, 1, n, f);
        uLong c = crc32(0, hdr + 4, 4);
        if (n) c = crc32(c, d, n);
        unsigned char cb[4] = {c >> 24, c >> 16, c >> 8, c};
        fwrite(cb, 1, 4, f);
    }
    unsigned char ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z, (uint32_t)zn);
    chunk("IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

static uint32_t *fb;
static void render(void) {
    fb = realloc(fb, (size_t)W * H * 4);
    Canvas c = {.px = fb, .w = W, .h = H, .stride = W};
    cv_clip(&c, (Rect){0, 0, W, H});
    app_render(&c);
}

static uint32_t keysym(const char *name, uint32_t *mods) {
    char buf[64], key[64] = "";
    snprintf(buf, sizeof buf, "%s", name);
    *mods = 0;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        if (!strcmp(tok, "ctrl")) *mods |= MOD_CTRL;
        else if (!strcmp(tok, "shift")) *mods |= MOD_SHIFT;
        else if (!strcmp(tok, "alt")) *mods |= MOD_ALT;
        else snprintf(key, sizeof key, "%s", tok);
    }
    if (!strcmp(name + strlen(name) - 1, "+") ) snprintf(key, sizeof key, "plus");
    uint32_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
    if ((*mods & MOD_SHIFT) && sym >= 'a' && sym <= 'z') sym -= 32;
    return sym;
}

int plat_run(void) {
    app_resize(W, H, SC);
    render();
    for (int i = 0; i < script_argc; i++) {
        char *t = script_argv[i];
        if (i) render(); /* the real app paints after each event */
        if (!strncmp(t, "type:", 5)) {
            for (char *s = t + 5; *s; s++) {
                char ch[8] = {0};
                uint32_t sym;
                if (*s == '\\' && s[1]) {
                    s++;
                    if (*s == 'n') { app_key(XKB_KEY_Return, 0, "\r"); continue; }
                    if (*s == 't') { app_key(XKB_KEY_Tab, 0, "\t"); continue; }
                    ch[0] = *s == 's' ? ' ' : *s;
                } else {
                    int l = 1;
                    unsigned char c0 = *s;
                    if (c0 >= 0xC0) l = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : 2;
                    memcpy(ch, s, l);
                    s += l - 1;
                }
                sym = (unsigned char)ch[0] < 0x80 ? (uint32_t)(unsigned char)ch[0] : 0;
                app_key(sym, 0, ch);
            }
        } else if (!strncmp(t, "key:", 4)) {
            uint32_t mods, sym = keysym(t + 4, &mods);
            char u[8] = {0};
            if (sym >= 0x20 && sym < 0x7f && !(mods & MOD_CTRL)) u[0] = (char)sym;
            app_key(sym, mods, u);
        } else if (!strncmp(t, "snap:", 5)) {
            render();
            render(); /* second pass uses zones from the first (hover etc.) */
            png_write(t + 5, fb, W, H);
            printf("SNAP %s title=\"%s\"\n", t + 5, title);
        } else if (!strncmp(t, "click:", 6)) {
            int x, y, n = 1;
            sscanf(t + 6, "%d,%d,%d", &x, &y, &n);
            render();
            app_motion(x * SC, y * SC);
            app_button(1, true, x * SC, y * SC, 0, n);
            app_button(1, false, x * SC, y * SC, 0, n);
        } else if (!strncmp(t, "hover:", 6)) {
            int x, y;
            sscanf(t + 6, "%d,%d", &x, &y);
            render();
            app_motion(x * SC, y * SC);
        } else if (!strncmp(t, "drag:", 5)) {
            int x, y, x2, y2;
            sscanf(t + 5, "%d,%d-%d,%d", &x, &y, &x2, &y2);
            render();
            app_button(1, true, x * SC, y * SC, 0, 1);
            app_motion(x2 * SC, y2 * SC);
            app_button(1, false, x2 * SC, y2 * SC, 0, 1);
        } else if (!strncmp(t, "wheel:", 6)) {
            render();
            app_scroll(0, atoi(t + 6), true);
        } else if (!strncmp(t, "size:", 5)) {
            sscanf(t + 5, "%dx%d@%lf", &W, &H, &SC);
            W = (int)(W * SC);
            H = (int)(H * SC);
            app_resize(W, H, SC);
        } else if (!strncmp(t, "csd", 3)) {
            csd = true;
        } else if (!strncmp(t, "clip", 4)) {
            printf("CLIP [%s]\n", clip[0] ? clip[0] : "");
        } else if (!strncmp(t, "bench:", 6)) {
            int n = atoi(t + 6);
            uint64_t t0 = plat_now_ms();
            for (int k = 0; k < n; k++) render();
            printf("BENCH %d frames %dx%d: %.2f ms/frame\n", n, W, H, (plat_now_ms() - t0) / (double)n);
        } else if (!strncmp(t, "wait:", 5)) {
            struct timespec ts = {0, atoi(t + 5) * 1000000L};
            nanosleep(&ts, NULL);
        } else if (!strncmp(t, "timer:", 6)) {
            app_timer(atoi(t + 6));
        }
    }
    return 0;
}

int app_main(int argc, char **argv);
int main(int argc, char **argv) {
    int split = argc;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--script")) { split = i; break; }
    script_argc = split < argc ? argc - split - 1 : 0;
    script_argv = argv + split + 1;
    argv[split] = NULL;
    return app_main(split, argv);
}
