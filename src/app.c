/* noctedit — editor UI, commands and input handling. */
#define _GNU_SOURCE
#include "noctedit.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/* Tabler icon codepoints (Noctalia ships the font) */
enum {
    I_FILE = 0xEAA4, I_FILE_TEXT = 0xEAA2, I_SAVE = 0xEB62, I_SEARCH = 0xEB1C, I_X = 0xEB55, I_PLUS = 0xEB0B,
    I_MENU = 0xEC42, I_FOLDER_OPEN = 0xFAF7, I_FOLDER = 0xEAAD, I_COMMAND = 0xEA78, I_WRAP = 0xEBDD,
    I_DOT = 0xF698, I_REPLACE = 0xEBC7, I_LINES = 0xEF11, I_DOWN = 0xEA16, I_UP = 0xEA25, I_CASE = 0xEEA5,
    I_ALERT = 0xEA06, I_UNDO = 0xEB77, I_REDO = 0xEB78, I_COPY = 0xEA7A, I_PASTE = 0xEA6F, I_CUT = 0xEA86,
    I_ZOOM_IN = 0xEB56, I_ZOOM_OUT = 0xEB57, I_MINUS = 0xEAF2, I_SQUARE = 0xEB2C, I_CHEV_DOWN = 0xEA5F,
    I_CHEV_RIGHT = 0xEA61, I_CODE = 0xEA77, I_INDENT = 0xEB92, I_OUTDENT = 0xEB91, I_FILE_PLUS = 0xEAA0,
    I_EXIT = 0xEF4D, I_REFRESH = 0xEB13, I_PALETTE = 0xEB01, I_TYPO = 0xEBC5, I_CODE_FILE = 0xEBD0,
    I_EYE = 0xEA9A, I_SORT = 0xEB26, I_UPPER = 0xEEA4, I_LOWER = 0xEEA2, I_TRASH = 0xEB41, I_DUP = 0xFDAE,
    I_ARROWS_V = 0xEB5B, I_FOLDER_UP = 0xF91C, I_CHECK = 0xEA5E, I_HOME = 0xEAC1, I_EXCHANGE = 0xF1F4,
    I_SPACE = 0xEC0C, I_PILCROW = 0xF5F6, I_LETTER_W = 0xEC66,
    I_BOLD = 0xEB7B, I_ITALIC = 0xEB93, I_UNDERLINE = 0xEBA2, I_STRIKE = 0xEB9E, I_AL_L = 0xEA09, I_AL_C = 0xEA07,
    I_AL_R = 0xEA0A, I_AL_J = 0xEA08, I_LIST = 0xEB6B, I_LINE_H = 0xEB94, I_QUOTE = 0xEE09, I_HEADING = 0xEE9B, I_HISTORY = 0xEBEA,
    I_SPELL = 0xF2A6, I_PDF = 0xFB10, I_EXPORT = 0xEDE9, I_BOOK = 0xEA39, I_LOCK = 0xEAE2, I_ABC = 0xF567,
};

typedef struct {
    Color bg, card, field, panel, text, muted, faint, primary, on_primary, sel, line_hl, match, bracket,
        caret, border, thumb, hover, error, shadow, syn[T_COUNT];
} Colors;

typedef struct { char s[1024]; int len, cur; bool all; } Input;

enum { M_EDIT, M_FIND, M_GOTO, M_PALETTE, M_DIALOG, M_BROWSER, M_SPELL, M_MENU };
enum { DLG_CLOSE, DLG_OVERWRITE };
enum { BR_OPEN, BR_SAVE, BR_EXPORT };
enum {
    A_NONE, A_TAB, A_TAB_CLOSE, A_NEWTAB, A_MENU, A_WIN_MIN, A_WIN_MAX, A_WIN_CLOSE, A_TITLEBAR,
    A_FIND_FIELD, A_REPL_FIELD, A_FIND_CASE, A_FIND_WORD, A_FIND_PREV, A_FIND_NEXT, A_FIND_CLOSE,
    A_FIND_TOGGLE, A_REPL_ONE, A_REPL_ALL, A_PAL_ITEM, A_DLG_BTN, A_BR_ITEM, A_BR_UP, A_BR_HIDDEN,
    A_ST_POS, A_ST_INDENT, A_ST_EOL, A_ST_LANG, A_ST_WRAP, A_ST_DISK, A_SCROLLBAR, A_TEXT, A_MODAL_BG, A_PANEL,
    A_SPELL_ITEM, A_ST_SPELL, A_TB_STYLE, A_TB_FMT, A_TB_ALIGN, A_TB_SPACING, A_TB_LIST, A_TB_INDENT,
    A_MENU_ITEM, A_ST_MODE, A_ST_WORDS,
};

typedef struct { Rect r; int act, arg; } Zone;
typedef struct { char *name; bool dir; } Entry;
typedef struct { const char *name, *keys; uint32_t icon; void (*fn)(void); } Cmd;

static struct {
    Config cfg;
    Palette pal;
    Colors c;
    double scale;
    int W, H;
    Font *mono, *ui, *ui_big, *icons;
    int lh, asc, sp, sp26, ui_asc, ui_h, zoom;

    Doc **docs;
    int ndocs, active;
    int mode;

    bool find_open, replace_open, find_case, find_word;
    int find_focus;
    Input fin, rep;
    Pos find_origin;
    uint64_t fc_ver; const Doc *fc_doc; char fc_needle[1024]; bool fc_case, fc_word; int fc_total;

    Input gin;
    Input pin;
    int pal_sel, pal_scroll, pal_n;
    int pal_items[512];

    int dlg_kind, dlg_arg, dlg_nbtn, dlg_sel;
    char dlg_title[256], dlg_msg[512];
    const char *dlg_btn[3];
    char *dlg_path;

    int br_mode, br_doc;
    char br_dir[PATH_MAX];
    Input bin;
    Entry *br_ent;
    int br_n, br_cap;
    int *br_f;
    int br_fn, br_sel, br_scroll;
    bool br_hidden, br_nav; /* br_nav: the user picked an entry with arrows/mouse */

    char toast[300];
    bool toast_err;

    Zone zones[512];
    int nz;
    int hover_act, hover_arg;

    Rect r_top, r_card, r_text, r_gutter, r_status, r_sb_track, r_sb_thumb;
    int tabs_scroll, vis_rows;

    bool caret_on, focused;
    uint64_t last_act;
    Rect caret_rect;

    double mx, my;
    bool drag_text, drag_sb;
    int drag_mode, drag_sb_off;
    Pos drag_a, drag_b;
    double scroll_acc;

    bool quitting;
    /* spellcheck popup */
    int sp_line, sp_a, sp_b, sp_n, sp_sel, sp_x, sp_y;
    char sp_sugg[8][64];
    bool spell_warned;
    int menu_kind, menu_n, menu_sel, menu_x, menu_y;
    char menu_lab[12][48];
    int menu_val[12];
    bool menu_chk[12];
    Rect r_tool;
    /* animation state */
    uint64_t now, pop_t0, find_t0, toast_t0, toast_out, so_t0, tab_t0, hv_t0, c_t0;
    float soff, so_from, pop_k;
    int pop_dy, last_mode, phv_act, phv_arg, tab_last, tab_prev, tab_ndocs;
    float tab_fx, tab_fw, tab_dx, tab_dw;
    bool last_find, anim_running;
    /* caret glide */
    int ct_x, ct_y, cf_x, cf_y;
    const Doc *ct_doc;
    bool cp_on;
    int cp_x, cp_y, cp_w, cp_h;
    char *clip_line;
    char title[400];
} A;

#define S(v) ((int)lround((v) * A.scale))

/* document-view state shared across the file */
static Font *dfont[FN_COUNT];
static int dfont_px;
static uint16_t *g_st;
static int g_st_cap;
static Para g_para;
static bool g_para_code;
static void dv_ensure_visible(Doc *d);
static float prog(uint64_t t0, int ms);
static float ease(float t);
static bool anim_on(void);
static float scroll_off_now(void);
static void dv_clamp(Doc *d);
static Pos dv_pos_at(double mx, double my);
static void dv_parse_line(Doc *d, int li, bool show);
static void load_doc_fonts(void);
static int dv_body_lh(Doc *d);
static bool looks_prose(Doc *d);
static bool props_load(Doc *d);
static void recent_add(const char *path);
static void props_save(Doc *d);
static Doc *D(void) { return A.docs[A.active]; }

static void redraw(void) { plat_redraw(); }

static void toast(bool err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void toast(bool err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof A.toast, fmt, ap);
    va_end(ap);
    A.toast_err = err;
    A.toast_t0 = plat_now_ms();
    A.toast_out = 0;
    plat_timer(TIMER_TOAST, err ? 4000 : 2200, 0);
    redraw();
}

/* ================================================================== theme & fonts */
static void derive_colors(void) {
    Palette *p = &A.pal;
    Colors *c = &A.c;
    c->bg = p->surface;
    c->card = p->surface_container;
    if ((c->card & 0xffffff) == (c->bg & 0xffffff)) c->card = color_mix(p->surface, p->on_surface, 0.04f);
    c->panel = color_mix(p->surface, p->surface_container, 0.5f);
    c->field = color_mix(c->card, p->on_surface, p->light ? 0.05f : 0.06f);
    c->text = p->on_surface;
    c->muted = p->on_surface_variant;
    c->faint = color_mix(p->on_surface_variant, c->card, 0.5f);
    c->primary = p->primary;
    c->on_primary = p->on_primary;
    c->sel = color_alpha(p->primary, p->light ? 0x45 : 0x4c);
    c->line_hl = color_alpha(p->on_surface, p->light ? 0x0d : 0x0b);
    c->match = color_alpha(p->tertiary, 0x55);
    c->bracket = color_alpha(p->tertiary, 0x50);
    c->caret = p->primary;
    c->border = color_mix(p->outline, p->surface, 0.45f);
    c->thumb = color_alpha(p->on_surface_variant, 0x55);
    c->hover = color_mix(c->card, p->primary, 0.16f);
    c->error = p->error;
    c->shadow = color_alpha(0, 0x40);
    c->syn[T_TEXT] = p->on_surface;
    c->syn[T_KW] = p->primary;
    c->syn[T_TYPE] = p->secondary;
    c->syn[T_STR] = p->tertiary;
    c->syn[T_NUM] = color_mix(p->secondary, p->error, 0.4f);
    c->syn[T_CONST] = c->syn[T_NUM];
    c->syn[T_COMMENT] = color_mix(p->outline, p->on_surface_variant, 0.25f);
    c->syn[T_PRE] = color_mix(p->primary, p->error, 0.5f);
    c->syn[T_FUNC] = color_mix(p->tertiary, p->primary, 0.5f);
    c->syn[T_PUNCT] = color_mix(p->on_surface, p->outline, 0.45f);
    c->syn[T_HEAD] = p->primary;
}

static const char *icon_font_paths[] = {
    "/usr/share/noctalia/assets/fonts/noctalia-tabler.ttf",
    "/usr/share/noctalia-shell/Assets/Fonts/tabler/tabler-icons.ttf",
    "/etc/xdg/quickshell/noctalia-shell/Assets/Fonts/tabler/tabler-icons.ttf",
    "/usr/share/fonts/TTF/tabler-icons.ttf",
};

static void load_mono(void) {
    font_free(A.mono);
    int px = MAX(6, (int)lround((A.cfg.font_size + A.zoom) * A.scale));
    A.mono = font_open(A.cfg.font, px);
    if (!A.mono) A.mono = font_open("monospace", px);
    if (!A.mono) { fprintf(stderr, "noctedit: no usable monospace font\n"); exit(1); }
    A.lh = font_height(A.mono) + S(4);
    A.asc = font_ascent(A.mono) + S(2);
    A.sp = MAX(1, font_adv(A.mono, ' '));
    A.sp26 = MAX(64, font_adv26(A.mono, ' '));
}

static void load_fonts(void) {
    load_mono();
    font_free(A.ui);
    font_free(A.ui_big);
    font_free(A.icons);
    int px = MAX(6, S(A.cfg.ui_font_size));
    A.ui = font_open(A.cfg.ui_font, px);
    if (!A.ui) A.ui = font_open("sans-serif", px);
    if (!A.ui) A.ui = font_open(A.cfg.font, px);
    A.ui_big = font_open(A.cfg.ui_font, (int)lround(px * 1.3));
    if (!A.ui_big) A.ui_big = font_open("sans-serif:weight=bold", (int)lround(px * 1.3));
    A.icons = NULL;
    const char *home = getenv("HOME");
    char extra[PATH_MAX];
    snprintf(extra, sizeof extra, "%s/.local/share/fonts/tabler-icons.ttf", home ? home : "");
    for (size_t i = 0; i <= sizeof icon_font_paths / sizeof *icon_font_paths && !A.icons; i++) {
        const char *p = i < sizeof icon_font_paths / sizeof *icon_font_paths ? icon_font_paths[i] : extra;
        if (access(p, R_OK) == 0) A.icons = font_open_file(p, (int)lround(px * 1.25));
    }
    A.ui_asc = font_ascent(A.ui);
    A.ui_h = font_height(A.ui);
}

static void reload_theme(void) {
    palette_load(&A.pal);
    derive_colors();
    redraw();
}

static void theme_watch_cb(int fd, void *ud) {
    (void)ud;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool hit = false;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (e->len && (!strcmp(e->name, "theme.json") || !strcmp(e->name, "noctalia.css"))) hit = true;
            p += sizeof *e + e->len;
        }
    }
    if (hit) reload_theme();
}

/* ================================================================== text layout */
static int *g_rows;
static int g_rows_cap;

/* code-view metrics are kept in 26.6 fixed point so columns don't drift */
static inline int adv_cp(const Doc *d, int x26, uint32_t cp) {
    if (cp == '\t') {
        int tw = MAX(64, A.sp26 * d->tab_w);
        return tw - (x26 % tw);
    }
    if (cp < 0x20) return A.sp26;
    return font_adv26(A.mono, cp);
}

static int measure26(const Doc *d, const char *s, int from, int to) {
    int x = 0;
    for (int i = from; i < to;) {
        int cl;
        uint32_t cp = utf8_dec(s + i, to - i, &cl);
        x += adv_cp(d, x, cp);
        i += cl;
    }
    return x;
}
static int measure(const Doc *d, const char *s, int from, int to) { return (measure26(d, s, from, to) + 32) >> 6; }

static int wrap_width(void) { return MAX(A.r_text.w - A.sp, A.sp * 8); }

/* fills g_rows with byte offsets of visual row starts; returns row count */
static int wrap_line(const Doc *d, int li) {
    if (!g_rows_cap) { g_rows_cap = 64; g_rows = xmalloc(g_rows_cap * sizeof(int)); }
    g_rows[0] = 0;
    if (!A.cfg.wrap) return 1;
    const Line *l = &d->lines[li];
    int W = wrap_width() << 6;
    int n = 1, x = 0, rs = 0, brk_at = -1;
    for (int i = 0; i < l->len;) {
        int cl;
        uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
        int w = adv_cp(d, x, cp);
        if (x + w > W && i > rs) {
            int brk = brk_at > rs ? brk_at : i;
            if (n == g_rows_cap) { g_rows_cap *= 2; g_rows = xrealloc(g_rows, g_rows_cap * sizeof(int)); }
            g_rows[n++] = brk;
            rs = brk;
            brk_at = -1;
            x = measure26(d, l->s, brk, i);
            continue;
        }
        x += w;
        if (cp == ' ' || cp == '\t') brk_at = i + cl;
        i += cl;
    }
    return n;
}

static int line_rows(const Doc *d, int li) { return A.cfg.wrap ? wrap_line(d, li) : 1; }

static int col_to_x(const Doc *d, int li, int col, int *row) {
    int n = wrap_line(d, li), r = 0;
    while (r + 1 < n && g_rows[r + 1] <= col) r++;
    if (row) *row = r;
    return measure(d, d->lines[li].s, g_rows[r], col);
}

static int x_to_col(const Doc *d, int li, int row, int x) {
    const Line *l = &d->lines[li];
    int n = wrap_line(d, li);
    row = CLAMP(row, 0, n - 1);
    int s = g_rows[row], e = row + 1 < n ? g_rows[row + 1] : l->len;
    int cx = 0;
    x <<= 6;
    for (int i = s; i < e;) {
        int cl;
        uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
        int w = adv_cp(d, cx, cp);
        if (x < cx + w / 2) return i;
        cx += w;
        i += cl;
    }
    if (row + 1 < n && e > s) return utf8_prev(l->s, e);
    return e;
}

static void step_rows(const Doc *d, int *line, int *row, int delta) {
    if (!A.cfg.wrap) {
        *line = CLAMP(*line + delta, 0, d->nlines - 1);
        *row = 0;
        return;
    }
    while (delta > 0) {
        if (*row + 1 < line_rows(d, *line)) (*row)++;
        else if (*line + 1 < d->nlines) { (*line)++; *row = 0; }
        else break;
        delta--;
    }
    while (delta < 0) {
        if (*row > 0) (*row)--;
        else if (*line > 0) { (*line)--; *row = line_rows(d, *line) - 1; }
        else break;
        delta++;
    }
}

static int rows_between(const Doc *d, int l0, int r0, int l1, int r1, int cap) {
    if (!A.cfg.wrap) return MIN(l1 - l0, cap + 1);
    int k = 0;
    while ((l0 < l1 || (l0 == l1 && r0 < r1)) && k <= cap) { step_rows(d, &l0, &r0, 1); k++; }
    return k;
}

static void clamp_scroll(Doc *d) {
    if (d->docmode) { dv_clamp(d); return; }
    if (d->top_line >= d->nlines) { d->top_line = d->nlines - 1; d->top_row = 0; }
    if (d->top_line < 0) d->top_line = 0;
    int n = line_rows(d, d->top_line);
    d->top_row = CLAMP(d->top_row, 0, n - 1);
    int ml = d->nlines - 1, mr = line_rows(d, ml) - 1;
    int back = A.vis_rows * 2 / 3;
    if (!A.cfg.wrap && d->nlines <= A.vis_rows) back = A.vis_rows; /* fits: no scrolling */
    step_rows(d, &ml, &mr, -back);
    if (d->top_line > ml || (d->top_line == ml && d->top_row > mr)) { d->top_line = ml; d->top_row = mr; }
    if (d->scroll_x < 0 || A.cfg.wrap) d->scroll_x = 0;
}

static void ensure_visible(Doc *d) {
    if (d->docmode) { dv_ensure_visible(d); return; }
    if (A.vis_rows <= 0) return;
    int vis = A.vis_rows, margin = MIN(2, vis / 4);
    int crow;
    int cx = col_to_x(d, d->cur.line, d->cur.col, &crow);
    if (d->cur.line < d->top_line || (d->cur.line == d->top_line && crow < d->top_row)) {
        d->top_line = d->cur.line;
        d->top_row = crow;
        step_rows(d, &d->top_line, &d->top_row, -margin);
    } else {
        int k = d->cur.line - d->top_line > vis + 2 ? vis + 2 : rows_between(d, d->top_line, d->top_row, d->cur.line, crow, vis + 2);
        if (k > vis - 1 - margin) {
            d->top_line = d->cur.line;
            d->top_row = crow;
            step_rows(d, &d->top_line, &d->top_row, -(vis - 1 - margin));
        }
    }
    if (!A.cfg.wrap) {
        int m = A.sp * 4, w = A.r_text.w;
        if (cx < d->scroll_x + m) d->scroll_x = MAX(0, cx - m);
        else if (cx > d->scroll_x + w - m) d->scroll_x = cx - w + m;
    }
    clamp_scroll(d);
}

static void center_cursor(Doc *d) {
    if (d->docmode) { d->top_line = d->cur.line; d->top_px = 0; dv_ensure_visible(d); return; }
    int crow;
    col_to_x(d, d->cur.line, d->cur.col, &crow);
    d->top_line = d->cur.line;
    d->top_row = crow;
    step_rows(d, &d->top_line, &d->top_row, -(A.vis_rows / 3));
    clamp_scroll(d);
    ensure_visible(d);
}

static Pos pos_at(double mx, double my) {
    Doc *d = D();
    if (d->docmode) return dv_pos_at(mx, my);
    int rowi = (int)floor((my - A.soff - A.r_text.y) / A.lh);
    int l = d->top_line, r = d->top_row;
    step_rows(d, &l, &r, rowi);
    if (rowi < 0 && l == 0 && r == 0 && my < A.r_text.y - A.lh) return (Pos){0, 0};
    int x = (int)(mx - A.r_text.x) + d->scroll_x;
    if (x < 0) x = 0;
    int last_l = d->nlines - 1;
    if (l == last_l) {
        int lr = d->top_line == l ? d->top_row : 0;
        int rows_below = rowi - rows_between(d, d->top_line, d->top_row, l, line_rows(d, l) - 1, rowi + 1);
        (void)lr;
        if (rows_below > 0 && r == line_rows(d, l) - 1) return (Pos){l, d->lines[l].len};
    }
    return (Pos){l, x_to_col(d, l, r, x)};
}


/* ---- smooth scrolling: measure how far the view moved during an event, then glide */
typedef struct { const Doc *d; int line, row, px; } ScrollSnap;
static ScrollSnap snap_scroll(void) { Doc *d = D(); return (ScrollSnap){d, d->top_line, d->top_row, d->top_px}; }
static int para_h(Doc *d, int li);
static int scroll_dist(Doc *d, ScrollSnap a) {
    int cap = A.r_text.h * 2;
    if (d->docmode) {
        if (d->top_line == a.line) return d->top_px - a.px;
        bool fwd = d->top_line > a.line;
        int lo = fwd ? a.line : d->top_line, hi = fwd ? d->top_line : a.line;
        int lo_px = fwd ? a.px : d->top_px, hi_px = fwd ? d->top_px : a.px;
        int dist = para_h(d, lo) - lo_px;
        for (int li = lo + 1; li < hi && dist < cap; li++) dist += para_h(d, li);
        dist += hi_px;
        return fwd ? dist : -dist;
    }
    bool fwd = d->top_line > a.line || (d->top_line == a.line && d->top_row > a.row);
    int rows = fwd ? rows_between(d, a.line, a.row, d->top_line, d->top_row, A.vis_rows * 2)
                   : rows_between(d, d->top_line, d->top_row, a.line, a.row, A.vis_rows * 2);
    return (fwd ? rows : -rows) * A.lh;
}
static void scroll_commit(ScrollSnap s) {
    if (s.d != D()) { A.so_from = 0; A.so_t0 = 0; return; }
    if (!anim_on()) return;
    int dpx = scroll_dist(D(), s);
    if (!dpx) return;
    A.now = plat_now_ms();
    float from = scroll_off_now() + dpx, lim = A.r_text.h * 0.8f;
    A.so_from = CLAMP(from, -lim, lim);
    A.so_t0 = A.now;
    A.now = 0;
}

/* ---- caret: draw after the text, gliding between positions */
static void caret_at(int x, int y, int w, int h) { A.cp_on = true; A.cp_x = x; A.cp_y = y; A.cp_w = w; A.cp_h = h; }
static void caret_flush(Canvas *c) {
    if (!A.cp_on) return;
    A.cp_on = false;
    int soff = (int)lroundf(A.soff);
    int tx = A.cp_x, ty = A.cp_y - soff;
    if (tx != A.ct_x || ty != A.ct_y || A.ct_doc != D()) {
        bool glide = anim_on() && A.cfg.smooth_caret && A.ct_doc == D() && abs(ty - A.ct_y) < A.r_text.h / 2;
        if (glide) {
            float e = ease(prog(A.c_t0, 90));
            A.cf_x = (int)lroundf(A.cf_x + (A.ct_x - A.cf_x) * e);
            A.cf_y = (int)lroundf(A.cf_y + (A.ct_y - A.cf_y) * e);
            A.c_t0 = A.now ? A.now : plat_now_ms();
        } else {
            A.cf_x = tx;
            A.cf_y = ty;
            A.c_t0 = 0;
        }
        A.ct_x = tx;
        A.ct_y = ty;
        A.ct_doc = D();
    }
    float e = ease(prog(A.c_t0, 90));
    int dx = (int)lroundf(A.cf_x + (A.ct_x - A.cf_x) * e), dy = (int)lroundf(A.cf_y + (A.ct_y - A.cf_y) * e) + soff;
    A.caret_rect = (Rect){dx - S(1), dy - S(2), A.cp_w + S(2), A.cp_h + S(4)};
    if (A.focused && (A.caret_on || !A.cfg.blink) && (A.mode == M_EDIT || A.mode == M_FIND))
        cv_rrect(c, dx, dy, A.cp_w, A.cp_h, A.cp_w / 2, A.c.caret);
    else if (!A.focused)
        cv_fill(c, dx, dy, MAX(1, S(1)), A.cp_h, color_alpha(A.c.caret, 0x80));
}

/* ================================================================== doc helpers */
static int cclass(uint32_t c) {
    if (c == ' ' || c == '\t') return 0;
    if (isalnum((int)(c < 128 ? c : 0)) || c == '_' || c >= 0x80) return 1;
    return 2;
}

static Pos word_left(Doc *d, Pos p) {
    if (p.col == 0) return p.line > 0 ? (Pos){p.line - 1, d->lines[p.line - 1].len} : p;
    Line *l = &d->lines[p.line];
    int i = p.col;
    while (i > 0 && cclass((unsigned char)l->s[i - 1]) == 0) i--;
    if (i > 0) {
        int j = utf8_prev(l->s, i), cl;
        int k = cclass(utf8_dec(l->s + j, l->len - j, &cl));
        while (i > 0) {
            j = utf8_prev(l->s, i);
            if (cclass(utf8_dec(l->s + j, l->len - j, &cl)) != k) break;
            i = j;
        }
    }
    return (Pos){p.line, i};
}

static Pos word_right(Doc *d, Pos p) {
    Line *l = &d->lines[p.line];
    if (p.col >= l->len) return p.line + 1 < d->nlines ? (Pos){p.line + 1, 0} : p;
    int i = p.col, cl;
    while (i < l->len && cclass((unsigned char)l->s[i]) == 0) i++;
    if (i < l->len) {
        int k = cclass(utf8_dec(l->s + i, l->len - i, &cl));
        while (i < l->len && cclass(utf8_dec(l->s + i, l->len - i, &cl)) == k) i += cl;
    }
    return (Pos){p.line, i};
}

static void word_at(Doc *d, Pos p, Pos *a, Pos *b) {
    Line *l = &d->lines[p.line];
    int cl, i = p.col;
    if (i >= l->len && i > 0) i = utf8_prev(l->s, i);
    if (l->len == 0) { *a = *b = (Pos){p.line, 0}; return; }
    int k = cclass(utf8_dec(l->s + i, l->len - i, &cl));
    int s = i, e = i;
    while (s > 0) {
        int j = utf8_prev(l->s, s);
        if (cclass(utf8_dec(l->s + j, l->len - j, &cl)) != k) break;
        s = j;
    }
    while (e < l->len && cclass(utf8_dec(l->s + e, l->len - e, &cl)) == k) e += cl;
    *a = (Pos){p.line, s};
    *b = (Pos){p.line, e};
}

static int first_nonws(const Line *l) {
    int i = 0;
    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
    return i;
}

static void begin(int kind) {
    if (D()->readonly) toast(true, "This PDF is read-only. Use Save As to keep an editable text copy");
    doc_begin(D(), kind, plat_now_ms());
}

static void delete_sel(Doc *d) {
    Pos a, b;
    doc_sel(d, &a, &b);
    doc_delete(d, a, b);
    d->cur = d->anc = a;
}

static void insert_text(const char *s, int n, int kind) {
    Doc *d = D();
    begin(doc_has_sel(d) ? EK_OTHER : kind);
    if (doc_has_sel(d)) delete_sel(d);
    d->cur = d->anc = doc_insert(d, d->cur, s, n);
    d->want_x = -1;
    ensure_visible(d);
}

static void sel_lines(Doc *d, int *a, int *b) {
    Pos p, q;
    doc_sel(d, &p, &q);
    *a = p.line;
    *b = q.line;
    if (q.line > p.line && q.col == 0) (*b)--;
}

static void set_primary_from_sel(void) {
    Doc *d = D();
    if (!doc_has_sel(d)) return;
    Pos a, b;
    doc_sel(d, &a, &b);
    int n;
    char *t = doc_text(d, a, b, &n);
    if (n < (1 << 24)) plat_set_clipboard(CLIP_PRIMARY, t, n);
    free(t);
}

/* ================================================================== find */
static inline bool eq_ci(const char *a, const char *b, int n, bool cs) {
    if (cs) return !memcmp(a, b, n);
    for (int i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

static bool word_ok(const char *s, int n, int i, int m) {
    if (i > 0 && cclass((unsigned char)s[i - 1]) == 1) return false;
    if (i + m < n && cclass((unsigned char)s[i + m]) == 1) return false;
    return true;
}

static int find_in(const char *s, int n, int from, const char *nd, int m, bool cs, bool w) {
    if (m == 0) return -1;
    for (int i = MAX(from, 0); i + m <= n; i++) {
        if ((cs ? s[i] == nd[0] : tolower((unsigned char)s[i]) == tolower((unsigned char)nd[0])) && eq_ci(s + i, nd, m, cs) &&
            (!w || word_ok(s, n, i, m)))
            return i;
    }
    return -1;
}

static int rfind_in(const char *s, int n, int before, const char *nd, int m, bool cs, bool w) {
    if (m == 0) return -1;
    for (int i = MIN(before - 1, n - m); i >= 0; i--)
        if (eq_ci(s + i, nd, m, cs) && (!w || word_ok(s, n, i, m))) return i;
    return -1;
}

static bool find_from(Doc *d, Pos from, int dir, bool select) {
    const char *nd = A.fin.s;
    int m = A.fin.len;
    if (!m) return false;
    int N = d->nlines;
    for (int k = 0; k <= N; k++) {
        int li = dir > 0 ? (from.line + k) % N : ((from.line - k) % N + N) % N;
        Line *l = &d->lines[li];
        int i;
        if (dir > 0) i = find_in(l->s, l->len, k == 0 ? from.col : 0, nd, m, A.find_case, A.find_word);
        else i = rfind_in(l->s, l->len, k == 0 ? from.col : l->len + 1, nd, m, A.find_case, A.find_word);
        if (k == N && li == from.line) {
            /* wrapped back to the starting line: only accept the part we skipped */
            if (i >= 0 && ((dir > 0 && i >= from.col) || (dir < 0 && i < from.col))) i = -1;
        }
        if (i >= 0) {
            if (select) {
                d->anc = (Pos){li, i};
                d->cur = (Pos){li, i + m};
                d->want_x = -1;
                ensure_visible(d);
            }
            return true;
        }
    }
    return false;
}

static bool sel_is_match(Doc *d) {
    if (!doc_has_sel(d) || d->cur.line != d->anc.line) return false;
    Pos a, b;
    doc_sel(d, &a, &b);
    Line *l = &d->lines[a.line];
    return b.col - a.col == A.fin.len && eq_ci(l->s + a.col, A.fin.s, A.fin.len, A.find_case) &&
           (!A.find_word || word_ok(l->s, l->len, a.col, A.fin.len));
}

static void find_next(int dir) {
    Doc *d = D();
    if (!A.fin.len) return;
    Pos a, b;
    doc_sel(d, &a, &b);
    Pos from = dir > 0 ? (sel_is_match(d) ? b : d->cur) : a;
    if (!find_from(d, from, dir, true)) toast(true, "No results for “%s”", A.fin.s);
}

static void find_counts(Doc *d, int *total, int *index) {
    if (A.fc_doc != d || A.fc_ver != d->version || strcmp(A.fc_needle, A.fin.s) || A.fc_case != A.find_case ||
        A.fc_word != A.find_word) {
        int t = 0;
        if (A.fin.len)
            for (int i = 0; i < d->nlines && t < 1000000; i++) {
                Line *l = &d->lines[i];
                for (int j = find_in(l->s, l->len, 0, A.fin.s, A.fin.len, A.find_case, A.find_word); j >= 0;
                     j = find_in(l->s, l->len, j + A.fin.len, A.fin.s, A.fin.len, A.find_case, A.find_word))
                    t++;
            }
        A.fc_doc = d;
        A.fc_ver = d->version;
        snprintf(A.fc_needle, sizeof A.fc_needle, "%s", A.fin.s);
        A.fc_case = A.find_case;
        A.fc_word = A.find_word;
        A.fc_total = t;
    }
    *total = A.fc_total;
    *index = 0;
    if (sel_is_match(d) && A.fc_total < 100000) {
        Pos a, b;
        doc_sel(d, &a, &b);
        int k = 0;
        for (int i = 0; i <= a.line; i++) {
            Line *l = &d->lines[i];
            for (int j = find_in(l->s, l->len, 0, A.fin.s, A.fin.len, A.find_case, A.find_word); j >= 0;
                 j = find_in(l->s, l->len, j + A.fin.len, A.fin.s, A.fin.len, A.find_case, A.find_word)) {
                if (i == a.line && j > a.col) break;
                k++;
            }
        }
        *index = k;
    }
}

static void replace_one(void) {
    Doc *d = D();
    if (sel_is_match(d)) {
        begin(EK_OTHER);
        delete_sel(d);
        d->cur = d->anc = doc_insert(d, d->cur, A.rep.s, A.rep.len);
    }
    find_next(1);
}

static void replace_all(void) {
    Doc *d = D();
    if (!A.fin.len) return;
    int count = 0;
    begin(EK_OTHER);
    for (int i = d->nlines - 1; i >= 0; i--) {
        Line *l = &d->lines[i];
        int pos[4096], np = 0;
        for (int j = find_in(l->s, l->len, 0, A.fin.s, A.fin.len, A.find_case, A.find_word); j >= 0 && np < 4096;
             j = find_in(l->s, l->len, j + A.fin.len, A.fin.s, A.fin.len, A.find_case, A.find_word))
            pos[np++] = j;
        for (int k = np - 1; k >= 0; k--) {
            doc_delete(d, (Pos){i, pos[k]}, (Pos){i, pos[k] + A.fin.len});
            if (A.rep.len) doc_insert(d, (Pos){i, pos[k]}, A.rep.s, A.rep.len);
            count++;
        }
    }
    d->cur = d->anc = doc_clamp(d, d->cur);
    toast(false, "Replaced %d occurrence%s", count, count == 1 ? "" : "s");
}

/* ================================================================== input fields */
static void input_set(Input *in, const char *s) {
    snprintf(in->s, sizeof in->s, "%s", s);
    in->len = (int)strlen(in->s);
    in->cur = in->len;
    in->all = in->len > 0;
}

static void input_insert(Input *in, const char *s, int n) {
    if (in->all) { in->len = in->cur = 0; in->s[0] = 0; in->all = false; }
    for (int i = 0; i < n; i++) if (s[i] == '\n' || s[i] == '\r') { n = i; break; }
    n = MIN(n, (int)sizeof in->s - 1 - in->len);
    if (n <= 0) return;
    memmove(in->s + in->cur + n, in->s + in->cur, in->len - in->cur + 1);
    memcpy(in->s + in->cur, s, n);
    in->len += n;
    in->cur += n;
}

/* returns true if the text changed */
static bool input_key(Input *in, uint32_t sym, uint32_t mods, const char *utf8, bool *handled) {
    *handled = true;
    bool ctrl = mods & MOD_CTRL;
    if (ctrl && (sym == XKB_KEY_a || sym == XKB_KEY_A)) { in->all = true; in->cur = in->len; return false; }
    if (ctrl && (sym == XKB_KEY_v || sym == XKB_KEY_V)) {
        char *t = plat_get_clipboard(CLIP_CLIPBOARD);
        if (t) { input_insert(in, t, (int)strlen(t)); free(t); }
        return true;
    }
    if (ctrl && (sym == XKB_KEY_c || sym == XKB_KEY_C)) { plat_set_clipboard(CLIP_CLIPBOARD, in->s, in->len); return false; }
    if (ctrl && (sym == XKB_KEY_x || sym == XKB_KEY_X)) {
        plat_set_clipboard(CLIP_CLIPBOARD, in->s, in->len);
        in->len = in->cur = 0; in->s[0] = 0; in->all = false;
        return true;
    }
    switch (sym) {
    case XKB_KEY_BackSpace:
        if (in->all) { in->len = in->cur = 0; in->s[0] = 0; in->all = false; return true; }
        if (in->cur > 0) {
            int p = ctrl ? in->cur : utf8_prev(in->s, in->cur);
            if (ctrl) {
                while (p > 0 && in->s[p - 1] == ' ') p--;
                while (p > 0 && in->s[p - 1] != ' ' && in->s[p - 1] != '/') p--;
                if (p == in->cur && p > 0) p--;
            }
            memmove(in->s + p, in->s + in->cur, in->len - in->cur + 1);
            in->len -= in->cur - p;
            in->cur = p;
            return true;
        }
        return false;
    case XKB_KEY_Delete:
        if (in->all) { in->len = in->cur = 0; in->s[0] = 0; in->all = false; return true; }
        if (in->cur < in->len) {
            int e = utf8_next(in->s, in->len, in->cur);
            memmove(in->s + in->cur, in->s + e, in->len - e + 1);
            in->len -= e - in->cur;
            return true;
        }
        return false;
    case XKB_KEY_Left: in->all = false; if (in->cur > 0) in->cur = utf8_prev(in->s, in->cur); return false;
    case XKB_KEY_Right: in->all = false; if (in->cur < in->len) in->cur = utf8_next(in->s, in->len, in->cur); return false;
    case XKB_KEY_Home: in->all = false; in->cur = 0; return false;
    case XKB_KEY_End: in->all = false; in->cur = in->len; return false;
    }
    if (!ctrl && !(mods & MOD_ALT) && utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
        input_insert(in, utf8, (int)strlen(utf8));
        return true;
    }
    *handled = false;
    return false;
}

/* ================================================================== documents & files */
static void update_title(void) {
    char t[400];
    Doc *d = D();
    snprintf(t, sizeof t, "%s%s — Noctedit", doc_modified(d) ? "• " : "", doc_name(d));
    if (strcmp(t, A.title)) { memcpy(A.title, t, sizeof t); plat_set_title(t); }
}

static void activate(int i) {
    if (i < 0 || i >= A.ndocs) return;
    A.active = i;
    A.drag_text = false;
    redraw();
}

static int add_doc(Doc *d) {
    A.docs = xrealloc(A.docs, (A.ndocs + 1) * sizeof(Doc *));
    A.docs[A.ndocs++] = d;
    return A.ndocs - 1;
}

static Doc *new_doc(void) {
    Doc *d = doc_new();
    d->tab_w = A.cfg.tab_w;
    d->use_spaces = A.cfg.use_spaces;
    d->spacing = 115;
    d->para_sp = 6;
    d->docmode = A.cfg.doc_default >= 1;
    return d;
}

static bool doc_empty_untitled(Doc *d) {
    return !d->path && !doc_modified(d) && d->nlines == 1 && d->lines[0].len == 0 && d->nundo == 0;
}

static void open_path(const char *path, int line) {
    char *rp = realpath(path, NULL);
    if (rp) {
        for (int i = 0; i < A.ndocs; i++)
            if (A.docs[i]->path && !strcmp(A.docs[i]->path, rp)) {
                free(rp);
                activate(i);
                if (line > 0) { Doc *d = D(); d->cur = d->anc = doc_clamp(d, (Pos){line - 1, 0}); center_cursor(d); }
                return;
            }
    }
    Doc *d = new_doc();
    char err[256];
    struct stat st;
    if (stat(path, &st) != 0 && errno == ENOENT) {
        /* new file: remember the path, create it on first save */
        if (rp) free(rp);
        char abs[PATH_MAX];
        if (path[0] == '/') snprintf(abs, sizeof abs, "%s", path);
        else { char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "."); snprintf(abs, sizeof abs, "%s/%s", cwd, path); }
        d->path = xstrdup(abs);
        d->lang = lang_detect(abs, "", 0);
        d->docmode = A.cfg.doc_default >= 1 && (d->lang == lang_at(0) || !strcmp(lang_name(d->lang), "Markdown"));
        props_load(d);
    } else if (is_pdf_file(path)) {
        size_t n;
        char *txt = pdf_extract_text(path, &n, err, sizeof err);
        if (!txt) {
            free(rp);
            doc_free(d);
            toast(true, "Cannot open PDF: %s", err);
            return;
        }
        doc_set_text(d, txt, n);
        d->path = rp ? rp : xstrdup(path);
        d->readonly = d->is_pdf = true;
        d->mtime_ns = file_mtime_ns(path);
        d->lang = lang_at(0);
        rp = NULL;
    } else if (!doc_load(d, path, err, sizeof err)) {
        free(rp);
        doc_free(d);
        toast(true, "Cannot open %s: %s", path, err);
        return;
    } else {
        free(rp);
        bool prose_lang = d->lang == lang_at(0) || !strcmp(lang_name(d->lang), "Markdown");
        d->docmode = false;
        if (prose_lang && doc_detect_double(d)) {
            doc_collapse_double(d);
            d->docmode = A.cfg.doc_default >= 1;
            d->spacing = 200;
            d->para_sp = 0;
            toast(false, "Double-spaced text detected — showing it with Double line spacing");
        } else if (prose_lang && (A.cfg.doc_default == 2 ||
                                  (A.cfg.doc_default == 1 && (!strcmp(lang_name(d->lang), "Markdown") || looks_prose(d))))) {
            d->docmode = true;
        }
        props_load(d);
        recent_add(d->path);
    }
    if (A.ndocs && doc_empty_untitled(D())) {
        doc_free(A.docs[A.active]);
        A.docs[A.active] = d;
    } else {
        A.active = add_doc(d);
    }
    if (line > 0) { d->cur = d->anc = doc_clamp(d, (Pos){line - 1, 0}); center_cursor(d); }
    redraw();
}

static void really_close(int i);
static void open_browser(int mode);

static void continue_quit(void) {
    for (int i = 0; i < A.ndocs; i++) {
        if (doc_modified(A.docs[i])) {
            activate(i);
            A.mode = M_DIALOG;
            A.dlg_kind = DLG_CLOSE;
            A.dlg_arg = i;
            snprintf(A.dlg_title, sizeof A.dlg_title, "Save changes to “%s”?", doc_name(A.docs[i]));
            snprintf(A.dlg_msg, sizeof A.dlg_msg, "Your changes will be lost if you don't save them.");
            A.dlg_btn[0] = "Save";
            A.dlg_btn[1] = "Don't Save";
            A.dlg_btn[2] = "Cancel";
            A.dlg_nbtn = 3;
            A.dlg_sel = 0;
            redraw();
            return;
        }
    }
    plat_quit();
}

static void close_tab(int i) {
    if (i < 0 || i >= A.ndocs) return;
    if (doc_modified(A.docs[i])) {
        activate(i);
        A.mode = M_DIALOG;
        A.dlg_kind = DLG_CLOSE;
        A.dlg_arg = i;
        snprintf(A.dlg_title, sizeof A.dlg_title, "Save changes to “%s”?", doc_name(A.docs[i]));
        snprintf(A.dlg_msg, sizeof A.dlg_msg, "Your changes will be lost if you don't save them.");
        A.dlg_btn[0] = "Save";
        A.dlg_btn[1] = "Don't Save";
        A.dlg_btn[2] = "Cancel";
        A.dlg_nbtn = 3;
        A.dlg_sel = 0;
        redraw();
        return;
    }
    really_close(i);
}

static void really_close(int i) {
    doc_free(A.docs[i]);
    memmove(A.docs + i, A.docs + i + 1, (A.ndocs - i - 1) * sizeof(Doc *));
    A.ndocs--;
    if (A.fc_doc && A.ndocs >= 0) A.fc_doc = NULL;
    if (A.ndocs == 0) {
        if (A.quitting) { plat_quit(); A.docs[0] = NULL; }
        A.ndocs = 0;
        add_doc(new_doc());
        A.active = 0;
        if (A.quitting) return;
    }
    if (A.active >= A.ndocs) A.active = A.ndocs - 1;
    else if (A.active > i) A.active--;
    A.mode = M_EDIT;
    redraw();
    if (A.quitting) continue_quit();
}

static bool do_save(int i, const char *path) {
    Doc *d = A.docs[i];
    char err[256];
    if (!doc_save(d, path, err, sizeof err)) {
        toast(true, "Save failed: %s", err);
        A.quitting = false;
        d->close_after_save = false;
        return false;
    }
    d->readonly = d->is_pdf = false;
    props_save(d);
    recent_add(d->path);
    toast(false, "Saved %s", doc_name(d));
    A.fc_doc = NULL;
    if (d->close_after_save) { d->close_after_save = false; really_close(i); }
    return true;
}

static void save_index(int i) {
    Doc *d = A.docs[i];
    if (d->readonly) { activate(i); open_browser(BR_SAVE); return; }
    if (d->path) do_save(i, d->path);
    else { activate(i); open_browser(BR_SAVE); }
}

static void reload_doc(Doc *d) {
    if (!d->path) return;
    Pos cur = d->cur;
    int top = d->top_line;
    char err[256];
    if (d->is_pdf) {
        size_t n;
        char *txt = pdf_extract_text(d->path, &n, err, sizeof err);
        if (!txt) { toast(true, "Reload failed: %s", err); return; }
        doc_set_text(d, txt, n);
        d->mtime_ns = file_mtime_ns(d->path);
        d->on_disk_changed = false;
    } else if (!doc_load(d, d->path, err, sizeof err)) { toast(true, "Reload failed: %s", err); return; }
    else if (doc_detect_double(d)) doc_collapse_double(d);
    d->cur = d->anc = doc_clamp(d, cur);
    d->top_line = MIN(top, d->nlines - 1);
    A.fc_doc = NULL;
}

static void check_disk(void) {
    for (int i = 0; i < A.ndocs; i++) {
        Doc *d = A.docs[i];
        if (!d->path || !d->mtime_ns) continue;
        long long m = file_mtime_ns(d->path);
        if (!m || m == d->mtime_ns) continue;
        if (!doc_modified(d)) { reload_doc(d); toast(false, "Reloaded %s (changed on disk)", doc_name(d)); }
        else { d->on_disk_changed = true; d->mtime_ns = m; toast(true, "%s changed on disk", doc_name(d)); }
    }
}

/* ================================================================== file browser */
static int ent_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void br_filter(void) {
    A.br_f = xrealloc(A.br_f, (A.br_n + 1) * sizeof(int));
    A.br_fn = 0;
    const char *f = A.bin.all ? "" : A.bin.s;
    bool path_mode = strchr(f, '/') != NULL || f[0] == '~';
    bool show_hidden = A.br_hidden || f[0] == '.';
    /* prefix matches first, then substring matches */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < A.br_n; i++) {
            const char *nm = A.br_ent[i].name;
            if (nm[0] == '.' && !show_hidden) continue;
            if (path_mode || !*f) { if (pass == 0) A.br_f[A.br_fn++] = i; continue; }
            bool pre = !strncasecmp(nm, f, strlen(f));
            if ((pass == 0 && pre) || (pass == 1 && !pre && strcasestr(nm, f))) A.br_f[A.br_fn++] = i;
        }
    A.br_sel = 0;
    A.br_scroll = 0;
    A.br_nav = false;
}

static void br_load(const char *dir) {
    char rp[PATH_MAX];
    if (!realpath(dir, rp)) { toast(true, "Cannot open folder: %s", strerror(errno)); return; }
    DIR *dh = opendir(rp);
    if (!dh) { toast(true, "Cannot open folder: %s", strerror(errno)); return; }
    for (int i = 0; i < A.br_n; i++) free(A.br_ent[i].name);
    A.br_n = 0;
    snprintf(A.br_dir, sizeof A.br_dir, "%s", rp);
    struct dirent *e;
    while ((e = readdir(dh))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        bool isdir = e->d_type == DT_DIR;
        if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            char p[PATH_MAX];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", rp, e->d_name);
            isdir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (A.br_n == A.br_cap) { A.br_cap = A.br_cap ? A.br_cap * 2 : 128; A.br_ent = xrealloc(A.br_ent, A.br_cap * sizeof(Entry)); }
        A.br_ent[A.br_n++] = (Entry){xstrdup(e->d_name), isdir};
    }
    closedir(dh);
    qsort(A.br_ent, A.br_n, sizeof(Entry), ent_cmp);
    br_filter();
}

static void open_browser(int mode) {
    Doc *d = D();
    A.br_mode = mode;
    A.br_doc = A.active;
    char dir[PATH_MAX];
    if (d->path) {
        snprintf(dir, sizeof dir, "%s", d->path);
        char *s = strrchr(dir, '/');
        if (s && s != dir) *s = 0;
    } else if (!getcwd(dir, sizeof dir)) {
        snprintf(dir, sizeof dir, "%s", getenv("HOME") ? getenv("HOME") : "/");
    }
    A.bin = (Input){0};
    if (mode == BR_SAVE) {
        char nm[256];
        snprintf(nm, sizeof nm, "%s", d->path ? doc_name(d) : d->docmode ? "untitled.md" : "untitled.txt");
        char *dot = strrchr(nm, '.');
        if (d->is_pdf && dot) snprintf(dot, sizeof nm - (dot - nm), ".txt");
        input_set(&A.bin, nm);
    } else if (mode == BR_EXPORT) {
        char nm[256];
        snprintf(nm, sizeof nm, "%s", d->path ? doc_name(d) : "untitled");
        char *dot = strrchr(nm, '.');
        if (dot && dot != nm) *dot = 0;
        strncat(nm, ".pdf", sizeof nm - strlen(nm) - 1);
        input_set(&A.bin, nm);
    }
    if (mode == BR_SAVE && !d->path) {
        /* select only the stem so typing replaces "untitled" but keeps nothing */
        A.bin.all = true;
    }
    br_load(dir);
    A.mode = M_BROWSER;
    redraw();
}

static void do_export(int i, const char *path) {
    char err[256];
    A.mode = M_EDIT;
    Doc *xd = A.docs[i];
    if (xd->docmode ? pdf_export_doc(xd, path, A.cfg.doc_font, A.cfg.font, err, sizeof err)
                    : pdf_export(xd, path, A.cfg.font, A.cfg.pdf_line_numbers, err, sizeof err)) {
        const char *b = strrchr(path, '/');
        toast(false, "Exported %s", b ? b + 1 : path);
    } else toast(true, "PDF export failed: %s", err);
    redraw();
}

static void close_modal(void) {
    if (A.mode == M_BROWSER || A.mode == M_DIALOG) {
        A.quitting = false;
        for (int i = 0; i < A.ndocs; i++) A.docs[i]->close_after_save = false;
    }
    A.mode = A.find_open && A.mode == M_FIND ? M_FIND : M_EDIT;
    redraw();
}

static void ask_overwrite(const char *path) {
    free(A.dlg_path);
    A.dlg_path = xstrdup(path);
    A.mode = M_DIALOG;
    A.dlg_kind = DLG_OVERWRITE;
    A.dlg_arg = A.br_doc;
    const char *b = strrchr(path, '/');
    snprintf(A.dlg_title, sizeof A.dlg_title, "Replace “%s”?", b ? b + 1 : path);
    snprintf(A.dlg_msg, sizeof A.dlg_msg, "It already exists. Replacing it overwrites its contents.");
    A.dlg_btn[0] = "Replace";
    A.dlg_btn[1] = "Cancel";
    A.dlg_nbtn = 2;
    A.dlg_sel = 1;
    redraw();
}

static void br_save_to(const char *path) {
    struct stat st;
    Doc *d = A.docs[A.br_doc];
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) { br_load(path); A.bin = (Input){0}; br_filter(); return; }
        if (A.br_mode == BR_EXPORT || !d->path || strcmp(d->path, path) || d->is_pdf) { ask_overwrite(path); return; }
    }
    if (A.br_mode == BR_EXPORT) { do_export(A.br_doc, path); return; }
    A.mode = M_EDIT;
    do_save(A.br_doc, path);
    redraw();
}

static void br_path_of(const char *name, char *out, size_t n) {
    if (name[0] == '/') snprintf(out, n, "%s", name);
    else if (name[0] == '~') snprintf(out, n, "%s%s", getenv("HOME") ? getenv("HOME") : "", name + 1);
    else snprintf(out, n, "%s/%s", strcmp(A.br_dir, "/") ? A.br_dir : "", name);
}

static void br_accept(void) {
    char path[PATH_MAX];
    const char *t = A.bin.s;
    bool typed = A.bin.len > 0 && !A.bin.all;
    struct stat st;
    if (A.bin.len && (strchr(t, '/') || t[0] == '~')) {
        br_path_of(t, path, sizeof path);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) { br_load(path); A.bin = (Input){0}; br_filter(); return; }
        if (A.br_mode == BR_OPEN) {
            A.mode = M_EDIT;
            open_path(path, 0);
        } else {
            br_save_to(path);
        }
        return;
    }
    if (A.br_mode != BR_OPEN) {
        if (A.br_nav && A.br_fn) {
            Entry *e = &A.br_ent[A.br_f[A.br_sel]];
            br_path_of(e->name, path, sizeof path);
            if (e->dir) { br_load(path); A.bin = (Input){0}; br_filter(); }
            else br_save_to(path);
        } else if (A.bin.len) {
            br_path_of(t, path, sizeof path);
            br_save_to(path);
        }
        (void)typed;
        return;
    }
    if (A.br_fn == 0) return;
    Entry *e = &A.br_ent[A.br_f[A.br_sel]];
    br_path_of(e->name, path, sizeof path);
    if (e->dir) { br_load(path); A.bin = (Input){0}; br_filter(); return; }
    if (A.br_mode == BR_OPEN) { A.mode = M_EDIT; open_path(path, 0); }
    else br_save_to(path);
}

static void br_up(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", A.br_dir);
    char *s = strrchr(p, '/');
    if (!s) return;
    char last[NAME_MAX + 1];
    snprintf(last, sizeof last, "%s", s + 1);
    if (s == p) s[1] = 0; else *s = 0;
    br_load(p);
    for (int i = 0; i < A.br_fn; i++)
        if (!strcmp(A.br_ent[A.br_f[i]].name, last)) { A.br_sel = i; break; }
}


/* ================================================================== session & recent files */
static void state_file(char *out, size_t n, const char *name) {
    const char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/noctedit/%s", x, name);
    else snprintf(out, n, "%s/.local/state/noctedit/%s", h ? h : "/tmp", name);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", out);
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
}

#define MAX_RECENT 25
static char *recent[MAX_RECENT];
static int nrecent;
static bool recent_loaded;

static void recent_load(void) {
    if (recent_loaded) return;
    recent_loaded = true;
    char p[PATH_MAX], line[PATH_MAX];
    state_file(p, sizeof p, "recent");
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (nrecent < MAX_RECENT && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (*line) recent[nrecent++] = xstrdup(line);
    }
    fclose(f);
}

static void recent_add(const char *path) {
    if (!path) return;
    recent_load();
    int at = -1;
    for (int i = 0; i < nrecent; i++) if (!strcmp(recent[i], path)) at = i;
    char *keep = at >= 0 ? recent[at] : xstrdup(path);
    if (at < 0) { if (nrecent == MAX_RECENT) free(recent[--nrecent]); at = nrecent++; }
    memmove(recent + 1, recent, at * sizeof(char *));
    recent[0] = keep;
    char p[PATH_MAX];
    state_file(p, sizeof p, "recent");
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nrecent; i++) fprintf(f, "%s\n", recent[i]);
    fclose(f);
}

static void session_save(void) {
    char p[PATH_MAX];
    state_file(p, sizeof p, "session");
    FILE *f = fopen(p, "w");
    if (!f) return;
    int active = 0, k = 0;
    for (int i = 0; i < A.ndocs; i++) {
        Doc *d = A.docs[i];
        if (!d->path || d->is_pdf) continue;
        if (i == A.active) active = k;
        fprintf(f, "%d %d %d\t%s\n", d->cur.line, d->cur.col, d->top_line, d->path);
        k++;
    }
    fprintf(f, "active %d\n", active);
    fclose(f);
}

static void open_path(const char *path, int line);
static void session_restore(void) {
    char p[PATH_MAX], line[PATH_MAX + 64];
    state_file(p, sizeof p, "session");
    FILE *f = fopen(p, "r");
    if (!f) return;
    int active = 0, k = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "active ", 7)) { active = atoi(line + 7); continue; }
        char *tab = strchr(line, '\t');
        int l, c, top;
        if (!tab || sscanf(line, "%d %d %d", &l, &c, &top) != 3 || access(tab + 1, R_OK)) continue;
        int before = A.ndocs;
        open_path(tab + 1, 0);
        if (A.ndocs == before && !(before == 1 && A.docs[0]->path)) {} /* replaced empty doc */
        Doc *d = D();
        d->cur = d->anc = doc_clamp(d, (Pos){l, c});
        d->top_line = CLAMP(top, 0, d->nlines - 1);
        k++;
    }
    fclose(f);
    A.toast[0] = 0;
    if (k) activate(CLAMP(active, 0, A.ndocs - 1));
}

/* ================================================================== editing commands */
static void cmd_new(void) { A.active = add_doc(new_doc()); A.mode = M_EDIT; redraw(); }
static void cmd_open(void) { open_browser(BR_OPEN); }
static void cmd_save(void) { save_index(A.active); }
static void cmd_save_as(void) { open_browser(BR_SAVE); }
static void cmd_save_all(void) {
    int n = 0, skipped = 0;
    for (int i = 0; i < A.ndocs; i++) {
        if (!doc_modified(A.docs[i])) continue;
        if (A.docs[i]->path) n += do_save(i, A.docs[i]->path);
        else skipped++;
    }
    if (skipped) toast(false, "Saved %d file%s, %d untitled skipped", n, n == 1 ? "" : "s", skipped);
    else toast(false, "Saved %d file%s", n, n == 1 ? "" : "s");
}
static void cmd_close(void) { close_tab(A.active); }
static void cmd_quit(void) {
    session_save();
    A.quitting = true;
    continue_quit();
}
static void cmd_next_tab(void) { activate((A.active + 1) % A.ndocs); }
static void cmd_prev_tab(void) { activate((A.active + A.ndocs - 1) % A.ndocs); }

static void cmd_undo(void) { Doc *d = D(); if (doc_undo(d)) ensure_visible(d); }
static void cmd_redo(void) { Doc *d = D(); if (doc_redo(d)) ensure_visible(d); }

static void cmd_copy(void) {
    Doc *d = D();
    int n;
    char *t;
    if (doc_has_sel(d)) {
        Pos a, b;
        doc_sel(d, &a, &b);
        t = doc_text(d, a, b, &n);
        free(A.clip_line);
        A.clip_line = NULL;
    } else {
        Line *l = &d->lines[d->cur.line];
        n = l->len + 1;
        t = xmalloc(n + 1);
        memcpy(t, l->s, l->len);
        t[l->len] = '\n';
        t[n] = 0;
        free(A.clip_line);
        A.clip_line = xstrdup(t);
    }
    plat_set_clipboard(CLIP_CLIPBOARD, t, n);
    free(t);
}

static void cmd_cut(void) {
    Doc *d = D();
    cmd_copy();
    begin(EK_OTHER);
    if (doc_has_sel(d)) delete_sel(d);
    else {
        int l = d->cur.line;
        if (l + 1 < d->nlines) doc_delete(d, (Pos){l, 0}, (Pos){l + 1, 0});
        else if (l > 0) doc_delete(d, (Pos){l - 1, d->lines[l - 1].len}, (Pos){l, d->lines[l].len});
        else doc_delete(d, (Pos){0, 0}, (Pos){0, d->lines[0].len});
        d->cur = d->anc = doc_clamp(d, (Pos){MIN(l, d->nlines - 1), 0});
    }
    ensure_visible(d);
}

static char *normalize_eol(char *t, int *n) {
    int j = 0, len = (int)strlen(t);
    for (int i = 0; i < len; i++) {
        if (t[i] == '\r') { if (i + 1 < len && t[i + 1] == '\n') continue; t[j++] = '\n'; continue; }
        t[j++] = t[i];
    }
    t[j] = 0;
    *n = j;
    return t;
}

static void paste_text(char *t) {
    if (!t) return;
    int n;
    normalize_eol(t, &n);
    Doc *d = D();
    if (A.clip_line && !doc_has_sel(d) && !strcmp(A.clip_line, t)) {
        /* whole-line copy: paste above the current line, like most code editors */
        begin(EK_OTHER);
        Pos at = {d->cur.line, 0};
        doc_insert(d, at, t, n);
        d->cur.line++;
        d->anc = d->cur;
        ensure_visible(d);
    } else {
        insert_text(t, n, EK_OTHER);
    }
    free(t);
}

static void cmd_paste(void) { paste_text(plat_get_clipboard(CLIP_CLIPBOARD)); }

static void cmd_select_all(void) {
    Doc *d = D();
    d->anc = (Pos){0, 0};
    d->cur = (Pos){d->nlines - 1, d->lines[d->nlines - 1].len};
    set_primary_from_sel();
}

static const char *indent_unit(Doc *d, int *n) {
    static char sp[17];
    if (!d->use_spaces) { *n = 1; return "\t"; }
    *n = MIN(d->tab_w, 16);
    memset(sp, ' ', *n);
    return sp;
}

static void shift_pos_col(Pos *p, int line, int at, int delta) {
    if (p->line == line && p->col >= at) p->col = MAX(at, p->col + delta);
}

static void cmd_indent(void) {
    Doc *d = D();
    int a, b, un;
    sel_lines(d, &a, &b);
    const char *u = indent_unit(d, &un);
    begin(EK_OTHER);
    Pos cur = d->cur, anc = d->anc;
    for (int i = a; i <= b; i++) {
        if (d->lines[i].len == 0 && a != b) continue;
        doc_insert(d, (Pos){i, 0}, u, un);
        if (cur.line == i && (cur.col > 0 || !doc_has_sel(d) || pos_cmp(cur, anc) > 0 || cur.line == anc.line)) cur.col += un;
        if (anc.line == i && (anc.col > 0 || pos_cmp(anc, cur) > 0 || cur.line == anc.line)) anc.col += un;
    }
    d->cur = doc_clamp(d, cur);
    d->anc = doc_clamp(d, anc);
}

static void cmd_outdent(void) {
    Doc *d = D();
    int a, b;
    sel_lines(d, &a, &b);
    begin(EK_OTHER);
    for (int i = a; i <= b; i++) {
        Line *l = &d->lines[i];
        int n = 0;
        if (l->len && l->s[0] == '\t') n = 1;
        else while (n < l->len && n < d->tab_w && l->s[n] == ' ') n++;
        if (!n) continue;
        doc_delete(d, (Pos){i, 0}, (Pos){i, n});
        shift_pos_col(&d->cur, i, 0, -n);
        shift_pos_col(&d->anc, i, 0, -n);
    }
}

static void cmd_toggle_comment(void) {
    Doc *d = D();
    const char *tok = lang_comment(d->lang);
    if (!tok) { toast(true, "No line comments in %s", lang_name(d->lang)); return; }
    int tl = (int)strlen(tok), a, b;
    sel_lines(d, &a, &b);
    bool all = true;
    int minind = INT_MAX;
    for (int i = a; i <= b; i++) {
        Line *l = &d->lines[i];
        int f = first_nonws(l);
        if (f == l->len) continue;
        minind = MIN(minind, f);
        if (l->len - f < tl || memcmp(l->s + f, tok, tl)) all = false;
    }
    if (minind == INT_MAX) return;
    begin(EK_OTHER);
    for (int i = a; i <= b; i++) {
        Line *l = &d->lines[i];
        int f = first_nonws(l);
        if (f == l->len) continue;
        if (all) {
            int n = tl + (f + tl < l->len && l->s[f + tl] == ' ');
            doc_delete(d, (Pos){i, f}, (Pos){i, f + n});
            shift_pos_col(&d->cur, i, f, -n);
            shift_pos_col(&d->anc, i, f, -n);
        } else {
            char buf[16];
            snprintf(buf, sizeof buf, "%s ", tok);
            doc_insert(d, (Pos){i, minind}, buf, tl + 1);
            shift_pos_col(&d->cur, i, minind, tl + 1);
            shift_pos_col(&d->anc, i, minind, tl + 1);
        }
    }
    d->cur = doc_clamp(d, d->cur);
    d->anc = doc_clamp(d, d->anc);
}

static void cmd_duplicate(void) {
    Doc *d = D();
    begin(EK_OTHER);
    if (doc_has_sel(d) && d->cur.line == d->anc.line) {
        Pos a, b;
        doc_sel(d, &a, &b);
        int n;
        char *t = doc_text(d, a, b, &n);
        doc_insert(d, b, t, n);
        free(t);
        d->anc = b;
        d->cur = (Pos){b.line, b.col + n};
        return;
    }
    int a, b;
    sel_lines(d, &a, &b);
    int n;
    char *t = doc_text(d, (Pos){a, 0}, (Pos){b, d->lines[b].len}, &n);
    char *u = xmalloc(n + 2);
    u[0] = '\n';
    memcpy(u + 1, t, n);
    doc_insert(d, (Pos){b, d->lines[b].len}, u, n + 1);
    free(t);
    free(u);
    int k = b - a + 1;
    d->cur.line += k;
    d->anc.line += k;
    ensure_visible(d);
}

static void cmd_delete_line(void) {
    Doc *d = D();
    int a, b;
    sel_lines(d, &a, &b);
    begin(EK_OTHER);
    int col = d->cur.col;
    if (b + 1 < d->nlines) doc_delete(d, (Pos){a, 0}, (Pos){b + 1, 0});
    else if (a > 0) { doc_delete(d, (Pos){a - 1, d->lines[a - 1].len}, (Pos){b, d->lines[b].len}); a--; }
    else doc_delete(d, (Pos){0, 0}, (Pos){b, d->lines[b].len});
    d->cur = d->anc = doc_clamp(d, (Pos){a, col});
    ensure_visible(d);
}

static void move_lines(int dir) {
    Doc *d = D();
    int a, b;
    sel_lines(d, &a, &b);
    if ((dir < 0 && a == 0) || (dir > 0 && b + 1 >= d->nlines)) return;
    begin(EK_OTHER);
    int n1, n2;
    Pos cur = d->cur, anc = d->anc;
    if (dir < 0) {
        char *blk = doc_text(d, (Pos){a, 0}, (Pos){b, d->lines[b].len}, &n1);
        char *prev = doc_text(d, (Pos){a - 1, 0}, (Pos){a - 1, d->lines[a - 1].len}, &n2);
        char *u = xmalloc(n1 + n2 + 2);
        memcpy(u, blk, n1); u[n1] = '\n'; memcpy(u + n1 + 1, prev, n2);
        doc_delete(d, (Pos){a - 1, 0}, (Pos){b, d->lines[b].len});
        doc_insert(d, (Pos){a - 1, 0}, u, n1 + n2 + 1);
        free(blk); free(prev); free(u);
    } else {
        char *blk = doc_text(d, (Pos){a, 0}, (Pos){b, d->lines[b].len}, &n1);
        char *next = doc_text(d, (Pos){b + 1, 0}, (Pos){b + 1, d->lines[b + 1].len}, &n2);
        char *u = xmalloc(n1 + n2 + 2);
        memcpy(u, next, n2); u[n2] = '\n'; memcpy(u + n2 + 1, blk, n1);
        doc_delete(d, (Pos){a, 0}, (Pos){b + 1, d->lines[b + 1].len});
        doc_insert(d, (Pos){a, 0}, u, n1 + n2 + 1);
        free(blk); free(next); free(u);
    }
    cur.line += dir;
    anc.line += dir;
    d->cur = doc_clamp(d, cur);
    d->anc = doc_clamp(d, anc);
    ensure_visible(d);
}
static void cmd_move_up(void) { move_lines(-1); }
static void cmd_move_down(void) { move_lines(1); }

static void cmd_join_lines(void) {
    Doc *d = D();
    int a, b;
    sel_lines(d, &a, &b);
    if (b == a) b = a + 1;
    if (b >= d->nlines) return;
    begin(EK_OTHER);
    for (int k = a; k < b; k++) {
        Line *l = &d->lines[a];
        int e = l->len;
        while (e > 0 && (l->s[e - 1] == ' ' || l->s[e - 1] == '\t')) e--;
        int f = first_nonws(&d->lines[a + 1]);
        doc_delete(d, (Pos){a, e}, (Pos){a + 1, f});
        if (e > 0 && d->lines[a].len > e) doc_insert(d, (Pos){a, e}, " ", 1);
        d->cur = d->anc = (Pos){a, e + (e > 0)};
    }
    d->cur = d->anc = doc_clamp(d, d->cur);
}

static void cmd_select_line(void) {
    Doc *d = D();
    int a, b;
    sel_lines(d, &a, &b);
    if (!doc_has_sel(d)) b = a;
    d->anc = (Pos){a, 0};
    d->cur = b + 1 < d->nlines ? (Pos){b + 1, 0} : (Pos){b, d->lines[b].len};
    set_primary_from_sel();
    ensure_visible(d);
}

static void transform_sel(int how) {
    Doc *d = D();
    if (!doc_has_sel(d)) { Pos a, b; word_at(d, d->cur, &a, &b); d->anc = a; d->cur = b; }
    Pos a, b;
    doc_sel(d, &a, &b);
    int n;
    char *t = doc_text(d, a, b, &n);
    for (int i = 0; i < n; i++) t[i] = how ? tolower((unsigned char)t[i]) : toupper((unsigned char)t[i]);
    begin(EK_OTHER);
    doc_delete(d, a, b);
    Pos e = doc_insert(d, a, t, n);
    free(t);
    d->anc = a;
    d->cur = e;
}
static void cmd_upper(void) { transform_sel(0); }
static void cmd_lower(void) { transform_sel(1); }

static int line_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void cmd_sort(void) {
    Doc *d = D();
    int a, b;
    sel_lines(d, &a, &b);
    if (!doc_has_sel(d)) { a = 0; b = d->nlines - 1; }
    if (b <= a) return;
    int k = b - a + 1;
    char **v = xmalloc(k * sizeof(char *));
    size_t total = 0;
    for (int i = 0; i < k; i++) { v[i] = xstrndup(d->lines[a + i].s, d->lines[a + i].len); total += d->lines[a + i].len + 1; }
    qsort(v, k, sizeof(char *), line_cmp);
    char *u = xmalloc(total + 1), *p = u;
    for (int i = 0; i < k; i++) { size_t l = strlen(v[i]); memcpy(p, v[i], l); p += l; if (i + 1 < k) *p++ = '\n'; free(v[i]); }
    free(v);
    begin(EK_OTHER);
    doc_delete(d, (Pos){a, 0}, (Pos){b, d->lines[b].len});
    doc_insert(d, (Pos){a, 0}, u, (int)(p - u));
    free(u);
    d->anc = (Pos){a, 0};
    d->cur = (Pos){b, d->lines[b].len};
}

static void cmd_trim(void) {
    Doc *d = D();
    int n = 0;
    begin(EK_OTHER);
    for (int i = 0; i < d->nlines; i++) {
        Line *l = &d->lines[i];
        int e = l->len;
        while (e > 0 && (l->s[e - 1] == ' ' || l->s[e - 1] == '\t')) e--;
        if (e < l->len) { doc_delete(d, (Pos){i, e}, (Pos){i, l->len}); n++; }
    }
    d->cur = doc_clamp(d, d->cur);
    d->anc = doc_clamp(d, d->anc);
    toast(false, "Trimmed %d line%s", n, n == 1 ? "" : "s");
}

static void cmd_find(void) {
    Doc *d = D();
    A.find_open = true;
    A.mode = M_FIND;
    A.find_focus = 0;
    if (doc_has_sel(d) && d->cur.line == d->anc.line) {
        Pos a, b;
        doc_sel(d, &a, &b);
        char tmp[1024];
        int n = MIN(b.col - a.col, (int)sizeof tmp - 1);
        memcpy(tmp, d->lines[a.line].s + a.col, n);
        tmp[n] = 0;
        input_set(&A.fin, tmp);
    } else {
        A.fin.all = A.fin.len > 0;
        A.fin.cur = A.fin.len;
    }
    Pos a, b;
    doc_sel(d, &a, &b);
    A.find_origin = a;
    redraw();
}
static void cmd_replace(void) { cmd_find(); A.replace_open = true; if (A.fin.len) { A.find_focus = 1; A.rep.all = true; } }
static void cmd_find_next(void) { if (!A.fin.len) cmd_find(); else find_next(1); }
static void cmd_find_prev(void) { if (!A.fin.len) cmd_find(); else find_next(-1); }
static void cmd_goto(void) { A.gin = (Input){0}; A.mode = M_GOTO; redraw(); }

static void cmd_wrap(void) {
    A.cfg.wrap = !A.cfg.wrap;
    for (int i = 0; i < A.ndocs; i++) { A.docs[i]->top_row = 0; A.docs[i]->scroll_x = 0; }
    ensure_visible(D());
    toast(false, "Word wrap %s", A.cfg.wrap ? "on" : "off");
}
static void cmd_line_numbers(void) { A.cfg.line_numbers = !A.cfg.line_numbers; redraw(); }
static void cmd_whitespace(void) { A.cfg.whitespace = !A.cfg.whitespace; redraw(); }
static void cmd_highlight_line(void) { A.cfg.highlight_line = !A.cfg.highlight_line; redraw(); }
static void zoom_by(int dz) {
    if (dz == 0) A.zoom = 0;
    else A.zoom = CLAMP(A.zoom + dz, 6 - A.cfg.font_size, 72 - A.cfg.font_size);
    load_mono();
    ensure_visible(D());
    redraw();
}
static void cmd_zoom_in(void) { zoom_by(1); }
static void cmd_zoom_out(void) { zoom_by(-1); }
static void cmd_zoom_reset(void) { zoom_by(0); }
static void cmd_eol(void) {
    Doc *d = D();
    d->crlf = !d->crlf;
    d->saved_pos = -1; /* line endings changed: mark dirty */
    toast(false, "Line endings: %s", d->crlf ? "CRLF" : "LF");
}
static void cmd_indent_mode(void) {
    Doc *d = D();
    if (d->use_spaces && d->tab_w == 2) d->tab_w = 4;
    else if (d->use_spaces && d->tab_w == 4) d->tab_w = 8;
    else if (d->use_spaces) { d->use_spaces = false; d->tab_w = 4; }
    else { d->use_spaces = true; d->tab_w = 2; }
    toast(false, "Indent: %s %d", d->use_spaces ? "Spaces" : "Tabs, width", d->tab_w);
}
static void cmd_spaces_to_tabs(void) { D()->use_spaces = !D()->use_spaces; redraw(); }
static void cmd_reload(void) {
    Doc *d = D();
    if (!d->path) return;
    reload_doc(d);
    toast(false, "Reloaded %s", doc_name(d));
}
static void cmd_reload_theme(void) { reload_theme(); toast(false, "Theme: %s", A.pal.source); }
static void cmd_palette(void);
static void cmd_spell_toggle(void);
static void dv_caret_xy(Doc *d, int *x, int *y);
static void cmd_docmode(void);
static void cmd_bold(void);
static void cmd_italic(void);
static void cmd_underline(void);
static void cmd_strike(void);
static void cmd_body(void);
static void cmd_h1(void);
static void cmd_h2(void);
static void cmd_h3(void);
static void cmd_bullets(void);
static void cmd_numbers(void);
static void cmd_quote(void);
static void cmd_al_left(void);
static void cmd_al_center(void);
static void cmd_al_right(void);
static void cmd_al_justify(void);
static void cmd_sp1(void);
static void cmd_sp115(void);
static void cmd_sp15(void);
static void cmd_sp2(void);
static void cmd_indent_first(void);
static void cmd_dbl_storage(void);
static void cmd_next_misspelling(void);
static void cmd_export_pdf(void) { open_browser(BR_EXPORT); }

static const Cmd cmds[] = {
    {"New File", "Ctrl+N", I_FILE_PLUS, cmd_new},
    {"Open File…", "Ctrl+O", I_FOLDER_OPEN, cmd_open},
    {"Save", "Ctrl+S", I_SAVE, cmd_save},
    {"Save As…", "Ctrl+Shift+S", I_SAVE, cmd_save_as},
    {"Save All", "Ctrl+Alt+S", I_SAVE, cmd_save_all},
    {"Close Tab", "Ctrl+W", I_X, cmd_close},
    {"Next Tab", "Ctrl+Tab", I_CHEV_RIGHT, cmd_next_tab},
    {"Previous Tab", "Ctrl+Shift+Tab", I_CHEV_RIGHT, cmd_prev_tab},
    {"Export as PDF…", "Ctrl+Shift+E", I_PDF, cmd_export_pdf},
    {"Reload from Disk", "", I_REFRESH, cmd_reload},
    {"Quit", "Ctrl+Q", I_EXIT, cmd_quit},
    {"Undo", "Ctrl+Z", I_UNDO, cmd_undo},
    {"Redo", "Ctrl+Shift+Z", I_REDO, cmd_redo},
    {"Cut", "Ctrl+X", I_CUT, cmd_cut},
    {"Copy", "Ctrl+C", I_COPY, cmd_copy},
    {"Paste", "Ctrl+V", I_PASTE, cmd_paste},
    {"Select All", "Ctrl+A", I_CHECK, cmd_select_all},
    {"Select Line", "Ctrl+L", I_LINES, cmd_select_line},
    {"Find", "Ctrl+F", I_SEARCH, cmd_find},
    {"Find and Replace", "Ctrl+H", I_REPLACE, cmd_replace},
    {"Find Next", "F3", I_DOWN, cmd_find_next},
    {"Find Previous", "Shift+F3", I_UP, cmd_find_prev},
    {"Go to Line…", "Ctrl+G", I_LINES, cmd_goto},
    {"Toggle Comment", "Ctrl+/", I_CODE, cmd_toggle_comment},
    {"Indent", "Tab", I_INDENT, cmd_indent},
    {"Outdent", "Shift+Tab", I_OUTDENT, cmd_outdent},
    {"Duplicate Line / Selection", "Ctrl+D", I_DUP, cmd_duplicate},
    {"Delete Line", "Ctrl+Shift+K", I_TRASH, cmd_delete_line},
    {"Move Line Up", "Alt+Up", I_UP, cmd_move_up},
    {"Move Line Down", "Alt+Down", I_DOWN, cmd_move_down},
    {"Join Lines", "Ctrl+J", I_ARROWS_V, cmd_join_lines},
    {"Transform to Uppercase", "Ctrl+U", I_UPPER, cmd_upper},
    {"Transform to Lowercase", "Ctrl+Shift+U", I_LOWER, cmd_lower},
    {"Sort Lines", "", I_SORT, cmd_sort},
    {"Trim Trailing Whitespace", "", I_SPACE, cmd_trim},
    {"Toggle Spellcheck", "", I_SPELL, cmd_spell_toggle},
    {"Toggle Document View", "Ctrl+Shift+M", I_FILE_TEXT, cmd_docmode},
    {"Bold", "Ctrl+B", I_BOLD, cmd_bold},
    {"Italic", "Ctrl+I", I_ITALIC, cmd_italic},
    {"Underline", "Ctrl+U", I_UNDERLINE, cmd_underline},
    {"Strikethrough", "", I_STRIKE, cmd_strike},
    {"Paragraph Style: Body Text", "Ctrl+0", I_FILE_TEXT, cmd_body},
    {"Paragraph Style: Heading 1", "Ctrl+1", I_HEADING, cmd_h1},
    {"Paragraph Style: Heading 2", "Ctrl+2", I_HEADING, cmd_h2},
    {"Paragraph Style: Heading 3", "Ctrl+3", I_HEADING, cmd_h3},
    {"Paragraph Style: Quote", "", I_QUOTE, cmd_quote},
    {"Bulleted List", "Shift+F12", I_LIST, cmd_bullets},
    {"Numbered List", "F12", I_LINES, cmd_numbers},
    {"Align Left", "Ctrl+L", I_AL_L, cmd_al_left},
    {"Align Center", "Ctrl+E", I_AL_C, cmd_al_center},
    {"Align Right", "Ctrl+R", I_AL_R, cmd_al_right},
    {"Justify", "Ctrl+J", I_AL_J, cmd_al_justify},
    {"Line Spacing: Single", "", I_LINE_H, cmd_sp1},
    {"Line Spacing: 1.15", "", I_LINE_H, cmd_sp115},
    {"Line Spacing: 1.5", "Ctrl+5", I_LINE_H, cmd_sp15},
    {"Line Spacing: Double", "", I_LINE_H, cmd_sp2},
    {"Toggle First-Line Indent", "", I_INDENT, cmd_indent_first},
    {"Toggle Double-Spaced File Format", "", I_LINE_H, cmd_dbl_storage},
    {"Next Spelling Mistake", "F7", I_ABC, cmd_next_misspelling},
    {"Toggle Word Wrap", "Alt+Z", I_WRAP, cmd_wrap},
    {"Toggle Line Numbers", "", I_LINES, cmd_line_numbers},
    {"Toggle Whitespace Markers", "", I_PILCROW, cmd_whitespace},
    {"Toggle Current Line Highlight", "", I_EYE, cmd_highlight_line},
    {"Zoom In", "Ctrl+=", I_ZOOM_IN, cmd_zoom_in},
    {"Zoom Out", "Ctrl+-", I_ZOOM_OUT, cmd_zoom_out},
    {"Reset Zoom", "Ctrl+0", I_TYPO, cmd_zoom_reset},
    {"Toggle Line Endings (LF/CRLF)", "", I_EXCHANGE, cmd_eol},
    {"Cycle Indentation (2/4/8/Tabs)", "", I_INDENT, cmd_indent_mode},
    {"Toggle Spaces / Tabs", "", I_SPACE, cmd_spaces_to_tabs},
    {"Reload Noctalia Theme", "", I_PALETTE, cmd_reload_theme},
    {"Command Palette", "Ctrl+Shift+P", I_COMMAND, cmd_palette},
};
#define NCMDS ((int)(sizeof cmds / sizeof *cmds))
#define PAL_LANG 1000
#define PAL_TAB 2000
#define PAL_RECENT 3000

static void pal_label(int item, char *out, size_t n) {
    if (item >= PAL_RECENT) {
        const char *r = recent[item - PAL_RECENT], *b = strrchr(r, '/');
        const char *home = getenv("HOME");
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%.*s", (int)(b ? b - r : 0), r);
        if (home && !strncmp(dir, home, strlen(home))) snprintf(out, n, "Open Recent: %s — ~%s", b ? b + 1 : r, dir + strlen(home));
        else snprintf(out, n, "Open Recent: %s — %s", b ? b + 1 : r, dir);
    } else if (item >= PAL_TAB) snprintf(out, n, "Switch to: %s", doc_name(A.docs[item - PAL_TAB]));
    else if (item >= PAL_LANG) snprintf(out, n, "Language: %s", lang_name(lang_at(item - PAL_LANG)));
    else snprintf(out, n, "%s", cmds[item].name);
}

static int fuzzy(const char *hay, const char *nd) {
    if (!*nd) return 1;
    if (strcasestr(hay, nd)) return strncasecmp(hay, nd, strlen(nd)) ? 2 : 3;
    const char *h = hay;
    for (const char *p = nd; *p; p++) {
        if (*p == ' ') continue;
        while (*h && tolower((unsigned char)*h) != tolower((unsigned char)*p)) h++;
        if (!*h) return 0;
        h++;
    }
    return 1;
}

static void pal_build(void) {
    const char *f = A.pin.all ? "" : A.pin.s;
    int cand[1024], score[1024], n = 0;
    char lab[256];
    for (int i = 0; i < NCMDS; i++) cand[n++] = i;
    for (int i = 0; lang_at(i); i++) cand[n++] = PAL_LANG + i;
    for (int i = 0; i < A.ndocs && n < 1024; i++) cand[n++] = PAL_TAB + i;
    recent_load();
    for (int i = 0; i < nrecent && n < 1024; i++) {
        bool open = false;
        for (int k = 0; k < A.ndocs; k++) if (A.docs[k]->path && !strcmp(A.docs[k]->path, recent[i])) open = true;
        if (!open) cand[n++] = PAL_RECENT + i;
    }
    A.pal_n = 0;
    for (int i = 0; i < n; i++) {
        pal_label(cand[i], lab, sizeof lab);
        int s = fuzzy(lab, f);
        if (!*f && cand[i] >= PAL_LANG && cand[i] < PAL_RECENT) s = 0; /* empty filter: commands + recent files */
        if (!*f && cand[i] >= PAL_RECENT) s = 2;                         /* recent files first */
        if (s && A.pal_n < 512) { score[A.pal_n] = s; A.pal_items[A.pal_n++] = cand[i]; }
    }
    /* stable sort by score descending */
    for (int i = 1; i < A.pal_n; i++)
        for (int j = i; j > 0 && score[j] > score[j - 1]; j--) {
            int t = score[j]; score[j] = score[j - 1]; score[j - 1] = t;
            t = A.pal_items[j]; A.pal_items[j] = A.pal_items[j - 1]; A.pal_items[j - 1] = t;
        }
    A.pal_sel = 0;
    A.pal_scroll = 0;
}

static void cmd_palette(void) {
    A.pin = (Input){0};
    A.mode = M_PALETTE;
    pal_build();
    redraw();
}

static void pal_run(int idx) {
    if (idx < 0 || idx >= A.pal_n) return;
    int item = A.pal_items[idx];
    A.mode = M_EDIT;
    if (item >= PAL_RECENT) open_path(recent[item - PAL_RECENT], 0);
    else if (item >= PAL_TAB) activate(item - PAL_TAB);
    else if (item >= PAL_LANG) {
        Doc *d = D();
        d->lang = lang_at(item - PAL_LANG);
        d->hl_valid = 0;
    } else cmds[item].fn();
    redraw();
}

/* ================================================================== drawing helpers */
static bool zrec;

static void zone(Rect r, int act, int arg) {
    if (zrec && A.nz < (int)(sizeof A.zones / sizeof *A.zones)) A.zones[A.nz++] = (Zone){r, act, arg};
}
static Zone *zone_at(double x, double y) {
    for (int i = A.nz - 1; i >= 0; i--) if (rect_hit(A.zones[i].r, (int)x, (int)y)) return &A.zones[i];
    return NULL;
}

/* ================================================================== animation helpers */
static bool anim_on(void) { return A.cfg.animations; }
static int adur(int ms) { return A.cfg.anim_speed > 0 ? (int)(ms / A.cfg.anim_speed) : ms; }
static float ease(float t) { t = 1.f - CLAMP(t, 0.f, 1.f); return 1.f - t * t * t; }
/* progress of an animation started at t0; keeps frames coming while it runs */
static float prog(uint64_t t0, int ms) {
    if (!anim_on() || !t0) return 1.f;
    uint64_t now = A.now ? A.now : plat_now_ms();
    float p = (float)(now - t0) / (float)adur(ms);
    if (p < 1.f) { A.anim_running = true; return p < 0 ? 0 : p; }
    return 1.f;
}
static float hk(int act, int arg) {
    if (act == A_NONE) return 0;
    if (A.hover_act == act && A.hover_arg == arg) return ease(prog(A.hv_t0, 120));
    if (A.phv_act == act && A.phv_arg == arg) return 1.f - ease(prog(A.hv_t0, 120));
    return 0;
}
static float scroll_off_now(void) { return A.so_from * (1.f - ease(prog(A.so_t0, 190))); }

static bool hovered(int act, int arg) { return act != A_NONE && A.hover_act == act && A.hover_arg == arg; }

static int ui_base(Font *f, int y, int h) {
    int asc = font_ascent(f), fh = font_height(f);
    return y + (h - fh) / 2 + asc;
}

static int draw_icon(Canvas *c, int x, int y, int h, uint32_t icon, const char *fallback, Color col) {
    if (A.icons) {
        int w = font_adv(A.icons, icon);
        draw_glyph(c, A.icons, x, ui_base(A.icons, y, h), icon, col);
        return w;
    }
    return draw_text(c, A.ui, x, ui_base(A.ui, y, h), fallback, (int)strlen(fallback), col) - x;
}
static int icon_w(uint32_t icon, const char *fallback) {
    return A.icons ? font_adv(A.icons, icon) : text_width(A.ui, fallback, (int)strlen(fallback));
}

/* draw text truncated with an ellipsis to fit max width */
static int draw_text_fit(Canvas *c, Font *f, int x, int base, const char *s, int maxw, Color col, bool left_ellipsis) {
    int n = (int)strlen(s);
    int w = text_width(f, s, n);
    if (w <= maxw) return draw_text(c, f, x, base, s, n, col);
    int ew = text_width(f, "…", 3);
    if (left_ellipsis) {
        int i = n, acc = 0;
        while (i > 0) {
            int j = utf8_prev(s, i);
            int cw = text_width(f, s + j, i - j);
            if (acc + cw + ew > maxw) break;
            acc += cw;
            i = j;
        }
        x = draw_text(c, f, x, base, "…", 3, col);
        return draw_text(c, f, x, base, s + i, n - i, col);
    }
    int i = 0, acc = 0;
    while (i < n) {
        int j = utf8_next(s, n, i);
        int cw = text_width(f, s + i, j - i);
        if (acc + cw + ew > maxw) break;
        acc += cw;
        i = j;
    }
    x = draw_text(c, f, x, base, s, i, col);
    return draw_text(c, f, x, base, "…", 3, col);
}

static void push_clip(Canvas *c, Rect r, Rect *saved) {
    *saved = cv_get_clip(c);
    cv_clip(c, rect_intersect(*saved, r));
}

/* pill-shaped icon button */
static void icon_button(Canvas *c, Rect r, uint32_t icon, const char *fb, int act, int arg, bool active) {
    float k = hk(act, arg);
    if (active) cv_rrect(c, r.x, r.y, r.w, r.h, r.h / 2, A.c.primary);
    else if (k > 0) cv_rrect(c, r.x, r.y, r.w, r.h, r.h / 2, color_alpha(A.c.hover, (int)(255 * k)));
    int iw = icon_w(icon, fb);
    draw_icon(c, r.x + (r.w - iw) / 2, r.y, r.h, icon, fb, active ? A.c.on_primary : color_mix(A.c.muted, A.c.primary, k));
    zone(r, act, arg);
}

static void text_button(Canvas *c, Rect r, const char *label, int act, int arg, bool filled) {
    float k = hk(act, arg);
    Color bg = filled ? color_mix(A.c.primary, A.c.text, 0.15f * k) : color_mix(A.c.field, A.c.hover, k);
    cv_rrect(c, r.x, r.y, r.w, r.h, r.h / 2, bg);
    int tw = text_width(A.ui, label, (int)strlen(label));
    draw_text(c, A.ui, r.x + (r.w - tw) / 2, ui_base(A.ui, r.y, r.h), label, (int)strlen(label), filled ? A.c.on_primary : A.c.text);
    zone(r, act, arg);
}

static void draw_input(Canvas *c, Rect r, Input *in, bool focused, const char *ph, int act) {
    cv_rrect_border(c, r.x, r.y, r.w, r.h, S(8), focused ? S(1.5) : S(1), focused ? A.c.primary : A.c.border, A.c.field);
    Rect saved, inner = {r.x + S(10), r.y, r.w - S(20), r.h};
    push_clip(c, inner, &saved);
    int base = ui_base(A.ui, r.y, r.h);
    if (!in->len) {
        draw_text(c, A.ui, inner.x, base, ph, (int)strlen(ph), A.c.faint);
        if (focused && A.caret_on) cv_fill(c, inner.x, r.y + S(7), MAX(1, S(1.5)), r.h - S(14), A.c.caret);
    } else {
        int cx = text_width(A.ui, in->s, in->cur);
        int off = MAX(0, cx - inner.w + S(4));
        int tx = inner.x - off;
        if (in->all && focused) cv_rrect(c, tx - S(2), r.y + S(6), text_width(A.ui, in->s, in->len) + S(4), r.h - S(12), S(3), A.c.sel);
        draw_text(c, A.ui, tx, base, in->s, in->len, A.c.text);
        if (focused && A.caret_on && !in->all) cv_fill(c, tx + cx, r.y + S(7), MAX(1, S(1.5)), r.h - S(14), A.c.caret);
    }
    cv_clip(c, saved);
    zone(r, act, 0);
}

static void panel(Canvas *c, Rect r, int radius) {
    for (int i = 3; i >= 1; i--)
        cv_rrect(c, r.x - S(i), r.y - S(i) + S(3), r.w + S(2 * i), r.h + S(2 * i), radius + S(i), color_alpha(0, 0x14));
    cv_rrect_border(c, r.x, r.y, r.w, r.h, radius, MAX(1, S(1)), A.c.border, A.c.panel);
    zone(r, A_PANEL, 0);
}

/* ================================================================== layout & drawing */
static void layout(void) {
    int pad = S(8);
    A.r_top = (Rect){pad, S(6), A.W - 2 * pad, S(34)};
    A.r_status = (Rect){pad, A.H - S(30), A.W - 2 * pad, S(26)};
    int cy = A.r_top.y + A.r_top.h + S(6);
    A.r_card = (Rect){pad, cy, A.W - 2 * pad, A.r_status.y - S(4) - cy};
    Doc *d = A.ndocs ? D() : NULL;
    int gw = S(16);
    if (A.cfg.line_numbers && d) {
        int digits = 1;
        for (int n = d->nlines; n >= 10; n /= 10) digits++;
        digits = MAX(digits, 3);
        gw = digits * font_adv(A.mono, '0') + S(30);
    }
    A.r_gutter = (Rect){A.r_card.x, A.r_card.y + S(8), gw, A.r_card.h - S(16)};
    A.r_text = (Rect){A.r_card.x + gw, A.r_card.y + S(8), A.r_card.w - gw - S(16), A.r_card.h - S(16)};
    A.vis_rows = MAX(1, A.r_text.h / MAX(1, A.lh));
    if (d && d->docmode) {
        load_doc_fonts();
        A.r_tool = (Rect){A.r_card.x + S(10), A.r_card.y + S(8), A.r_card.w - S(20), S(36)};
        int top = A.r_tool.y + A.r_tool.h + S(22);
        int w = MIN(S(A.cfg.page_width), A.r_card.w - S(80));
        A.r_gutter = (Rect){A.r_card.x, top, 0, 0};
        A.r_text = (Rect){A.r_card.x + (A.r_card.w - w) / 2, top, w, A.r_card.y + A.r_card.h - S(14) - top};
        A.vis_rows = MAX(1, A.r_text.h / dv_body_lh(d));
    }
}

static void draw_topbar(Canvas *c) {
    Rect t = A.r_top;
    zone((Rect){0, 0, A.W, t.y + t.h}, A_TITLEBAR, 0);
    int h = t.h, x = t.x;
    icon_button(c, (Rect){x, t.y, h, h}, I_MENU, "≡", A_MENU, 0, A.mode == M_PALETTE);
    x += h + S(6);
    int right = t.x + t.w;
    if (plat_csd()) {
        int bw = h;
        right -= bw;
        icon_button(c, (Rect){right, t.y, bw, h}, I_X, "×", A_WIN_CLOSE, 0, false);
        right -= bw;
        icon_button(c, (Rect){right, t.y, bw, h}, I_SQUARE, "□", A_WIN_MAX, 0, false);
        right -= bw;
        icon_button(c, (Rect){right, t.y, bw, h}, I_MINUS, "–", A_WIN_MIN, 0, false);
        right -= S(8);
    }
    int plus_w = h;
    int avail = right - x - plus_w - S(6);
    int tw[512], total = 0, gap = S(4);
    for (int i = 0; i < A.ndocs && i < 512; i++) {
        const char *nm = doc_name(A.docs[i]);
        tw[i] = CLAMP(text_width(A.ui, nm, (int)strlen(nm)) + icon_w(I_FILE, "") + S(64), S(96), S(260));
        total += tw[i] + gap;
    }
    /* keep the active tab in view */
    int ax = 0;
    for (int i = 0; i < A.active; i++) ax += tw[i] + gap;
    if (total <= avail) A.tabs_scroll = 0;
    else {
        if (ax - A.tabs_scroll < 0) A.tabs_scroll = ax;
        if (ax + tw[A.active] - A.tabs_scroll > avail) A.tabs_scroll = ax + tw[A.active] - avail;
        A.tabs_scroll = CLAMP(A.tabs_scroll, 0, total - avail);
    }
    Rect area = {x, t.y, MIN(avail, total), h}, saved;
    push_clip(c, area, &saved);
    /* the active pill slides between tabs */
    int tx = x - A.tabs_scroll, atx = tx;
    for (int i = 0; i < A.active; i++) atx += tw[i] + gap;
    if (A.active != A.tab_last || A.ndocs != A.tab_ndocs) {
        bool slide = anim_on() && A.ndocs == A.tab_ndocs && A.tab_dw > 0;
        A.tab_fx = slide ? A.tab_dx : atx;
        A.tab_fw = slide ? A.tab_dw : tw[A.active];
        A.tab_t0 = slide ? (A.now ? A.now : plat_now_ms()) : 0;
        A.tab_prev = A.tab_last;
        A.tab_last = A.active;
        A.tab_ndocs = A.ndocs;
    }
    float te = ease(prog(A.tab_t0, 220));
    A.tab_dx = A.tab_fx + (atx - A.tab_fx) * te;
    A.tab_dw = A.tab_fw + (tw[A.active] - A.tab_fw) * te;
    for (int i = 0, bx = tx; i < A.ndocs && i < 512; bx += tw[i] + gap, i++)
        cv_rrect(c, bx, t.y, tw[i], h, S(10), color_mix(A.c.card, A.c.hover, MAX(hk(A_TAB, i), hk(A_TAB_CLOSE, i))));
    cv_rrect(c, (int)lroundf(A.tab_dx), t.y, (int)lroundf(A.tab_dw), h, S(10), A.c.primary);
    for (int i = 0; i < A.ndocs && i < 512; i++) {
        Doc *d = A.docs[i];
        Rect r = {tx, t.y, tw[i], h};
        bool act = i == A.active, hv = hovered(A_TAB, i) || hovered(A_TAB_CLOSE, i);
        float ka = act ? te : (i == A.tab_prev && te < 1.f ? 1.f - te : 0.f);
        Color fg = color_mix(color_mix(A.c.muted, A.c.text, MAX(hk(A_TAB, i), hk(A_TAB_CLOSE, i))), A.c.on_primary, ka);
        Rect zr = rect_intersect(r, area);
        if (zr.w > 0) zone(zr, A_TAB, i);
        int iw = draw_icon(c, r.x + S(12), r.y, h, d->path ? I_FILE_TEXT : I_FILE, "", fg);
        int nx = r.x + S(12) + iw + S(6);
        draw_text_fit(c, A.ui, nx, ui_base(A.ui, r.y, h), doc_name(d), r.x + r.w - S(30) - nx, fg, false);
        Rect cb = {r.x + r.w - S(28), r.y + (h - S(22)) / 2, S(22), S(22)};
        bool chv = hovered(A_TAB_CLOSE, i);
        if (doc_modified(d) && !chv && !hv) {
            int ds = S(8);
            cv_rrect(c, cb.x + (cb.w - ds) / 2, cb.y + (cb.h - ds) / 2, ds, ds, ds / 2, color_mix(A.c.primary, A.c.on_primary, ka));
        } else if (act || hv) {
            if (chv) cv_rrect(c, cb.x, cb.y, cb.w, cb.h, cb.h / 2, act ? color_alpha(A.c.on_primary, 0x30) : A.c.hover);
            int w2 = icon_w(I_X, "×");
            draw_icon(c, cb.x + (cb.w - w2) / 2, cb.y, cb.h, I_X, "×", fg);
        }
        Rect zc = rect_intersect(cb, area);
        if (zc.w > 0) zone(zc, A_TAB_CLOSE, i);
        tx += tw[i] + gap;
    }
    cv_clip(c, saved);
    icon_button(c, (Rect){x + MIN(avail, total) + (total ? S(2) : 0), t.y, plus_w, h}, I_PLUS, "+", A_NEWTAB, 0, false);
}

static uint8_t *g_cls;
static int g_cls_cap;

static uint8_t hl_state(Doc *d, int li) {
    if (d->hl_cap < d->nlines + 1) {
        d->hl_cap = d->nlines + 64;
        d->hl = xrealloc(d->hl, d->hl_cap);
    }
    if (d->hl_valid < 1) { d->hl[0] = 0; d->hl_valid = 1; }
    while (d->hl_valid <= li) {
        int i = d->hl_valid - 1;
        d->hl[i + 1] = hl_line(d->lang, d->lines[i].s, d->lines[i].len, d->hl[i], NULL);
        d->hl_valid++;
    }
    return d->hl[li];
}


/* ================================================================== spellcheck */
static bool spell_on(const Doc *d) { return A.cfg.spellcheck && !d->readonly; }

static bool spell_class_ok(const Doc *d, uint8_t t) {
    const char *ln = lang_name(d->lang);
    if (!strcmp(ln, "Plain Text")) return true;
    if (!strcmp(ln, "Markdown")) return t == T_TEXT || t == T_HEAD || t == T_COMMENT || t == T_CONST;
    return t == T_COMMENT;
}

static bool cp_letter(uint32_t c) {
    return (c < 128 && isalpha((int)c)) || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c <= 0x52F);
}

static int word_end(const char *s, int n, int i) {
    while (i < n) {
        int cl;
        uint32_t c = utf8_dec(s + i, n - i, &cl);
        if (cp_letter(c)) { i += cl; continue; }
        if ((c == '\'' || c == 0x2019) && i + cl < n && isalpha((unsigned char)s[i + cl])) { i += cl; continue; }
        break;
    }
    return i;
}

/* filters out code-ish tokens: paths, identifiers, acronyms, domains */
static bool checkable(const char *s, int n, int ws, int we) {
    if (we - ws < 2 || we - ws > 60) return false;
    if (ws > 0) {
        char p = s[ws - 1];
        if (isdigit((unsigned char)p) || strchr("/\\@#$_%&~=<", p)) return false;
        if (p == '.' && ws > 1 && isalnum((unsigned char)s[ws - 2])) return false;
    }
    if (we < n) {
        char q = s[we];
        if (isdigit((unsigned char)q) || q == '_' || q == '/' || q == '\\' || q == '=') return false;
        if (q == '.' && we + 1 < n && isalnum((unsigned char)s[we + 1])) return false;
        if (q == ':' && we + 2 < n && s[we + 1] == '/' && s[we + 2] == '/') return false;
    }
    for (int i = ws + 1; i < we; i++) if (isupper((unsigned char)s[i])) return false; /* camelCase, ACRONYMS */
    return true;
}

static void cls_for(Doc *d, int li) {
    if (d->docmode) { dv_parse_line(d, li, true); return; }
    Line *l = &d->lines[li];
    if (g_cls_cap < l->len + 1) { g_cls_cap = l->len + 256; g_cls = xrealloc(g_cls, g_cls_cap); }
    hl_line(d->lang, l->s, l->len, hl_state(d, li), g_cls);
}

/* returns 1 misspelled, 0 fine, -1 unavailable; g_cls must hold the line's classes */
static int word_bad(Doc *d, int li, int ws, int we) {
    Line *l = &d->lines[li];
    if (d->docmode ? (g_para.st[ws] & (ST_CODE | ST_LINK | ST_MARK)) != 0 : !spell_class_ok(d, g_cls[ws])) return 0;
    if (!checkable(l->s, l->len, ws, we)) return 0;
    if (li == d->cur.line && d->cur.col == we && d->last_kind == EK_TYPE && d->undo_pos == d->nundo) return 0;
    int r = spell_check(l->s + ws, we - ws);
    if (r < 0) {
        if (!A.spell_warned) { A.spell_warned = true; toast(true, "Spellcheck off: %s", spell_status()); }
        return -1;
    }
    return !r;
}

static void squiggle(Canvas *c, int x0, int x1, int y) {
    int p = MAX(2, S(2)), t = MAX(1, S(1));
    for (int x = x0; x < x1; x++) {
        int ph = (x - x0) % (2 * p);
        int dy = ph < p ? ph : 2 * p - ph;
        cv_fill(c, x, y + dy, 1, t, A.c.error);
    }
}

static void spell_row(Canvas *c, Doc *d, int li, int s0, int s1, int x0, int y) {
    Line *ln = &d->lines[li];
    for (int i = s0; i < s1;) {
        int cl;
        uint32_t cp = utf8_dec(ln->s + i, ln->len - i, &cl);
        if (!cp_letter(cp)) { i += cl; continue; }
        int ws = i, we = word_end(ln->s, ln->len, i);
        i = we;
        int r = word_bad(d, li, ws, we);
        if (r < 0) return;
        if (!r) continue;
        int xa = measure(d, ln->s, s0, ws), xb = measure(d, ln->s, s0, MIN(we, s1));
        squiggle(c, x0 + xa, x0 + xb, y + A.asc + S(2));
    }
}

static bool spell_word_at(Doc *d, Pos p, int *ws, int *we) {
    if (!spell_on(d)) return false;
    Line *l = &d->lines[p.line];
    cls_for(d, p.line);
    for (int i = 0; i < l->len;) {
        int cl;
        uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
        if (!cp_letter(cp)) { i += cl; continue; }
        int a = i, b = word_end(l->s, l->len, i);
        i = b;
        if (p.col >= a && p.col <= b) {
            if (word_bad(d, p.line, a, b) != 1) return false;
            *ws = a;
            *we = b;
            return true;
        }
    }
    return false;
}

static void open_spell_menu(int line, int ws, int we, int x, int y) {
    Doc *d = D();
    A.sp_line = line;
    A.sp_a = ws;
    A.sp_b = we;
    A.sp_n = spell_suggest(d->lines[line].s + ws, we - ws, A.sp_sugg, 8);
    A.sp_sel = 0;
    A.sp_x = x;
    A.sp_y = y;
    d->anc = (Pos){line, ws};
    d->cur = (Pos){line, we};
    A.mode = M_SPELL;
    redraw();
}

static void spell_apply(int i) {
    Doc *d = D();
    A.mode = M_EDIT;
    Line *l = &d->lines[A.sp_line];
    if (A.sp_b > l->len) return;
    if (i < A.sp_n) {
        begin(EK_OTHER);
        doc_delete(d, (Pos){A.sp_line, A.sp_a}, (Pos){A.sp_line, A.sp_b});
        d->cur = d->anc = doc_insert(d, (Pos){A.sp_line, A.sp_a}, A.sp_sugg[i], (int)strlen(A.sp_sugg[i]));
    } else if (i == A.sp_n) {
        spell_add(l->s + A.sp_a, A.sp_b - A.sp_a);
        toast(false, "Added “%.*s” to your dictionary", A.sp_b - A.sp_a, l->s + A.sp_a);
        d->anc = d->cur;
    } else {
        spell_ignore(l->s + A.sp_a, A.sp_b - A.sp_a);
        d->anc = d->cur;
    }
    redraw();
}

static void cmd_spell_toggle(void) {
    A.cfg.spellcheck = !A.cfg.spellcheck;
    A.spell_warned = false;
    if (A.cfg.spellcheck && !spell_ready()) toast(true, "Spellcheck unavailable: %s", spell_status());
    else toast(false, "Spellcheck %s%s%s", A.cfg.spellcheck ? "on (" : "off", A.cfg.spellcheck ? spell_status() : "", A.cfg.spellcheck ? ")" : "");
}

static void cmd_next_misspelling(void) {
    Doc *d = D();
    if (d->readonly) { toast(true, "Spellcheck is off for read-only PDFs"); return; }
    if (!A.cfg.spellcheck) A.cfg.spellcheck = true;
    if (!spell_ready()) { toast(true, "Spellcheck unavailable: %s", spell_status()); return; }
    Pos a, b;
    doc_sel(d, &a, &b);
    int N = d->nlines;
    for (int k = 0; k <= N; k++) {
        int li = (b.line + k) % N;
        Line *l = &d->lines[li];
        cls_for(d, li);
        for (int i = 0; i < l->len;) {
            int cl;
            uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
            if (!cp_letter(cp)) { i += cl; continue; }
            int ws = i, we = word_end(l->s, l->len, i);
            i = we;
            if (k == 0 && ws < b.col) continue;
            if (k == N && ws >= b.col) break;
            if (word_bad(d, li, ws, we) != 1) continue;
            d->anc = (Pos){li, ws};
            d->cur = (Pos){li, we};
            d->want_x = -1;
            ensure_visible(d);
            if (d->docmode) {
                int cx, cy;
                dv_caret_xy(d, &cx, &cy);
                open_spell_menu(li, ws, we, cx, cy);
                return;
            }
            int row, cx = col_to_x(d, li, ws, &row);
            int rows = rows_between(d, d->top_line, d->top_row, li, row, A.vis_rows + 2);
            open_spell_menu(li, ws, we, A.r_text.x - d->scroll_x + cx, A.r_text.y + (rows + 1) * A.lh);
            return;
        }
    }
    toast(false, "No spelling mistakes found");
}

static void draw_list_item(Canvas *c, Rect r, uint32_t icon, const char *label, const char *hint, bool sel, int act, int arg);
static void draw_spell_menu(Canvas *c) {
    int ih = S(32), pad = S(6);
    char add[128], *w = D()->lines[A.sp_line].s + A.sp_a;
    snprintf(add, sizeof add, "Add “%.*s” to dictionary", MIN(A.sp_b - A.sp_a, 60), w);
    int width = text_width(A.ui, add, (int)strlen(add));
    for (int i = 0; i < A.sp_n; i++) width = MAX(width, text_width(A.ui, A.sp_sugg[i], (int)strlen(A.sp_sugg[i])));
    width += S(64);
    int n = MAX(A.sp_n, 1) + 2;
    int h = pad * 2 + n * ih + S(7);
    Rect p = {A.sp_x, A.sp_y + S(4), width, h};
    if (p.x + p.w > A.W - S(8)) p.x = A.W - S(8) - p.w;
    if (p.y + p.h > A.H - S(8)) p.y = A.sp_y - A.lh - h - S(4);
    p.x = MAX(S(8), p.x);
    p.y = MAX(S(8), p.y) + A.pop_dy / 2;
    zone((Rect){0, 0, A.W, A.H}, A_MODAL_BG, 0);
    panel(c, p, S(10));
    int y = p.y + pad;
    if (!A.sp_n) {
        const char *m = "No suggestions";
        draw_text(c, A.ui, p.x + S(18), ui_base(A.ui, y, ih), m, (int)strlen(m), A.c.faint);
        y += ih;
    }
    for (int i = 0; i < A.sp_n; i++, y += ih)
        draw_list_item(c, (Rect){p.x + pad, y, p.w - 2 * pad, ih}, I_ABC, A.sp_sugg[i], i < 9 ? (char[]){(char)('1' + i), 0} : "",
                       A.sp_sel == i, A_SPELL_ITEM, i);
    cv_fill(c, p.x + S(12), y + S(3), p.w - S(24), MAX(1, S(1)), A.c.border);
    y += S(7);
    draw_list_item(c, (Rect){p.x + pad, y, p.w - 2 * pad, ih}, I_BOOK, add, "", A.sp_sel == A.sp_n, A_SPELL_ITEM, A.sp_n);
    y += ih;
    draw_list_item(c, (Rect){p.x + pad, y, p.w - 2 * pad, ih}, I_EYE, "Ignore", "", A.sp_sel == A.sp_n + 1, A_SPELL_ITEM, A.sp_n + 1);
}

static void editor_key(uint32_t sym, uint32_t mods, const char *utf8);
static void app_key_inner(uint32_t sym, uint32_t mods, const char *utf8);
static void draw_list_item(Canvas *c, Rect r, uint32_t icon, const char *label, const char *hint, bool sel, int act, int arg);
static void spell_key(uint32_t sym, uint32_t mods, const char *utf8) {
    int total = A.sp_n + 2;
    switch (sym) {
    case XKB_KEY_Escape: A.mode = M_EDIT; return;
    case XKB_KEY_Up: A.sp_sel = (A.sp_sel + total - 1) % total; return;
    case XKB_KEY_Down: case XKB_KEY_Tab: A.sp_sel = (A.sp_sel + 1) % total; return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: spell_apply(A.sp_sel); return;
    case XKB_KEY_F7: A.mode = M_EDIT; D()->anc = D()->cur; cmd_next_misspelling(); return;
    }
    if (utf8 && utf8[0] >= '1' && utf8[0] <= '9' && utf8[0] - '1' < A.sp_n) { spell_apply(utf8[0] - '1'); return; }
    if (!(mods & MOD_CTRL) && utf8 && (unsigned char)utf8[0] >= 0x20) { A.mode = M_EDIT; editor_key(sym, mods, utf8); }
}

/* ================================================================== document view */
typedef struct { int sb, sa, lh, bh, asc, left, first, height, font; bool code, table, thead, tsep; } PG;

/* ---- Markdown tables: column layout shared by every row of a table block */
#define TMAXC 24
static struct {
    const Doc *d;
    uint64_t ver;
    int px, width, start, end, ncol;
    int colx[TMAXC + 1];
} tcache;
static int cell_s[TMAXC], cell_e[TMAXC], ncell; /* cells of the row last laid out */
#define TMAXL 24
static int cell_nl[TMAXC], cell_brk[TMAXC][TMAXL]; /* wrapped line starts inside each cell */

static bool table_line(const Line *l) {
    int f = 0;
    while (f < l->len && l->s[f] == ' ') f++;
    return f < l->len && l->s[f] == '|';
}
static bool table_sep(const Line *l) {
    bool dash = false;
    for (int i = 0; i < l->len; i++) {
        char ch = l->s[i];
        if (ch == '-') dash = true;
        else if (ch != '|' && ch != ':' && ch != ' ') return false;
    }
    return dash;
}
/* split a row into trimmed cell byte ranges; returns cell count */
static int table_cells(const Line *l, int *cs, int *ce) {
    int n = 0, i = 0;
    while (i < l->len && l->s[i] == ' ') i++;
    if (i < l->len && l->s[i] == '|') i++;
    while (i < l->len && n < TMAXC) {
        int a = i;
        while (i < l->len && !(l->s[i] == '|' && (i == 0 || l->s[i - 1] != '\\'))) i++;
        int b = i;
        if (i >= l->len && a == b) break;
        while (a < b && l->s[a] == ' ') a++;
        while (b > a && l->s[b - 1] == ' ') b--;
        cs[n] = a;
        ce[n] = b;
        n++;
        if (i < l->len) i++;
        if (i >= l->len) break;
    }
    return n;
}

/* the paragraph engine measures in 26.6 fixed point on screen */
static int scr_adv(void *ctx, int f, uint32_t cp) { (void)ctx; return font_adv26(dfont[f], cp); }
static const Measurer scr_meas = {scr_adv, NULL};

static void load_doc_fonts(void) {
    int px = MAX(6, (int)lround((A.cfg.doc_font_size + A.zoom) * A.scale));
    if (dfont[0] && px == dfont_px) return;
    for (int i = 0; i < FN_COUNT; i++) { font_free(dfont[i]); dfont[i] = NULL; }
    static const struct { int id; const char *sfx; double k; } spec[] = {
        {FN_REG, "", 1}, {FN_B, ":weight=bold", 1}, {FN_I, ":slant=italic", 1}, {FN_BI, ":weight=bold:slant=italic", 1},
        {FN_H1, ":weight=bold", 1.75}, {FN_H2, ":weight=bold", 1.4}, {FN_H3, ":weight=bold", 1.18}};
    char pat[200];
    for (size_t i = 0; i < sizeof spec / sizeof *spec; i++) {
        int fpx = (int)lround(px * spec[i].k), id = spec[i].id;
        snprintf(pat, sizeof pat, "%s%s", A.cfg.doc_font, spec[i].sfx);
        dfont[id] = font_open(pat, fpx);
        if (!dfont[id]) { snprintf(pat, sizeof pat, "sans-serif%s", spec[i].sfx); dfont[id] = font_open(pat, fpx); }
        if (!dfont[id]) dfont[id] = font_open(A.cfg.font, fpx);
    }
    dfont[FN_CODE] = font_open(A.cfg.font, (int)lround(px * 0.9));
    if (!dfont[FN_CODE]) dfont[FN_CODE] = font_open("monospace", (int)lround(px * 0.9));
    dfont_px = px;
}

static bool md_fence(Doc *d, int li) {
    if (strcmp(lang_name(d->lang), "Markdown")) return false;
    Line *l = &d->lines[li];
    int f = first_nonws(l);
    if (l->len - f >= 3 && (!memcmp(l->s + f, "```", 3) || !memcmp(l->s + f, "~~~", 3))) return true;
    return hl_state(d, li) == 5; /* inside a fenced block */
}

static bool dv_show(Doc *d, int li) {
    Pos a, b;
    doc_sel(d, &a, &b);
    return li >= a.line && li <= b.line;
}

static void dv_parse_line(Doc *d, int li, bool show) {
    Line *l = &d->lines[li];
    if (g_st_cap < l->len + 1) { g_st_cap = l->len + 256; g_st = xrealloc(g_st, g_st_cap * sizeof(uint16_t)); }
    g_para.st = g_st;
    if (md_fence(d, li)) {
        g_para.kind = BK_BODY;
        g_para.prefix = g_para.level = 0;
        for (int k = 0; k < l->len; k++) g_st[k] = ST_CODE;
        g_para_code = true;
    } else {
        para_parse(l->s, l->len, &g_para, show);
        g_para_code = false;
    }
}


static void table_layout(Doc *d, int li) {
    if (tcache.d == d && tcache.ver == d->version && tcache.px == dfont_px && tcache.width == A.r_text.w && li >= tcache.start && li <= tcache.end) return;
    int a = li, b = li;
    while (a > 0 && li - a < 500 && table_line(&d->lines[a - 1])) a--;
    while (b + 1 < d->nlines && b - li < 500 && table_line(&d->lines[b + 1])) b++;
    int w[TMAXC] = {0}, ncol = 0, cs[TMAXC], ce[TMAXC];
    uint16_t *st = NULL;
    int stcap = 0;
    Para tp = {0};
    for (int k = a; k <= b; k++) {
        Line *l = &d->lines[k];
        if (table_sep(l)) continue;
        if (stcap < l->len + 1) { stcap = l->len + 64; st = xrealloc(st, stcap * sizeof(uint16_t)); }
        tp.st = st;
        para_parse(l->s, l->len, &tp, false);
        bool head = k == a && k + 1 <= b && table_sep(&d->lines[k + 1]);
        int n = table_cells(l, cs, ce);
        ncol = MAX(ncol, n);
        for (int c = 0; c < n; c++) {
            int cw = 0;
            for (int i = cs[c]; i < ce[c];) {
                int cl;
                uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
                if (!(st[i] & ST_HIDE)) cw += font_adv26(dfont[head ? FN_B : para_font(&tp, i)], cp);
                i += cl;
            }
            w[c] = MAX(w[c], (cw + 63) >> 6);
        }
    }
    free(st);
    free(tp.rows);
    int pad = dfont_px, total = 0;
    for (int c = 0; c < ncol; c++) total += w[c] + 2 * pad;
    int avail = A.r_text.w;
    if (total > avail && total > 0) {
        /* too wide: columns narrower than an even share keep their width, wide ones share the rest (and wrap) */
        int inner = avail - 2 * pad * ncol, left = inner, nwide = ncol;
        bool wide[TMAXC];
        for (int c = 0; c < ncol; c++) wide[c] = true;
        for (bool changed = true; changed && nwide;) {
            changed = false;
            int share = left / MAX(1, nwide);
            for (int c = 0; c < ncol; c++)
                if (wide[c] && w[c] <= share) { wide[c] = false; left -= w[c]; nwide--; changed = true; }
        }
        int wsum = 0;
        for (int c = 0; c < ncol; c++) if (wide[c]) wsum += w[c];
        for (int c = 0; c < ncol; c++)
            if (wide[c]) w[c] = MAX(dfont_px * 3, wsum ? (int)((long long)w[c] * left / wsum) : left / MAX(1, nwide));
    }
    tcache.colx[0] = 0;
    for (int c = 0; c < ncol; c++) tcache.colx[c + 1] = tcache.colx[c] + w[c] + 2 * pad;
    tcache = (typeof(tcache)){d, d->version, dfont_px, A.r_text.w, a, b, ncol, {0}};
    tcache.colx[0] = 0;
    for (int c = 0; c < ncol; c++) tcache.colx[c + 1] = tcache.colx[c] + w[c] + 2 * pad;
}

static bool list_line(const Line *l) {
    int i = 0;
    while (i < l->len && l->s[i] == ' ') i++;
    if (i + 1 < l->len && strchr("-*+", l->s[i]) && l->s[i + 1] == ' ') return true;
    int j = i;
    while (j < l->len && isdigit((unsigned char)l->s[j])) j++;
    return j > i && j + 1 < l->len && (l->s[j] == '.' || l->s[j] == ')') && l->s[j + 1] == ' ';
}

static bool fence_marker(const Line *l) {
    int f = first_nonws(l);
    return l->len - f >= 3 && (!memcmp(l->s + f, "```", 3) || !memcmp(l->s + f, "~~~", 3));
}

static void para_geo(Doc *d, int li, PG *g) {
    Line *l = &d->lines[li];
    dv_parse_line(d, li, dv_show(d, li));
    g->code = g_para_code;
    int k = g_para.kind;
    bool head = k >= BK_H1 && k <= BK_H3;
    g->font = g->code ? FN_CODE : k == BK_H1 ? FN_H1 : k == BK_H2 ? FN_H2 : k == BK_H3 ? FN_H3 : FN_REG;
    int bh = font_height(dfont[FN_REG]);
    g->bh = g->code ? bh : font_height(dfont[g->font]);
    g->asc = g->code ? font_ascent(dfont[FN_REG]) : font_ascent(dfont[g->font]);
    double sp = head ? 1.05 : g->code ? 1.0 : d->spacing / 100.0;
    g->lh = MAX(1, (int)lround(g->bh * sp));
    g->sb = li == 0 ? 0 : k == BK_H1 ? bh : k == BK_H2 ? bh * 3 / 4 : k == BK_H3 ? bh / 2 : 0;
    g->sa = head ? bh / 4 : g->code ? 0 : (int)lround(d->para_sp * dfont_px / 12.0);
    g->left = g->first = 0;
    if (k == BK_BULLET || k == BK_NUMBER) g->left = (int)lround(dfont_px * 1.7) * (g_para.level + 1);
    else if (k == BK_QUOTE) g->left = (int)lround(dfont_px * 1.4);
    else if (g->code) g->left = S(12);
    else if (k == BK_BODY && d->indent && l->len) g->first = dfont_px * 2;
    int W = MAX(A.r_text.w - g->left - (g->code ? S(12) : 0), dfont_px * 4);
    para_wrap(l->s, l->len, &g_para, (W - g->first) << 6, W << 6, &scr_meas);
    g->table = g->thead = g->tsep = false;
    if (!g->code && !strcmp(lang_name(d->lang), "Markdown") && table_line(l) && !dv_show(d, li)) {
        table_layout(d, li);
        g->table = true;
        g->tsep = table_sep(l);
        g->thead = !g->tsep && li == tcache.start && li + 1 < d->nlines && table_sep(&d->lines[li + 1]);
        ncell = g->tsep ? 0 : table_cells(l, cell_s, cell_e);
        /* everything outside the cell text (pipes, padding) is hidden */
        for (int q = 0; q < l->len; q++) {
            bool in = false;
            for (int c = 0; c < ncell; c++) if (q >= cell_s[c] && q < cell_e[c]) in = true;
            if (!in) g_st[q] |= ST_HIDE;
            else if (g->thead) g_st[q] |= ST_B;
        }
        g_para.nrows = 1;
        g_para.rows[0] = (DRow){0, l->len, 0, 0};
        g->left = g->first = 0;
        int maxl = 1;
        for (int c = 0; c < ncell && c < tcache.ncol; c++) {
            int inner = (tcache.colx[c + 1] - tcache.colx[c] - 2 * dfont_px) << 6, x = 0, brk = -1, n = 1;
            cell_brk[c][0] = cell_s[c];
            for (int i = cell_s[c]; i < cell_e[c];) {
                int cl;
                uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
                int w = (g_st[i] & ST_HIDE) ? 0 : font_adv26(dfont[para_font(&g_para, i)], cp);
                if (w && x + w > inner && i > cell_brk[c][n - 1] && n < TMAXL) {
                    int b = brk > cell_brk[c][n - 1] ? brk : i;
                    cell_brk[c][n++] = b;
                    brk = -1;
                    x = 0;
                    for (int q = b; q < i;) {
                        int ql;
                        uint32_t qc = utf8_dec(l->s + q, l->len - q, &ql);
                        if (!(g_st[q] & ST_HIDE)) x += font_adv26(dfont[para_font(&g_para, q)], qc);
                        q += ql;
                    }
                    continue;
                }
                x += w;
                if (cp == ' ') brk = i + cl;
                i += cl;
            }
            cell_nl[c] = n;
            maxl = MAX(maxl, n);
        }
        g->lh = g->tsep ? MAX(1, S(2)) : (int)lround(g->bh * (maxl + 0.45));
        g->sb = 0;
        g->sa = 0;
        g->height = g->lh;
        if (li == tcache.start) g->sb = (int)lround(dfont_px * 0.3);
        if (li == tcache.end) g->sa = (int)lround(d->para_sp * dfont_px / 12.0) + dfont_px / 3;
        g->height = g->sb + g->lh + g->sa;
        return;
    }
    if ((k == BK_BULLET || k == BK_NUMBER) && li + 1 < d->nlines && list_line(&d->lines[li + 1]))
        g->sa /= 3; /* items of one list sit closer together */
    if (g->code && fence_marker(l) && !dv_show(d, li)) {
        /* ``` lines become the block's top/bottom padding unless being edited */
        for (int q = 0; q < l->len; q++) g_st[q] |= ST_HIDE;
        para_wrap(l->s, l->len, &g_para, (W - g->first) << 6, W << 6, &scr_meas);
        g->lh = MAX(1, g->bh / 3);
    }
    if (!g->code && k == BK_BODY && first_nonws(l) == l->len && !strcmp(lang_name(d->lang), "Markdown")) {
        /* Markdown needs blank lines between paragraphs; paragraph spacing already separates them */
        g->lh = MAX(1, g->lh * 2 / 5);
        g->sa = 0;
        g->first = 0;
    }
    g->height = g->sb + g_para.nrows * g->lh + g->sa;
}

static int para_h(Doc *d, int li) {
    PG g;
    para_geo(d, li, &g);
    return g.height;
}

static int row_avail(const PG *g, int r) { return A.r_text.w - g->left - (g->code ? S(12) : 0) - (r == 0 ? g->first : 0); }
static int row_x0(const PG *g, int r) { return A.r_text.x + g->left + (r == 0 ? g->first : 0); }

static double row_off(Doc *d, const PG *g, int r, double *jx) {
    DRow *row = &g_para.rows[r];
    double extra = row_avail(g, r) - row->w / 64.0;
    *jx = 0;
    if (g->code || extra <= 0) return 0;
    int al = d->align;
    if (g_para.kind >= BK_H1 && g_para.kind <= BK_H3 && al == AL_JUSTIFY) al = AL_LEFT;
    if (al == AL_CENTER) return extra / 2;
    if (al == AL_RIGHT) return extra;
    if (al == AL_JUSTIFY && r + 1 < g_para.nrows && row->nsp > 0) *jx = extra / row->nsp;
    return 0;
}

static int dv_row_of(int col) {
    int r = 0;
    while (r + 1 < g_para.nrows && g_para.rows[r + 1].s <= col) r++;
    return r;
}

static int cell_line_of(int c, int col) {
    int k = 0;
    while (k + 1 < cell_nl[c] && cell_brk[c][k + 1] <= col) k++;
    return k;
}
static double table_cell_x(Doc *d, int li, int c, int col) {
    Line *l = &d->lines[li];
    double x = A.r_text.x + tcache.colx[c] + dfont_px;
    for (int i = cell_brk[c][cell_line_of(c, col)]; i < col && i < cell_e[c];) {
        int cl;
        uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
        if (!(g_st[i] & ST_HIDE)) x += font_adv26(dfont[para_font(&g_para, i)], cp) / 64.0;
        i += cl;
    }
    return x;
}

static double dv_x_of(Doc *d, int li, const PG *g, int r, int col) {
    if (g->table) {
        for (int c = 0; c < ncell; c++) if (col <= cell_e[c] || c == ncell - 1) return table_cell_x(d, li, c, MIN(col, cell_e[c]));
        return A.r_text.x;
    }
    double jx, off = row_off(d, g, r, &jx);
    Line *l = &d->lines[li];
    return row_x0(g, r) + off + para_row_x(l->s, l->len, &g_para, &g_para.rows[r], col, jx * 64, &scr_meas) / 64.0;
}

static double dv_glyph_w(int i, uint32_t cp) {
    if (g_para.st[i] & ST_HIDE) return 0;
    Font *f = dfont[para_font(&g_para, i)];
    return (cp == '\t' ? 4 * font_adv26(f, ' ') : font_adv26(f, cp)) / 64.0;
}

static int dv_col_at(Doc *d, int li, const PG *g, int r, double x) {
    if (g->table) {
        if (!ncell) return 0;
        int c = 0;
        while (c + 1 < ncell && x >= A.r_text.x + tcache.colx[c + 1]) c++;
        Line *l = &d->lines[li];
        double cx = A.r_text.x + tcache.colx[c] + dfont_px;
        for (int i = cell_s[c]; i < cell_e[c];) {
            int cl;
            uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
            double w = font_adv26(dfont[para_font(&g_para, i)], cp) / 64.0;
            if (x < cx + w / 2) return i;
            cx += w;
            i += cl;
        }
        return cell_e[c];
    }
    Line *l = &d->lines[li];
    DRow *row = &g_para.rows[r];
    double jx, cx = row_x0(g, r) + row_off(d, g, r, &jx);
    for (int i = row->s; i < row->e;) {
        int cl;
        uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
        double w = dv_glyph_w(i, cp);
        if (cp == ' ' && w > 0) w += jx;
        if (w > 0 && x < cx + w / 2) return i;
        cx += w;
        i += cl;
    }
    if (r + 1 < g_para.nrows && row->e > row->s) return utf8_prev(l->s, row->e);
    return row->e;
}

static int dv_body_lh(Doc *d) { return MAX(1, (int)lround(font_height(dfont[FN_REG]) * d->spacing / 100.0)); }

static void dv_clamp(Doc *d) {
    load_doc_fonts();
    if (d->top_line >= d->nlines) { d->top_line = d->nlines - 1; d->top_px = 0; }
    if (d->top_line < 0) d->top_line = 0;
    while (d->top_px < 0 && d->top_line > 0) { d->top_line--; d->top_px += para_h(d, d->top_line); }
    if (d->top_px < 0) d->top_px = 0;
    while (d->top_line < d->nlines - 1) {
        int h = para_h(d, d->top_line);
        if (d->top_px < h) break;
        d->top_px -= h;
        d->top_line++;
    }
    /* stop once the end of the document reaches the upper third of the view */
    int need = A.r_text.h * 2 / 3, acc = -d->top_px;
    for (int li = d->top_line; li < d->nlines && acc < need; li++) acc += para_h(d, li);
    if (acc < need) {
        d->top_px -= need - acc;
        while (d->top_px < 0 && d->top_line > 0) { d->top_line--; d->top_px += para_h(d, d->top_line); }
        if (d->top_px < 0) d->top_px = 0;
    }
    d->top_row = 0;
    d->scroll_x = 0;
}

static void dv_scroll(Doc *d, int px) {
    d->top_px += px;
    dv_clamp(d);
}

static void dv_ensure_visible(Doc *d) {
    if (A.r_text.h <= 0) return;
    load_doc_fonts();
    PG g;
    para_geo(d, d->cur.line, &g);
    int r = dv_row_of(d->cur.col);
    int cy = g.sb + r * g.lh, ch = g.lh;
    int view = A.r_text.h, margin = MIN(g.lh, view / 4);
    if (d->cur.line < d->top_line || (d->cur.line == d->top_line && cy < d->top_px)) {
        d->top_line = d->cur.line;
        d->top_px = cy - margin;
        dv_clamp(d);
        return;
    }
    int y = -d->top_px, li = d->top_line;
    while (li < d->cur.line && y < view + ch) y += para_h(d, li++);
    if (li < d->cur.line || y + cy + ch > view - margin) {
        int remaining = view - margin - (cy + ch);
        li = d->cur.line;
        d->top_line = li;
        d->top_px = remaining < 0 ? -remaining : 0;
        while (remaining > 0 && li > 0) {
            int h = para_h(d, --li);
            d->top_line = li;
            if (h >= remaining) { d->top_px = h - remaining; break; }
            d->top_px = 0;
            remaining -= h;
        }
    }
    dv_clamp(d);
}

static Pos dv_pos_at(double mx, double my) {
    Doc *d = D();
    load_doc_fonts();
    PG g;
    int y = A.r_text.y - d->top_px + (int)lroundf(A.soff), li = d->top_line;
    while (y > my && li > 0) y -= para_h(d, --li);
    for (;; li++) {
        para_geo(d, li, &g);
        if (my < y + g.height || li == d->nlines - 1) break;
        y += g.height;
    }
    int r = (int)floor((my - y - g.sb) / (double)g.lh);
    if (r >= g_para.nrows) {
        if (li == d->nlines - 1 && my >= y + g.height) return (Pos){li, d->lines[li].len};
        r = g_para.nrows - 1;
    }
    if (r < 0) r = 0;
    return (Pos){li, dv_col_at(d, li, &g, r, mx)};
}

static void dv_caret_xy(Doc *d, int *x, int *y) {
    PG g;
    int yy = A.r_text.y - d->top_px;
    for (int li = d->top_line; li < d->cur.line && yy < A.H; li++) yy += para_h(d, li);
    para_geo(d, d->cur.line, &g);
    int r = dv_row_of(d->cur.col);
    *x = (int)dv_x_of(d, d->cur.line, &g, r, d->cur.col);
    *y = yy + g.sb + (r + 1) * g.lh;
}

static void dv_move_vert(Doc *d, int delta, bool shift) {
    if (!shift && doc_has_sel(d) && d->want_x < 0) {
        Pos a, b;
        doc_sel(d, &a, &b);
        d->cur = delta < 0 ? a : b;
    }
    PG g;
    para_geo(d, d->cur.line, &g);
    int r = dv_row_of(d->cur.col);
    if (d->want_x < 0) d->want_x = (int)dv_x_of(d, d->cur.line, &g, r, d->cur.col);
    int li = d->cur.line, moved = 0, step = delta > 0 ? 1 : -1;
    for (int k = 0; k != delta; k += step) {
        if (step > 0) {
            if (r + 1 < g_para.nrows) r++;
            else if (li + 1 < d->nlines) { li++; para_geo(d, li, &g); r = 0; }
            else break;
        } else {
            if (r > 0) r--;
            else if (li > 0) { li--; para_geo(d, li, &g); r = g_para.nrows - 1; }
            else break;
        }
        moved++;
    }
    Pos p;
    if (!moved) p = step < 0 ? (Pos){0, 0} : (Pos){d->nlines - 1, d->lines[d->nlines - 1].len};
    else p = (Pos){li, dv_col_at(d, li, &g, r, d->want_x)};
    int keep = d->want_x;
    d->cur = doc_clamp(d, p);
    if (!shift) d->anc = d->cur;
    dv_ensure_visible(d);
    d->want_x = keep;
}

/* ---- styles at the caret, for the toolbar */
static uint16_t dv_style_at_cursor(Doc *d, int *kind, bool *code) {
    dv_parse_line(d, d->cur.line, true);
    *kind = g_para.kind;
    *code = g_para_code;
    Line *l = &d->lines[d->cur.line];
    Pos a, b;
    doc_sel(d, &a, &b);
    int col = doc_has_sel(d) && a.line == d->cur.line ? a.col : d->cur.col;
    for (int i = col - 1; i >= g_para.prefix && i < l->len; i--) if (!(g_st[i] & ST_MARK)) {
        if (doc_has_sel(d) && i < col) { for (int j = col; j < l->len; j++) if (!(g_st[j] & ST_MARK)) return g_st[j]; }
        return g_st[i];
    }
    for (int i = col; i < l->len; i++) if (!(g_st[i] & ST_MARK)) return g_st[i];
    return 0;
}

/* ---- per-file document settings, kept outside the file */
static void props_path(char *out, size_t n) {
    const char *s = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
    if (s && *s) snprintf(out, n, "%s/noctedit/docprops", s);
    else snprintf(out, n, "%s/.local/state/noctedit/docprops", h ? h : "/tmp");
}

static bool props_load(Doc *d) {
    if (!d->path) return false;
    char p[PATH_MAX];
    props_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    char line[PATH_MAX + 64];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || strcmp(tab + 1, d->path)) continue;
        int dm, al, sp, ps, in;
        if (sscanf(line, "%d %d %d %d %d", &dm, &al, &sp, &ps, &in) == 5) {
            d->docmode = dm;
            d->align = (uint8_t)CLAMP(al, 0, 3);
            d->spacing = (uint16_t)CLAMP(sp, 100, 300);
            d->para_sp = (uint8_t)CLAMP(ps, 0, 48);
            d->indent = in != 0;
            found = true;
        }
    }
    fclose(f);
    return found;
}

static void props_save(Doc *d) {
    if (!d->path) return;
    char p[PATH_MAX];
    props_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    char **keep = NULL;
    int nk = 0;
    if (f) {
        char line[PATH_MAX + 64];
        while (fgets(line, sizeof line, f)) {
            char *tab = strchr(line, '\t');
            line[strcspn(line, "\n")] = 0;
            if (!tab || !strcmp(tab + 1, d->path)) continue;
            keep = xrealloc(keep, (nk + 1) * sizeof(char *));
            keep[nk++] = xstrdup(line);
        }
        fclose(f);
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", p);
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    f = fopen(p, "w");
    if (f) {
        for (int i = MAX(0, nk - 2000); i < nk; i++) fprintf(f, "%s\n", keep[i]);
        fprintf(f, "%d %d %d %d %d\t%s\n", d->docmode, d->align, d->spacing, d->para_sp, d->indent, d->path);
        fclose(f);
    }
    for (int i = 0; i < nk; i++) free(keep[i]);
    free(keep);
}

static bool looks_prose(Doc *d) {
    int prose = 0, total = 0;
    for (int i = 0; i < d->nlines && total < 300; i++) {
        Line *l = &d->lines[i];
        if (l->len < 2) continue;
        total++;
        int spaces = 0, code = 0;
        for (int k = 0; k < l->len; k++) {
            spaces += l->s[k] == ' ';
            code += strchr("{};=<>|\\$", l->s[k]) != NULL && l->s[k];
        }
        if (spaces >= 3 && code * 20 < l->len) prose++;
    }
    return total > 0 && prose * 10 >= total * 6;
}

/* ---- formatting commands */
static void props_changed(void) { props_save(D()); redraw(); }

static void set_docmode(Doc *d, bool on) {
    d->docmode = on;
    d->top_px = 0;
    d->top_row = 0;
    d->want_x = -1;
    if (on) load_doc_fonts();
    props_save(d);
    ensure_visible(d);
}
static void cmd_docmode(void) {
    Doc *d = D();
    set_docmode(d, !d->docmode);
    toast(false, "%s view", d->docmode ? "Document" : "Code");
}

static void need_doc(void) { if (!D()->docmode) set_docmode(D(), true); }

static void toggle_inline_seg(Doc *d, int li, int s, int e, const char *open, const char *close, Pos *na, Pos *nb) {
    Line *l = &d->lines[li];
    int ol = (int)strlen(open), cl = (int)strlen(close);
    while (s < e && l->s[s] == ' ') s++;
    while (e > s && l->s[e - 1] == ' ') e--;
    bool single_star = ol == 1 && open[0] == '*';
    bool outside = s >= ol && e + cl <= l->len && !memcmp(l->s + s - ol, open, ol) && !memcmp(l->s + e, close, cl);
    if (outside && single_star && s >= 2 && l->s[s - 2] == '*' && !(s >= 3 && l->s[s - 3] == '*')) outside = false;
    bool inside = !outside && e - s >= ol + cl && !memcmp(l->s + s, open, ol) && !memcmp(l->s + e - cl, close, cl);
    if (inside && single_star && e - s >= 4 && l->s[s + 1] == '*' && l->s[s + 2] != '*') inside = false;
    if (outside) {
        doc_delete(d, (Pos){li, e}, (Pos){li, e + cl});
        doc_delete(d, (Pos){li, s - ol}, (Pos){li, s});
        *na = (Pos){li, s - ol};
        *nb = (Pos){li, e - ol};
    } else if (inside) {
        doc_delete(d, (Pos){li, e - cl}, (Pos){li, e});
        doc_delete(d, (Pos){li, s}, (Pos){li, s + ol});
        *na = (Pos){li, s};
        *nb = (Pos){li, e - ol - cl};
    } else {
        doc_insert(d, (Pos){li, e}, close, cl);
        doc_insert(d, (Pos){li, s}, open, ol);
        *na = (Pos){li, s + ol};
        *nb = (Pos){li, e + ol};
    }
}

static void toggle_inline(const char *open, const char *close) {
    Doc *d = D();
    if (d->readonly) { begin(EK_OTHER); return; }
    need_doc();
    Pos a, b;
    if (doc_has_sel(d)) doc_sel(d, &a, &b);
    else {
        word_at(d, d->cur, &a, &b);
        Line *l = &d->lines[d->cur.line];
        bool word = b.col > a.col && cclass((unsigned char)l->s[a.col]) == 1;
        if (!word) {
            begin(EK_OTHER);
            int ol = (int)strlen(open), cl = (int)strlen(close);
            doc_insert(d, d->cur, close, cl);
            doc_insert(d, d->cur, open, ol);
            d->cur.col += ol;
            d->anc = d->cur;
            return;
        }
    }
    begin(EK_OTHER);
    Pos na = a, nb = b, ta, tb;
    for (int li = b.line; li >= a.line; li--) {
        int s = li == a.line ? a.col : 0, e = li == b.line ? b.col : d->lines[li].len;
        dv_parse_line(d, li, true);
        s = MAX(s, g_para.prefix);
        if (e <= s) continue;
        toggle_inline_seg(d, li, s, e, open, close, &ta, &tb);
        if (li == b.line) nb = tb;
        if (li == a.line) na = ta;
    }
    if (pos_cmp(d->cur, d->anc) >= 0) { d->anc = na; d->cur = nb; }
    else { d->anc = nb; d->cur = na; }
    d->anc = doc_clamp(d, d->anc);
    d->cur = doc_clamp(d, d->cur);
    d->want_x = -1;
}

static void cmd_bold(void) { toggle_inline("**", "**"); }
static void cmd_italic(void) { toggle_inline("*", "*"); }
static void cmd_underline(void) { toggle_inline("<u>", "</u>"); }
static void cmd_strike(void) { toggle_inline("~~", "~~"); }

static void set_block(int kind) {
    Doc *d = D();
    if (d->readonly) { begin(EK_OTHER); return; }
    need_doc();
    int a, b;
    sel_lines(d, &a, &b);
    dv_parse_line(d, d->cur.line, true);
    bool toggle_off = g_para.kind == kind && kind != BK_BODY;
    begin(EK_OTHER);
    int num = 1;
    for (int li = a; li <= b; li++) {
        Line *l = &d->lines[li];
        if (!l->len && a != b) continue;
        dv_parse_line(d, li, true);
        if (g_para_code) continue;
        int old = g_para.kind == BK_BODY ? 0 : g_para.prefix;
        char pre[24] = "";
        int target = toggle_off ? BK_BODY : kind;
        switch (target) {
        case BK_H1: strcpy(pre, "# "); break;
        case BK_H2: strcpy(pre, "## "); break;
        case BK_H3: strcpy(pre, "### "); break;
        case BK_BULLET: strcpy(pre, "- "); break;
        case BK_NUMBER: snprintf(pre, sizeof pre, "%d. ", num++); break;
        case BK_QUOTE: strcpy(pre, "> "); break;
        }
        int nl = (int)strlen(pre);
        if (old) doc_delete(d, (Pos){li, 0}, (Pos){li, old});
        if (nl) doc_insert(d, (Pos){li, 0}, pre, nl);
        int delta = nl - old;
        if (d->cur.line == li) d->cur.col = MAX(nl, d->cur.col + delta);
        if (d->anc.line == li) d->anc.col = MAX(nl, d->anc.col + delta);
    }
    d->cur = doc_clamp(d, d->cur);
    d->anc = doc_clamp(d, d->anc);
    d->want_x = -1;
}
static void cmd_body(void) { set_block(BK_BODY); }
static void cmd_h1(void) { set_block(BK_H1); }
static void cmd_h2(void) { set_block(BK_H2); }
static void cmd_h3(void) { set_block(BK_H3); }
static void cmd_bullets(void) { set_block(BK_BULLET); }
static void cmd_numbers(void) { set_block(BK_NUMBER); }
static void cmd_quote(void) { set_block(BK_QUOTE); }

static void set_align(int al) { need_doc(); D()->align = (uint8_t)al; props_changed(); }
static void cmd_al_left(void) { set_align(AL_LEFT); }
static void cmd_al_center(void) { set_align(AL_CENTER); }
static void cmd_al_right(void) { set_align(AL_RIGHT); }
static void cmd_al_justify(void) { set_align(AL_JUSTIFY); }
static void set_spacing(int sp) { need_doc(); D()->spacing = (uint16_t)sp; ensure_visible(D()); props_changed(); }
static void cmd_sp1(void) { set_spacing(100); }
static void cmd_sp115(void) { set_spacing(115); }
static void cmd_sp15(void) { set_spacing(150); }
static void cmd_sp2(void) { set_spacing(200); }
static void cmd_indent_first(void) { need_doc(); D()->indent = !D()->indent; props_changed(); }
static void cmd_dbl_storage(void) {
    Doc *d = D();
    d->dbl = !d->dbl;
    d->dbl_tail = true;
    d->saved_pos = -1;
    toast(false, d->dbl ? "Will save with a blank line after every line (double-spaced text)" : "Will save as normal single-spaced text");
}

/* Enter inside lists/quotes continues them; on an empty item it ends the list */
static bool dv_enter(void) {
    Doc *d = D();
    if (doc_has_sel(d)) return false;
    dv_parse_line(d, d->cur.line, true);
    Line *l = &d->lines[d->cur.line];
    int k = g_para.kind;
    if (g_para_code || (k != BK_BULLET && k != BK_NUMBER && k != BK_QUOTE && !(k >= BK_H1 && k <= BK_H3))) return false;
    if (d->cur.col < g_para.prefix) return false;
    begin(EK_OTHER);
    if (k >= BK_H1 && k <= BK_H3) {
        d->cur = d->anc = doc_insert(d, d->cur, "\n", 1);
        dv_ensure_visible(d);
        return true;
    }
    bool empty = true;
    for (int i = g_para.prefix; i < l->len; i++) if (l->s[i] != ' ') empty = false;
    if (empty) {
        doc_delete(d, (Pos){d->cur.line, 0}, (Pos){d->cur.line, l->len});
        d->cur = d->anc = (Pos){d->cur.line, 0};
        return true;
    }
    char pre[48];
    int lead = 0;
    while (lead < l->len && l->s[lead] == ' ' && lead < 20) lead++;
    int n = 0;
    pre[n++] = '\n';
    memset(pre + n, ' ', lead);
    n += lead;
    if (k == BK_BULLET) n += snprintf(pre + n, sizeof pre - n, "%c ", l->s[lead]);
    else if (k == BK_NUMBER) n += snprintf(pre + n, sizeof pre - n, "%d. ", g_para.number + 1);
    else n += snprintf(pre + n, sizeof pre - n, "> ");
    d->cur = d->anc = doc_insert(d, d->cur, pre, n);
    dv_ensure_visible(d);
    return true;
}

/* ---- popup menus (style, spacing) */
static void open_menu(int kind, int x, int y) {
    Doc *d = D();
    A.menu_kind = kind;
    A.menu_n = 0;
    A.menu_x = x;
    A.menu_y = y;
#define ITEM(lab, val, chk) do { snprintf(A.menu_lab[A.menu_n], sizeof A.menu_lab[0], "%s", lab); A.menu_val[A.menu_n] = val; A.menu_chk[A.menu_n++] = chk; } while (0)
    if (kind == 0) {
        int k;
        bool code;
        dv_style_at_cursor(d, &k, &code);
        ITEM("Body Text", BK_BODY, k == BK_BODY && !code);
        ITEM("Heading 1", BK_H1, k == BK_H1);
        ITEM("Heading 2", BK_H2, k == BK_H2);
        ITEM("Heading 3", BK_H3, k == BK_H3);
        ITEM("Quote", BK_QUOTE, k == BK_QUOTE);
        ITEM("Bulleted List", BK_BULLET, k == BK_BULLET);
        ITEM("Numbered List", BK_NUMBER, k == BK_NUMBER);
    } else {
        ITEM("Line spacing: Single", 100, d->spacing == 100);
        ITEM("Line spacing: 1.15", 115, d->spacing == 115);
        ITEM("Line spacing: 1.5", 150, d->spacing == 150);
        ITEM("Line spacing: Double", 200, d->spacing == 200);
        ITEM("Paragraph spacing: None", 1000, d->para_sp == 0);
        ITEM("Paragraph spacing: Small", 1006, d->para_sp == 6);
        ITEM("Paragraph spacing: Large", 1012, d->para_sp == 12);
        ITEM("First-line indent", 2000, d->indent);
        ITEM("Save as double-spaced text", 3000, d->dbl);
    }
#undef ITEM
    A.menu_sel = 0;
    for (int i = 0; i < A.menu_n; i++) if (A.menu_chk[i]) { A.menu_sel = i; break; }
    A.mode = M_MENU;
    redraw();
}

static void menu_apply(int i) {
    if (i < 0 || i >= A.menu_n) return;
    A.mode = M_EDIT;
    int v = A.menu_val[i];
    Doc *d = D();
    if (A.menu_kind == 0) set_block(v);
    else if (v < 1000) set_spacing(v);
    else if (v < 2000) { d->para_sp = (uint8_t)(v - 1000); props_changed(); }
    else if (v == 2000) cmd_indent_first();
    else cmd_dbl_storage();
    redraw();
}

static void draw_menu(Canvas *c) {
    int ih = S(32), pad = S(6), w = 0;
    for (int i = 0; i < A.menu_n; i++) w = MAX(w, text_width(A.ui, A.menu_lab[i], (int)strlen(A.menu_lab[i])));
    w += S(70);
    int h = pad * 2 + A.menu_n * ih;
    Rect p = {A.menu_x, A.menu_y, w, h};
    if (p.x + p.w > A.W - S(8)) p.x = A.W - S(8) - p.w;
    if (p.y + p.h > A.H - S(8)) p.y = A.H - S(8) - p.h;
    p.x = MAX(S(8), p.x);
    p.y = MAX(S(8), p.y) + A.pop_dy / 2;
    zone((Rect){0, 0, A.W, A.H}, A_MODAL_BG, 0);
    panel(c, p, S(10));
    for (int i = 0; i < A.menu_n; i++)
        draw_list_item(c, (Rect){p.x + pad, p.y + pad + i * ih, p.w - 2 * pad, ih}, A.menu_chk[i] ? I_CHECK : 1,
                       A.menu_lab[i], "", A.menu_sel == i, A_MENU_ITEM, i);
}

static void menu_key(uint32_t sym, uint32_t mods, const char *utf8) {
    (void)mods; (void)utf8;
    switch (sym) {
    case XKB_KEY_Escape: A.mode = M_EDIT; return;
    case XKB_KEY_Up: A.menu_sel = (A.menu_sel + A.menu_n - 1) % A.menu_n; return;
    case XKB_KEY_Down: case XKB_KEY_Tab: A.menu_sel = (A.menu_sel + 1) % A.menu_n; return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: menu_apply(A.menu_sel); return;
    }
}

/* ---- drawing */
static Color dv_color(const PG *g, uint16_t st) {
    if (st & ST_MARK) return A.c.faint;
    if (st & ST_CODE) return A.c.syn[T_STR];
    if (st & ST_LINK) return A.c.primary;
    int k = g_para.kind;
    if (k >= BK_H1 && k <= BK_H3) return A.c.primary;
    if (k == BK_QUOTE) return A.c.muted;
    (void)g;
    return A.c.text;
}

static void dv_spell_row(Canvas *c, Doc *d, int li, const PG *g, int r, int base) {
    Line *l = &d->lines[li];
    DRow *row = &g_para.rows[r];
    for (int i = row->s; i < row->e;) {
        int cl;
        uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
        if (!cp_letter(cp) || (g_para.st[i] & (ST_CODE | ST_LINK | ST_MARK))) { i += cl; continue; }
        int ws = i, we = word_end(l->s, l->len, i);
        i = we;
        int bad = word_bad(d, li, ws, we);
        if (bad < 0) return;
        if (!bad) continue;
        double xa = dv_x_of(d, li, g, r, ws), xb = dv_x_of(d, li, g, r, MIN(we, row->e));
        squiggle(c, (int)xa, (int)xb, base + S(3));
    }
}

static void draw_table_row(Canvas *c, Doc *d, int li, const PG *g, int y) {
    Line *l = &d->lines[li];
    int y0 = y + g->sb, x0 = A.r_text.x, tw = tcache.colx[tcache.ncol];
    int bw = MAX(1, S(1));
    Color line = color_alpha(A.c.border, 0xc0);
    if (g->tsep) { cv_fill(c, x0, y0, tw, MAX(1, g->lh / 2), line); return; }
    if (g->thead) cv_rrect(c, x0, y0, tw, g->lh, S(6), A.c.field);
    if (li == tcache.start && !g->thead) cv_fill(c, x0, y0, tw, bw, line);
    if (li != tcache.end && !(li + 1 <= tcache.end && table_sep(&d->lines[li + 1]))) cv_fill(c, x0, y0 + g->lh - bw, tw, bw, color_alpha(A.c.border, 0x70));
    if (li == tcache.end) cv_fill(c, x0, y0 + g->lh - bw, tw, bw, line);
    for (int k = 1; k < tcache.ncol; k++) cv_fill(c, x0 + tcache.colx[k], y0 + S(4), bw, g->lh - S(8), color_alpha(A.c.border, 0x70));
    int base0 = y0 + (int)lround(g->bh * 0.225) + g->asc;
    for (int k = 0; k < ncell && k < tcache.ncol; k++) {
        Rect cell = {x0 + tcache.colx[k] + dfont_px / 2, y0, tcache.colx[k + 1] - tcache.colx[k] - dfont_px, g->lh}, saved;
        push_clip(c, cell, &saved);
        if (A.find_open && A.fin.len)
            for (int j = find_in(l->s, l->len, cell_s[k], A.fin.s, A.fin.len, A.find_case, A.find_word); j >= 0 && j < cell_e[k];
                 j = find_in(l->s, l->len, j + A.fin.len, A.fin.s, A.fin.len, A.find_case, A.find_word)) {
                int base = base0 + cell_line_of(k, j) * g->bh;
                double xa = table_cell_x(d, li, k, j), xb = table_cell_x(d, li, k, MIN(j + A.fin.len, cell_e[k]));
                if (cell_line_of(k, MIN(j + A.fin.len, cell_e[k])) != cell_line_of(k, j)) xb = cell.x + cell.w;
                cv_rrect(c, (int)xa - S(1), base - g->asc, (int)(xb - xa) + S(2), g->bh, S(3), A.c.match);
            }
        double x = table_cell_x(d, li, k, cell_s[k]);
        int ln = 0, base = base0;
        for (int i = cell_s[k]; i < cell_e[k];) {
            if (ln + 1 < cell_nl[k] && i >= cell_brk[k][ln + 1]) {
                ln++;
                base = base0 + ln * g->bh;
                x = table_cell_x(d, li, k, i);
            }
            int cl;
            uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
            uint16_t st = g_st[i];
            if (st & ST_HIDE) { i += cl; continue; }
            int f = para_font(&g_para, i);
            double w = font_adv26(dfont[f], cp) / 64.0;
            Color col = (st & ST_CODE) ? A.c.syn[T_STR] : (st & ST_LINK) ? A.c.primary : g->thead ? A.c.primary : A.c.text;
            if (cp != ' ') draw_glyph26(c, dfont[f], (int)lround(x * 64), base, cp, col);
            if (st & (ST_U | ST_LINK)) cv_fill(c, (int)x, base + MAX(1, S(2)), (int)ceil(w), MAX(1, S(1)), col);
            x += w;
            i += cl;
        }
        cv_clip(c, saved);
    }
}

static void draw_para(Canvas *c, Doc *d, int li, const PG *g, int y) {
    if (g->table) { draw_table_row(c, d, li, g, y); return; }
    Line *l = &d->lines[li];
    Para *p = &g_para;
    Pos sa, sb;
    doc_sel(d, &sa, &sb);
    bool has_sel = doc_has_sel(d);
    int y0 = y + g->sb, k = p->kind;
    Rect clip = cv_get_clip(c);
    int fill_h = p->nrows * g->lh;
    if (k == BK_RULE && !(p->nrows && l->len && !(p->st[0] & ST_HIDE)))
        cv_fill(c, A.r_text.x, y0 + g->lh / 2, A.r_text.w, MAX(1, S(1)), A.c.border);
    if (k == BK_QUOTE) cv_rrect(c, A.r_text.x + S(2), y0 + S(3), MAX(2, S(4)), fill_h - S(6), S(2), color_alpha(A.c.primary, 0x90));
    if (g->code) cv_fill(c, A.r_text.x, y0, A.r_text.w, fill_h, A.c.field);
    if ((k == BK_BULLET || k == BK_NUMBER) && p->prefix > 0 && (p->st[p->prefix - 1] & ST_HIDE)) {
        char m[16];
        int mn;
        if (k == BK_BULLET) { static const char *bl[] = {"•", "◦", "▪"}; mn = snprintf(m, sizeof m, "%s", bl[p->level % 3]); }
        else mn = snprintf(m, sizeof m, "%d.", p->number);
        int mw = text_width(dfont[FN_REG], m, mn);
        draw_text(c, dfont[FN_REG], row_x0(g, 0) - mw - dfont_px / 2, y0 + (g->lh - g->bh) / 2 + g->asc, m, mn, A.c.primary);
    }
    for (int r = 0; r < p->nrows; r++) {
        int ry = y0 + r * g->lh;
        if (ry >= clip.y + clip.h) break;
        if (ry + g->lh <= clip.y) continue;
        DRow *row = &p->rows[r];
        double jx, off = row_off(d, g, r, &jx);
        double x0 = row_x0(g, r) + off;
        int base = ry + (g->lh - g->bh) / 2 + g->asc;
        int top = base - g->asc, th = g->bh;
        if (has_sel && li >= sa.line && li <= sb.line) {
            int c0 = li == sa.line ? sa.col : 0, c1 = li == sb.line ? sb.col : l->len;
            int a0 = MAX(c0, row->s), a1 = MIN(c1, row->e);
            bool eol = li < sb.line && r == p->nrows - 1;
            if (a0 < a1 || (eol && c0 <= row->e)) {
                double xa = dv_x_of(d, li, g, r, a0 < a1 ? a0 : row->e);
                double xb = a0 < a1 ? dv_x_of(d, li, g, r, a1) : xa;
                if (eol) xb += dfont_px / 3;
                cv_fill(c, (int)xa, top, (int)(xb - xa + .5), th, A.c.sel);
            }
        }
        if (A.find_open && A.fin.len)
            for (int j = find_in(l->s, l->len, MAX(0, row->s - A.fin.len + 1), A.fin.s, A.fin.len, A.find_case, A.find_word);
                 j >= 0 && j < row->e; j = find_in(l->s, l->len, j + A.fin.len, A.fin.s, A.fin.len, A.find_case, A.find_word)) {
                double xa = dv_x_of(d, li, g, r, MAX(j, row->s)), xb = dv_x_of(d, li, g, r, MIN(j + A.fin.len, row->e));
                cv_rrect(c, (int)xa - S(1), top, (int)(xb - xa) + S(2), th, S(3), A.c.match);
            }
        double x = x0;
        for (int i = row->s; i < row->e;) {
            int cl;
            uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl);
            uint16_t st = p->st[i];
            if (st & ST_HIDE) { i += cl; continue; }
            int f = para_font(p, i);
            double w = (cp == '\t' ? 4 * font_adv26(dfont[f], ' ') : font_adv26(dfont[f], cp)) / 64.0;
            Color col = dv_color(g, st);
            if ((st & ST_CODE) && !g->code) cv_fill(c, (int)x, top + S(1), (int)ceil(w), th - S(2), A.c.field);
            if (cp != ' ' && cp != '\t') draw_glyph26(c, dfont[f], (int)lround(x * 64), base, cp, col);
            double adv = w + (cp == ' ' ? jx : 0);
            if (st & (ST_U | ST_LINK)) cv_fill(c, (int)x, base + MAX(1, S(2)), (int)ceil(adv), MAX(1, S(1)), col);
            if (st & ST_S) cv_fill(c, (int)x, base - g->asc * 3 / 10, (int)ceil(adv), MAX(1, S(1)), col);
            x += adv;
            i += cl;
        }
        if (spell_on(d) && !g->code) dv_spell_row(c, d, li, g, r, base);
        if (li == d->cur.line && dv_row_of(d->cur.col) == r)
            caret_at((int)dv_x_of(d, li, g, r, d->cur.col), top, MAX(2, S(2)), th);
    }
}

static int tb_btn(Canvas *c, int x, int y, int h, uint32_t icon, const char *fb, int act, int arg, bool on) {
    icon_button(c, (Rect){x, y, h, h}, icon, fb, act, arg, on);
    return x + h + S(2);
}
static int tb_sep(Canvas *c, int x, int y, int h) {
    cv_fill(c, x + S(5), y + S(9), MAX(1, S(1)), h - S(18), A.c.border);
    return x + S(12);
}

static void draw_toolbar(Canvas *c) {
    Doc *d = D();
    Rect t = A.r_tool, saved;
    push_clip(c, t, &saved);
    int h = t.h, x = t.x, y = t.y;
    int k;
    bool code;
    uint16_t st = dv_style_at_cursor(d, &k, &code);
    static const char *names[] = {"Body Text", "Heading 1", "Heading 2", "Heading 3", "Bulleted List", "Numbered List", "Quote", "Rule"};
    const char *nm = code ? "Code Block" : names[k];
    int sw = text_width(A.ui, "Numbered List", 13) + S(52);
    Rect sr = {x, y, sw, h};
    cv_rrect(c, sr.x, sr.y, sr.w, sr.h, S(9), hovered(A_TB_STYLE, 0) ? A.c.hover : A.c.field);
    draw_text(c, A.ui, sr.x + S(12), ui_base(A.ui, sr.y, h), nm, (int)strlen(nm), A.c.text);
    draw_icon(c, sr.x + sr.w - S(26), sr.y, h, I_CHEV_DOWN, "v", A.c.muted);
    zone(sr, A_TB_STYLE, 0);
    x = tb_sep(c, x + sw + S(2), y, h);
    x = tb_btn(c, x, y, h, I_BOLD, "B", A_TB_FMT, ST_B, st & ST_B);
    x = tb_btn(c, x, y, h, I_ITALIC, "I", A_TB_FMT, ST_I, st & ST_I);
    x = tb_btn(c, x, y, h, I_UNDERLINE, "U", A_TB_FMT, ST_U, st & ST_U);
    x = tb_btn(c, x, y, h, I_STRIKE, "S", A_TB_FMT, ST_S, st & ST_S);
    x = tb_sep(c, x, y, h);
    x = tb_btn(c, x, y, h, I_AL_L, "L", A_TB_ALIGN, AL_LEFT, d->align == AL_LEFT);
    x = tb_btn(c, x, y, h, I_AL_C, "C", A_TB_ALIGN, AL_CENTER, d->align == AL_CENTER);
    x = tb_btn(c, x, y, h, I_AL_R, "R", A_TB_ALIGN, AL_RIGHT, d->align == AL_RIGHT);
    x = tb_btn(c, x, y, h, I_AL_J, "J", A_TB_ALIGN, AL_JUSTIFY, d->align == AL_JUSTIFY);
    x = tb_sep(c, x, y, h);
    char sp[16];
    snprintf(sp, sizeof sp, d->spacing % 100 ? (d->spacing % 10 ? "%.2f" : "%.1f") : "%.1f", d->spacing / 100.0);
    int spw = text_width(A.ui, sp, (int)strlen(sp)) + icon_w(I_LINE_H, "") + S(30);
    Rect spr = {x, y, spw, h};
    bool shv = hovered(A_TB_SPACING, 0);
    if (shv) cv_rrect(c, spr.x, spr.y, spr.w, spr.h, h / 2, A.c.hover);
    int ix = spr.x + S(10);
    ix += draw_icon(c, ix, y, h, I_LINE_H, "↕", shv ? A.c.primary : A.c.muted) + S(6);
    draw_text(c, A.ui, ix, ui_base(A.ui, y, h), sp, (int)strlen(sp), shv ? A.c.primary : A.c.text);
    zone(spr, A_TB_SPACING, 0);
    x = tb_sep(c, x + spw + S(2), y, h);
    x = tb_btn(c, x, y, h, I_LIST, "•", A_TB_LIST, BK_BULLET, k == BK_BULLET);
    x = tb_btn(c, x, y, h, I_LINES, "1.", A_TB_LIST, BK_NUMBER, k == BK_NUMBER);
    x = tb_btn(c, x, y, h, I_QUOTE, "\"", A_TB_LIST, BK_QUOTE, k == BK_QUOTE);
    x = tb_btn(c, x, y, h, I_INDENT, "→", A_TB_INDENT, 0, d->indent);
    cv_clip(c, saved);
}

static void draw_scrollbar(Canvas *c, Doc *d) {
    Rect card = A.r_card;
    int track_x = card.x + card.w - S(11);
    int ty0 = d->docmode ? A.r_text.y : card.y + S(8);
    A.r_sb_track = (Rect){track_x, ty0, S(7), card.y + card.h - S(8) - ty0};
    A.r_sb_thumb = (Rect){0};
    bool show = d->docmode ? (d->top_line > 0 || d->top_px > 0 || d->nlines > 20)
                           : (d->nlines > A.vis_rows || (A.cfg.wrap && (d->top_line > 0 || d->top_row > 0 || d->nlines > A.vis_rows / 2)));
    if (!show) return;
    int content = d->nlines + A.vis_rows * 2 / 3;
    int th = MAX(S(28), (int)((long long)A.r_sb_track.h * A.vis_rows / MAX(1, content)));
    th = MIN(th, A.r_sb_track.h);
    int maxtop = MAX(1, d->nlines - 1 - A.vis_rows / 3);
    int ty = A.r_sb_track.y + (int)((long long)(A.r_sb_track.h - th) * MIN(d->top_line, maxtop) / maxtop);
    A.r_sb_thumb = (Rect){track_x, ty, S(7), th};
    bool hv = hovered(A_SCROLLBAR, 0) || A.drag_sb;
    cv_rrect(c, track_x + (hv ? 0 : S(1)), ty, hv ? S(7) : S(5), th, S(4), hv ? color_alpha(A.c.primary, 0xa0) : A.c.thumb);
    zone((Rect){track_x - S(4), A.r_sb_track.y, S(15), A.r_sb_track.h}, A_SCROLLBAR, 0);
}

static void draw_doc(Canvas *c) {
    Doc *d = D();
    load_doc_fonts();
    Rect card = A.r_card;
    cv_rrect(c, card.x, card.y, card.w, card.h, S(A.cfg.radius), A.c.card);
    cv_fill(c, card.x + S(10), A.r_tool.y + A.r_tool.h + S(7), card.w - S(20), MAX(1, S(1)), color_alpha(A.c.border, 0x80));
    zone((Rect){card.x, A.r_text.y - S(8), card.w - S(16), card.y + card.h - A.r_text.y + S(8)}, A_TEXT, 0);
    draw_toolbar(c);
    dv_clamp(d);
    Rect saved;
    push_clip(c, (Rect){card.x, A.r_text.y - S(4), card.w, A.r_text.h + S(8)}, &saved);
    Rect clip = cv_get_clip(c);
    A.caret_rect = (Rect){0};
    int y = A.r_text.y - d->top_px + (int)lroundf(A.soff), li0 = d->top_line;
    while (y > A.r_text.y && li0 > 0) y -= para_h(d, --li0);
    PG g;
    A.cp_on = false;
    for (int li = li0; li < d->nlines && y < clip.y + clip.h; li++) {
        para_geo(d, li, &g);
        if (y + g.height > clip.y) draw_para(c, d, li, &g, y);
        y += g.height;
    }
    caret_flush(c);
    cv_clip(c, saved);
    draw_scrollbar(c, d);
}

static long dv_word_count(Doc *d) {
    static const Doc *wd;
    static uint64_t wv;
    static long words;
    if (wd == d && wv == d->version) return words;
    words = 0;
    for (int i = 0; i < d->nlines; i++) {
        Line *l = &d->lines[i];
        bool in = false;
        for (int k = 0; k < l->len; k++) {
            unsigned char ch = l->s[k];
            bool w = isalnum(ch) || ch >= 0x80 || ch == '\'';
            if (w && !in) words++;
            in = w;
        }
    }
    wd = d;
    wv = d->version;
    return words;
}


/* ---- auto-closing brackets and quotes */
static bool ac_type(Doc *d, char ch) {
    if (!A.cfg.auto_close) return false;
    static const char *open = "([{\"'`", *close = ")]}\"'`";
    Line *l = &d->lines[d->cur.line];
    char next = d->cur.col < l->len ? l->s[d->cur.col] : 0;
    char prev = d->cur.col > 0 ? l->s[d->cur.col - 1] : 0;
    bool quote = ch == '"' || ch == '\'' || ch == '`';
    if (quote && d->docmode) return false; /* prose: apostrophes and quotes stay single */
    /* step over a closer that's already there */
    if (!doc_has_sel(d) && strchr(")]}\"'`", ch) && next == ch) {
        if (!quote || (prev && prev != ' ')) {
            d->cur.col++;
            d->anc = d->cur;
            return true;
        }
    }
    const char *o = strchr(open, ch);
    if (!o || !ch) return false;
    char cl = close[o - open];
    if (doc_has_sel(d)) { /* wrap the selection */
        Pos a, b;
        doc_sel(d, &a, &b);
        begin(EK_OTHER);
        doc_insert(d, b, &cl, 1);
        doc_insert(d, a, &ch, 1);
        bool fwd = pos_cmp(d->cur, d->anc) > 0;
        a.col++;
        if (b.line == a.line) b.col++;
        d->anc = fwd ? a : b;
        d->cur = fwd ? b : a;
        return true;
    }
    bool next_ok = !next || next == ' ' || next == '\t' || strchr(")]};,:", next);
    if (!next_ok) return false;
    if (quote && (isalnum((unsigned char)prev) || prev == ch || (prev & 0x80))) return false;
    if (quote) { /* don't pair inside an existing string or comment */
        uint8_t st = hl_state(d, d->cur.line);
        if (g_cls_cap < l->len + 1) { g_cls_cap = l->len + 256; g_cls = xrealloc(g_cls, g_cls_cap); }
        hl_line(d->lang, l->s, l->len, st, g_cls);
        if (d->cur.col > 0 && (g_cls[d->cur.col - 1] == T_STR || g_cls[d->cur.col - 1] == T_COMMENT) && d->lang != lang_at(0)) return false;
    }
    char pair[2] = {ch, cl};
    begin(EK_OTHER);
    doc_insert(d, d->cur, pair, 2);
    d->cur.col++;
    d->anc = d->cur;
    return true;
}

static bool ac_backspace(Doc *d) {
    if (!A.cfg.auto_close || doc_has_sel(d)) return false;
    Line *l = &d->lines[d->cur.line];
    int c = d->cur.col;
    if (c <= 0 || c >= l->len) return false;
    static const char *pairs[] = {"()", "[]", "{}", "\"\"", "''", "``"};
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++)
        if (l->s[c - 1] == pairs[i][0] && l->s[c] == pairs[i][1]) {
            begin(EK_DELETE);
            doc_delete(d, (Pos){d->cur.line, c - 1}, (Pos){d->cur.line, c + 1});
            d->cur = d->anc = (Pos){d->cur.line, c - 1};
            return true;
        }
    return false;
}


/* number of indent guides for a line; blank lines borrow from their neighbours */
static int line_indent_px(const Doc *d, int li, bool *blank) {
    const Line *l = &d->lines[li];
    int f = first_nonws(l);
    *blank = f == l->len;
    return measure(d, l->s, 0, f);
}
static int indent_levels(const Doc *d, int li) {
    int tw = MAX(1, (A.sp26 * d->tab_w + 32) >> 6);
    bool blank;
    int px = line_indent_px(d, li, &blank);
    if (blank || px % tw) { /* blank or alignment-indented: follow the surrounding block */
        int up = 0, dn = 0;
        bool b2 = true;
        for (int k = li - 1; k >= 0 && k > li - 200 && b2; k--) { up = line_indent_px(d, k, &b2); if (!b2 && up % tw) b2 = true; }
        if (b2) up = 0;
        if (!blank) return MIN(up, px) / tw;
        b2 = true;
        for (int k = li + 1; k < d->nlines && k < li + 200 && b2; k++) dn = line_indent_px(d, k, &b2);
        if (b2) dn = 0;
        return MIN(up, dn) / tw;
    }
    return px / tw;
}

static bool find_bracket(Doc *d, Pos *p1, Pos *p2) {
    static const char *open = "([{", *close = ")]}";
    Line *l = &d->lines[d->cur.line];
    int cands[2] = {d->cur.col, d->cur.col - 1};
    for (int k = 0; k < 2; k++) {
        int i = cands[k];
        if (i < 0 || i >= l->len) continue;
        char ch = l->s[i];
        const char *o = strchr(open, ch), *cl = strchr(close, ch);
        if (!ch || (!o && !cl)) continue;
        int dir = o ? 1 : -1;
        char want = o ? close[o - open] : open[cl - close];
        int depth = 0, li = d->cur.line, ci = i, budget = 200000;
        while (budget-- > 0) {
            Line *L = &d->lines[li];
            if (ci >= 0 && ci < L->len) {
                if (L->s[ci] == ch) depth++;
                else if (L->s[ci] == want && --depth == 0) {
                    *p1 = (Pos){d->cur.line, i};
                    *p2 = (Pos){li, ci};
                    return true;
                }
            }
            ci += dir;
            if (ci < 0 || ci >= L->len) {
                if (dir > 0 && ci >= L->len) { if (++li >= d->nlines) break; ci = 0; if (!d->lines[li].len) { ci = -1; continue; } }
                else if (dir < 0 && ci < 0) { if (--li < 0) break; ci = d->lines[li].len - 1; if (ci < 0) continue; }
            }
        }
    }
    return false;
}

static void draw_editor(Canvas *c) {
    Doc *d = D();
    if (d->docmode) { draw_doc(c); return; }
    Rect card = A.r_card;
    cv_rrect(c, card.x, card.y, card.w, card.h, S(A.cfg.radius), A.c.card);
    zone(A.r_gutter, A_TEXT, 0);
    zone((Rect){A.r_text.x, card.y, A.r_text.w + S(4), card.h}, A_TEXT, 0);
    clamp_scroll(d);

    Pos sa, sb;
    doc_sel(d, &sa, &sb);
    bool has_sel = doc_has_sel(d);
    Pos b1, b2;
    bool brk = find_bracket(d, &b1, &b2);

    /* highlight occurrences: active find needle, or the current single-line selection */
    const char *nd = NULL;
    int nn = 0;
    bool ncs = A.find_case, nw = A.find_word;
    char selbuf[256];
    if (A.find_open && A.fin.len) { nd = A.fin.s; nn = A.fin.len; }
    else if (has_sel && sa.line == sb.line && sb.col - sa.col >= 2 && sb.col - sa.col < (int)sizeof selbuf) {
        nn = sb.col - sa.col;
        memcpy(selbuf, d->lines[sa.line].s + sa.col, nn);
        bool ok = true;
        for (int i = 0; i < nn; i++) if (selbuf[i] == ' ' || selbuf[i] == '\t') ok = false;
        if (ok) { nd = selbuf; ncs = true; nw = false; } else nn = 0;
    }

    Rect saved;
    Rect textclip = {card.x, A.r_text.y, card.w, A.r_text.h};
    push_clip(c, textclip, &saved);
    int caret_w = MAX(2, S(2));
    int l = d->top_line, r = d->top_row;
    int soff = (int)lroundf(A.soff);
    int y = A.r_text.y + soff;
    if (soff > 0) {
        int back = (soff + A.lh - 1) / A.lh, l0 = l, r0 = r;
        step_rows(d, &l, &r, -back);
        y -= rows_between(d, l, r, l0, r0, back + 1) * A.lh;
    }
    A.cp_on = false;
    int x0 = A.r_text.x - d->scroll_x;
    Rect clip = cv_get_clip(c);
    A.caret_rect = (Rect){0};
    Rect tclip = rect_intersect(clip, (Rect){A.r_text.x, A.r_text.y, A.r_text.w + S(8), A.r_text.h});
    while (l < d->nlines && y < A.r_text.y + A.r_text.h) {
        Line *ln = &d->lines[l];
        int nrows = wrap_line(d, l);
        int rs_buf[8], *rs = rs_buf;
        if (nrows > 8) rs = xmalloc(nrows * sizeof(int));
        memcpy(rs, g_rows, nrows * sizeof(int));
        /* syntax classes for the whole line */
        if (g_cls_cap < ln->len + 1) { g_cls_cap = ln->len + 256; g_cls = xrealloc(g_cls, g_cls_cap); }
        bool visible_line = y + (nrows - r) * A.lh > clip.y && y < clip.y + clip.h;
        if (visible_line) hl_line(d->lang, ln->s, ln->len, hl_state(d, l), g_cls);
        for (; r < nrows && y < A.r_text.y + A.r_text.h; r++, y += A.lh) {
            if (y + A.lh <= clip.y || y >= clip.y + clip.h) continue;
            int s0 = rs[r], s1 = r + 1 < nrows ? rs[r + 1] : ln->len;
            bool cur_line = l == d->cur.line;
            if (cur_line && A.cfg.highlight_line && !has_sel)
                cv_rrect(c, card.x + S(6), y, card.w - S(12), A.lh, S(6), A.c.line_hl);
            if (r == 0 && A.cfg.line_numbers) {
                char num[16];
                int nl = snprintf(num, sizeof num, "%d", l + 1);
                int w = text_width(A.mono, num, nl);
                draw_text(c, A.mono, A.r_gutter.x + A.r_gutter.w - S(16) - w, y + A.asc, num, nl,
                          cur_line ? A.c.primary : A.c.faint);
            }
            Rect sv2;
            push_clip(c, tclip, &sv2);
            if (A.cfg.indent_guides && r == 0) {
                int lvl = indent_levels(d, l);
                int tw26 = MAX(64, A.sp26 * d->tab_w);
                for (int k = 0; k < lvl; k++)
                    cv_fill(c, x0 + ((k * tw26 + 32) >> 6) + S(1), y, MAX(1, S(1)), A.lh, color_alpha(A.c.faint, 0x38));
            }
            /* selection */
            if (has_sel && l >= sa.line && l <= sb.line) {
                int c0 = l == sa.line ? sa.col : 0, c1 = l == sb.line ? sb.col : ln->len;
                int a0 = MAX(c0, s0), a1 = MIN(c1, s1);
                bool eol = l < sb.line && r == nrows - 1;
                if (a0 < a1 || (eol && c0 <= s1)) {
                    int xa = a0 <= a1 ? measure(d, ln->s, s0, MAX(a0, s0)) : 0;
                    int xb = a0 < a1 ? measure(d, ln->s, s0, a1) : xa;
                    if (a0 > a1) xa = xb = measure(d, ln->s, s0, s1);
                    if (eol) xb += A.sp / 2 + S(2);
                    cv_fill(c, x0 + xa, y, xb - xa, A.lh, A.c.sel);
                }
            }
            /* occurrences */
            if (nd && nn)
                for (int j = find_in(ln->s, ln->len, s0 > nn ? s0 - nn + 1 : 0, nd, nn, ncs, nw); j >= 0 && j < s1;
                     j = find_in(ln->s, ln->len, j + nn, nd, nn, ncs, nw)) {
                    int xa = measure(d, ln->s, s0, MAX(j, s0)), xb = measure(d, ln->s, s0, MIN(j + nn, s1));
                    cv_rrect(c, x0 + xa - S(1), y + S(1), xb - xa + S(2), A.lh - S(2), S(3), A.c.match);
                }
            /* bracket pair */
            if (brk) {
                Pos bp[2] = {b1, b2};
                for (int k = 0; k < 2; k++)
                    if (bp[k].line == l && bp[k].col >= s0 && bp[k].col < s1) {
                        int xa = measure(d, ln->s, s0, bp[k].col);
                        int w = (font_adv26(A.mono, (unsigned char)ln->s[bp[k].col]) + 32) >> 6;
                        cv_rrect(c, x0 + xa - S(1), y + S(1), w + S(2), A.lh - S(2), S(3), A.c.bracket);
                    }
            }
            /* glyphs */
            int x = 0, base = y + A.asc, X0 = x0 << 6;
            int limit = (d->scroll_x + A.r_text.w + S(8)) << 6, sx = d->scroll_x << 6;
            for (int i = s0; i < s1;) {
                int cl;
                uint32_t cp = utf8_dec(ln->s + i, s1 - i, &cl);
                int w = adv_cp(d, x, cp);
                if (x + w >= sx) {
                    if (cp == '\t') {
                        if (A.cfg.whitespace) draw_glyph26(c, A.mono, X0 + x + (w - font_adv26(A.mono, 0x2192)) / 2, base, 0x2192, A.c.faint);
                    } else if (cp == ' ') {
                        if (A.cfg.whitespace) draw_glyph26(c, A.mono, X0 + x, base, 0x00B7, A.c.faint);
                    } else if (cp < 0x20) {
                        cv_rrect(c, x0 + (x >> 6) + S(1), y + S(4), (w >> 6) - S(2), A.lh - S(8), S(2), color_alpha(A.c.error, 0x60));
                    } else {
                        uint8_t t = visible_line ? g_cls[i] : T_TEXT;
                        draw_glyph26(c, A.mono, X0 + x, base, cp, A.c.syn[t < T_COUNT ? t : 0]);
                    }
                }
                x += w;
                i += cl;
                if (x > limit) break;
            }
            if (visible_line && spell_on(d)) spell_row(c, d, l, s0, s1, x0, y);
            /* caret */
            if (cur_line && d->cur.col >= s0 && (d->cur.col < s1 || (d->cur.col == s1 && r == nrows - 1)))
                caret_at(x0 + measure(d, ln->s, s0, d->cur.col), y + S(2), caret_w, A.lh - S(4));
            cv_clip(c, sv2);
        }
        if (rs != rs_buf) free(rs);
        l++;
        r = 0;
    }
    caret_flush(c);
    cv_clip(c, saved);
    draw_scrollbar(c, d);
}

static int status_pill(Canvas *c, int right, const char *label, uint32_t icon, int act, bool warn) {
    int tw = text_width(A.ui, label, (int)strlen(label));
    int iw = icon ? icon_w(icon, "") + S(6) : 0;
    int w = tw + iw + S(20);
    Rect r = {right - w, A.r_status.y, w, A.r_status.h};
    float k = hk(act, 0);
    Color bg = warn ? color_alpha(A.c.error, 0x30) : color_mix(A.c.card, A.c.hover, k);
    cv_rrect(c, r.x, r.y, r.w, r.h, r.h / 2, bg);
    Color fg = warn ? A.c.error : color_mix(A.c.muted, A.c.primary, k);
    int x = r.x + S(10);
    if (icon) x += draw_icon(c, x, r.y, r.h, icon, "", fg) + S(6);
    draw_text(c, A.ui, x, ui_base(A.ui, r.y, r.h), label, (int)strlen(label), fg);
    zone(r, act, 0);
    return r.x - S(6);
}

static void draw_status(Canvas *c) {
    Doc *d = D();
    Rect s = A.r_status;
    char buf[128];
    int right = s.x + s.w;
    right = status_pill(c, right, lang_name(d->lang), I_CODE, A_ST_LANG, false);
    right = status_pill(c, right, d->crlf ? "CRLF" : "LF", 0, A_ST_EOL, false);
    right = status_pill(c, right, "UTF-8", 0, A_NONE, false);
    right = status_pill(c, right, d->docmode ? "Document" : "Code", d->docmode ? I_FILE_TEXT : I_CODE, A_ST_MODE, false);
    snprintf(buf, sizeof buf, "%s: %d", d->use_spaces ? "Spaces" : "Tab", d->tab_w);
    if (!d->docmode) right = status_pill(c, right, buf, 0, A_ST_INDENT, false);
    if (d->docmode) {
        snprintf(buf, sizeof buf, "%ld words", dv_word_count(d));
        right = status_pill(c, right, buf, 0, A_NONE, false);
    } else
        right = status_pill(c, right, A.cfg.wrap ? "Wrap" : "No Wrap", I_WRAP, A_ST_WRAP, false);
    if (doc_has_sel(d)) {
        Pos a, b;
        doc_sel(d, &a, &b);
        long chars = 0;
        for (int i = a.line; i <= b.line && i - a.line < 200000; i++) {
            Line *l = &d->lines[i];
            int s0 = i == a.line ? a.col : 0, s1 = i == b.line ? b.col : l->len;
            for (int k = s0; k < s1; k++) chars += ((unsigned char)l->s[k] & 0xC0) != 0x80;
            chars += i < b.line;
        }
        if (b.line > a.line) snprintf(buf, sizeof buf, "Ln %d, Col %d  (%ld selected, %d lines)", d->cur.line + 1, d->cur.col + 1, chars, b.line - a.line + 1);
        else snprintf(buf, sizeof buf, "Ln %d, Col %d  (%ld selected)", d->cur.line + 1, d->cur.col + 1, chars);
    } else {
        Line *l = &d->lines[d->cur.line];
        int col = 1, x = 0;
        for (int i = 0; i < d->cur.col;) { int cl; uint32_t cp = utf8_dec(l->s + i, l->len - i, &cl); if (cp == '\t') { col += d->tab_w - (x % d->tab_w); x += d->tab_w - (x % d->tab_w); } else { col++; x++; } i += cl; }
        snprintf(buf, sizeof buf, "Ln %d, Col %d", d->cur.line + 1, col);
    }
    right = status_pill(c, right, buf, 0, A_ST_POS, false);
    if (!d->readonly && A.cfg.spellcheck) right = status_pill(c, right, "Spell", I_SPELL, A_ST_SPELL, false);
    if (d->readonly) right = status_pill(c, right, "PDF · read-only", I_LOCK, A_NONE, false);
    if (d->on_disk_changed) right = status_pill(c, right, "Changed on disk", I_ALERT, A_ST_DISK, true);
    /* left: path */
    int x = s.x + S(6);
    x += draw_icon(c, x, s.y, s.h, I_FOLDER, "", A.c.faint) + S(8);
    const char *home = getenv("HOME");
    char path[PATH_MAX + 2];
    if (d->path && home && !strncmp(d->path, home, strlen(home)) && d->path[strlen(home)] == '/')
        snprintf(path, sizeof path, "~%s", d->path + strlen(home));
    else snprintf(path, sizeof path, "%s", d->path ? d->path : "Not saved yet");
    draw_text_fit(c, A.ui, x, ui_base(A.ui, s.y, s.h), path, right - x - S(12), A.c.faint, true);
}

static void draw_findbar(Canvas *c) {
    Doc *d = D();
    int rowh = S(36), pad = S(8);
    int w = MIN(S(620), A.r_card.w - S(24));
    int h = pad * 2 + rowh + (A.replace_open ? rowh + S(6) : 0);
    Rect p = {A.r_card.x + A.r_card.w - w - S(18), A.r_card.y + S(10) + (D()->docmode ? S(46) : 0) - (int)lroundf((1.f - ease(prog(A.find_t0, 160))) * S(12)), w, h};
    panel(c, p, S(12));
    int x = p.x + pad, y = p.y + pad;
    icon_button(c, (Rect){x, y + S(4), S(28), S(28)}, A.replace_open ? I_CHEV_DOWN : I_CHEV_RIGHT, ">", A_FIND_TOGGLE, 0, false);
    x += S(32);
    int bw = S(30);
    int right = p.x + p.w - pad;
    int total, idx;
    find_counts(d, &total, &idx);
    char cnt[64];
    if (!A.fin.len) cnt[0] = 0;
    else if (!total) snprintf(cnt, sizeof cnt, "No results");
    else if (idx) snprintf(cnt, sizeof cnt, "%d of %d", idx, total);
    else snprintf(cnt, sizeof cnt, "%d found", total);
    int cw = MAX(text_width(A.ui, "999 of 999", 10), text_width(A.ui, cnt, (int)strlen(cnt)));
    int fx_end = right - 5 * (bw + S(2)) - cw - S(10);
    draw_input(c, (Rect){x, y, fx_end - x, rowh}, &A.fin, A.mode == M_FIND && A.find_focus == 0, "Find", A_FIND_FIELD);
    draw_text(c, A.ui, fx_end + S(8), ui_base(A.ui, y, rowh), cnt, (int)strlen(cnt), A.fin.len && !total ? A.c.error : A.c.muted);
    int bx = right - 5 * (bw + S(2));
    icon_button(c, (Rect){bx, y + S(3), bw, bw}, I_CASE, "Aa", A_FIND_CASE, 0, A.find_case); bx += bw + S(2);
    icon_button(c, (Rect){bx, y + S(3), bw, bw}, I_LETTER_W, "W", A_FIND_WORD, 0, A.find_word); bx += bw + S(2);
    icon_button(c, (Rect){bx, y + S(3), bw, bw}, I_UP, "↑", A_FIND_PREV, 0, false); bx += bw + S(2);
    icon_button(c, (Rect){bx, y + S(3), bw, bw}, I_DOWN, "↓", A_FIND_NEXT, 0, false); bx += bw + S(2);
    icon_button(c, (Rect){bx, y + S(3), bw, bw}, I_X, "×", A_FIND_CLOSE, 0, false);
    if (A.replace_open) {
        y += rowh + S(6);
        int b1 = text_width(A.ui, "Replace", 7) + S(28), b2 = text_width(A.ui, "All", 3) + S(28);
        int rx_end = right - b1 - b2 - S(12);
        draw_input(c, (Rect){x, y, rx_end - x, rowh}, &A.rep, A.mode == M_FIND && A.find_focus == 1, "Replace", A_REPL_FIELD);
        text_button(c, (Rect){rx_end + S(8), y + S(2), b1, rowh - S(4)}, "Replace", A_REPL_ONE, 0, false);
        text_button(c, (Rect){right - b2, y + S(2), b2, rowh - S(4)}, "All", A_REPL_ALL, 0, false);
    }
}

static void draw_list_item(Canvas *c, Rect r, uint32_t icon, const char *label, const char *hint, bool sel, int act, int arg) {
    float k = hk(act, arg);
    if (sel) cv_rrect(c, r.x, r.y, r.w, r.h, S(8), A.c.primary);
    else if (k > 0) cv_rrect(c, r.x, r.y, r.w, r.h, S(8), color_alpha(A.c.hover, (int)(255 * k)));
    Color fg = sel ? A.c.on_primary : A.c.text, fg2 = sel ? A.c.on_primary : A.c.faint;
    int x = r.x + S(12);
    if (icon == 1) x += icon_w(I_CHECK, "") + S(10);
    else if (icon) x += draw_icon(c, x, r.y, r.h, icon, "", sel ? A.c.on_primary : A.c.primary) + S(10);
    int hw = hint && *hint ? text_width(A.ui, hint, (int)strlen(hint)) + S(16) : 0;
    draw_text_fit(c, A.ui, x, ui_base(A.ui, r.y, r.h), label, r.x + r.w - x - hw - S(8), fg, false);
    if (hw) draw_text(c, A.ui, r.x + r.w - hw, ui_base(A.ui, r.y, r.h), hint, (int)strlen(hint), fg2);
    zone(r, act, arg);
}

static void backdrop(Canvas *c) {
    cv_fill(c, 0, 0, A.W, A.H, color_alpha(0, (int)((A.pal.light ? 0x30 : 0x60) * A.pop_k)));
    zone((Rect){0, 0, A.W, A.H}, A_MODAL_BG, 0);
}

static void draw_palette(Canvas *c) {
    backdrop(c);
    int w = MIN(S(640), A.W - S(40));
    int ih = S(36), maxi = MAX(3, MIN(10, (A.H - S(200)) / ih));
    int n = MIN(A.pal_n, maxi);
    int h = S(12) * 2 + S(40) + S(8) + MAX(n, 1) * ih;
    Rect p = {(A.W - w) / 2, MAX(S(50), A.H / 7) + A.pop_dy, w, h};
    panel(c, p, S(A.cfg.radius + 2));
    draw_input(c, (Rect){p.x + S(12), p.y + S(12), p.w - S(24), S(40)}, &A.pin, true, "Type a command, language or open tab…", A_NONE);
    if (A.pal_sel < A.pal_scroll) A.pal_scroll = A.pal_sel;
    if (A.pal_sel >= A.pal_scroll + maxi) A.pal_scroll = A.pal_sel - maxi + 1;
    int y = p.y + S(12) + S(40) + S(8);
    if (!A.pal_n) {
        const char *m = "No matching commands";
        draw_text(c, A.ui, p.x + S(24), ui_base(A.ui, y, ih), m, (int)strlen(m), A.c.faint);
    }
    char lab[256];
    for (int k = 0; k < n; k++) {
        int i = A.pal_scroll + k;
        if (i >= A.pal_n) break;
        int item = A.pal_items[i];
        pal_label(item, lab, sizeof lab);
        uint32_t icon = item >= PAL_RECENT ? I_HISTORY : item >= PAL_TAB ? I_FILE_TEXT : item >= PAL_LANG ? I_CODE_FILE : cmds[item].icon;
        const char *hint = item < PAL_LANG ? cmds[item].keys : item >= PAL_TAB ? "" : (D()->lang == lang_at(item - PAL_LANG) ? "current" : "");
        draw_list_item(c, (Rect){p.x + S(8), y, p.w - S(16), ih}, icon, lab, hint, i == A.pal_sel, A_PAL_ITEM, i);
        y += ih;
    }
}

static void draw_goto(Canvas *c) {
    int w = MIN(S(420), A.W - S(40));
    Rect p = {(A.W - w) / 2, A.r_card.y + S(20) + A.pop_dy, w, S(12) * 2 + S(40) + S(24)};
    panel(c, p, S(12));
    char ph[96];
    snprintf(ph, sizeof ph, "Line number (1–%d), optionally :column", D()->nlines);
    draw_input(c, (Rect){p.x + S(12), p.y + S(12), p.w - S(24), S(40)}, &A.gin, true, ph, A_NONE);
    char hint[96];
    snprintf(hint, sizeof hint, "Current: line %d of %d · Enter to jump · Esc to cancel", D()->cur.line + 1, D()->nlines);
    draw_text_fit(c, A.ui, p.x + S(16), ui_base(A.ui, p.y + S(56), S(24)), hint, p.w - S(32), A.c.faint, false);
}

static void draw_dialog(Canvas *c) {
    backdrop(c);
    int w = MIN(S(480), A.W - S(40));
    int h = S(170);
    Rect p = {(A.W - w) / 2, (A.H - h) / 2 + A.pop_dy, w, h};
    panel(c, p, S(A.cfg.radius + 2));
    int x = p.x + S(22);
    Font *tf = A.ui_big ? A.ui_big : A.ui;
    draw_icon(c, x, p.y + S(22), S(30), A.dlg_kind == DLG_OVERWRITE ? I_ALERT : I_SAVE, "", A.c.primary);
    draw_text_fit(c, tf, x + S(32), ui_base(tf, p.y + S(22), S(30)), A.dlg_title, p.w - S(76), A.c.text, false);
    draw_text_fit(c, A.ui, x, ui_base(A.ui, p.y + S(62), S(26)), A.dlg_msg, p.w - S(44), A.c.muted, false);
    int bx = p.x + p.w - S(20), by = p.y + p.h - S(20) - S(36);
    for (int i = A.dlg_nbtn - 1; i >= 0; i--) {
        int bw = text_width(A.ui, A.dlg_btn[i], (int)strlen(A.dlg_btn[i])) + S(36);
        bx -= bw;
        Rect r = {bx, by, bw, S(36)};
        text_button(c, r, A.dlg_btn[i], A_DLG_BTN, i, i == A.dlg_sel);
        bx -= S(8);
    }
}

static void draw_browser(Canvas *c) {
    backdrop(c);
    int w = MIN(S(720), A.W - S(40));
    int h = MIN(S(560), A.H - S(60));
    Rect p = {(A.W - w) / 2, (A.H - h) / 2 + A.pop_dy, w, h};
    panel(c, p, S(A.cfg.radius + 2));
    int x = p.x + S(14), y = p.y + S(14);
    Font *tf = A.ui_big ? A.ui_big : A.ui;
    const char *title = A.br_mode == BR_OPEN ? "Open File" : A.br_mode == BR_EXPORT ? "Export as PDF" : "Save As";
    int iw = draw_icon(c, x, y, S(30), A.br_mode == BR_OPEN ? I_FOLDER_OPEN : A.br_mode == BR_EXPORT ? I_PDF : I_SAVE, "", A.c.primary);
    draw_text(c, tf, x + iw + S(10), ui_base(tf, y, S(30)), title, (int)strlen(title), A.c.text);
    int bw = S(30);
    icon_button(c, (Rect){p.x + p.w - S(14) - bw, y, bw, bw}, I_X, "×", A_MODAL_BG, 1, false);
    icon_button(c, (Rect){p.x + p.w - S(14) - 2 * bw - S(4), y, bw, bw}, I_EYE, ".", A_BR_HIDDEN, 0, A.br_hidden);
    icon_button(c, (Rect){p.x + p.w - S(14) - 3 * bw - S(8), y, bw, bw}, I_FOLDER_UP, "..", A_BR_UP, 0, false);
    y += S(38);
    const char *home = getenv("HOME");
    char dir[PATH_MAX + 2];
    if (home && !strncmp(A.br_dir, home, strlen(home)) && (A.br_dir[strlen(home)] == '/' || !A.br_dir[strlen(home)]))
        snprintf(dir, sizeof dir, "~%s", A.br_dir + strlen(home));
    else snprintf(dir, sizeof dir, "%s", A.br_dir);
    draw_text_fit(c, A.ui, x, ui_base(A.ui, y, S(22)), dir, p.w - S(28), A.c.muted, true);
    y += S(26);
    draw_input(c, (Rect){x, y, p.w - S(28), S(40)}, &A.bin, true,
               A.br_mode == BR_OPEN ? "Filter, or type a path…" : "File name, or a path…", A_NONE);
    y += S(48);
    int ih = S(32), foot = S(30);
    int maxi = MAX(1, (p.y + p.h - foot - y) / ih);
    if (A.br_sel < A.br_scroll) A.br_scroll = A.br_sel;
    if (A.br_sel >= A.br_scroll + maxi) A.br_scroll = A.br_sel - maxi + 1;
    Rect lr = {p.x + S(8), y, p.w - S(16), maxi * ih}, saved;
    push_clip(c, lr, &saved);
    if (!A.br_fn) {
        const char *m = A.br_n ? "No matches" : "Empty folder";
        draw_text(c, A.ui, x + S(10), ui_base(A.ui, y, ih), m, (int)strlen(m), A.c.faint);
    }
    for (int k = 0; k < maxi && A.br_scroll + k < A.br_fn; k++) {
        int i = A.br_scroll + k;
        Entry *e = &A.br_ent[A.br_f[i]];
        draw_list_item(c, (Rect){lr.x, y + k * ih, lr.w, ih}, e->dir ? I_FOLDER : I_FILE, e->name, e->dir ? "folder" : "",
                       i == A.br_sel, A_BR_ITEM, i);
    }
    cv_clip(c, saved);
    const char *hint = A.br_mode == BR_OPEN ? "Enter open · Backspace up · Ctrl+H hidden files · Esc cancel"
                       : A.br_mode == BR_EXPORT ? "Enter export · Tab complete · Backspace up · Esc cancel"
                                                : "Enter save · Tab complete · Backspace up · Esc cancel";
    draw_text_fit(c, A.ui, x, ui_base(A.ui, p.y + p.h - foot, foot - S(6)), hint, p.w - S(28), A.c.faint, false);
}

static void draw_toast(Canvas *c) {
    if (!A.toast[0]) return;
    float pin = ease(prog(A.toast_t0, 200)), pout = A.toast_out ? prog(A.toast_out, 180) : 0.f;
    if (pout >= 1.f) { A.toast[0] = 0; A.toast_out = 0; return; }
    float a = pin * (1.f - ease(pout));
    int al = (int)(255 * a), bw = MAX(1, S(1));
    int tw = text_width(A.ui, A.toast, (int)strlen(A.toast));
    int iw = icon_w(A.toast_err ? I_ALERT : I_CHECK, "");
    int w = MIN(tw + iw + S(44), A.W - S(40)), h = S(38);
    int dy = (int)lroundf((1.f - pin) * S(18) + ease(pout) * S(8));
    Rect r = {(A.W - w) / 2, A.r_status.y - h - S(14) + dy, w, h};
    for (int i = 3; i >= 1; i--) cv_rrect(c, r.x - S(i), r.y - S(i) + S(2), r.w + S(2 * i), r.h + S(2 * i), h / 2 + S(i), color_alpha(0, (int)(0x14 * a)));
    cv_rrect(c, r.x, r.y, r.w, r.h, h / 2, color_alpha(A.toast_err ? A.c.error : A.c.border, al));
    cv_rrect(c, r.x + bw, r.y + bw, r.w - 2 * bw, r.h - 2 * bw, h / 2 - bw, al == 255 ? A.c.panel : color_mix(A.c.bg, A.c.panel, a));
    int x = r.x + S(16);
    x += draw_icon(c, x, r.y, h, A.toast_err ? I_ALERT : I_CHECK, "", color_alpha(A.toast_err ? A.c.error : A.c.primary, al)) + S(10);
    draw_text_fit(c, A.ui, x, ui_base(A.ui, r.y, h), A.toast, r.x + r.w - x - S(14), color_alpha(A.c.text, al), false);
}

void app_render(Canvas *c) {
    A.now = plat_now_ms();
    A.anim_running = false;
    bool popup = A.mode == M_GOTO || A.mode == M_PALETTE || A.mode == M_BROWSER || A.mode == M_DIALOG || A.mode == M_SPELL || A.mode == M_MENU;
    if (A.mode != A.last_mode) {
        bool was = A.last_mode == M_GOTO || A.last_mode == M_PALETTE || A.last_mode == M_BROWSER || A.last_mode == M_DIALOG ||
                   A.last_mode == M_SPELL || A.last_mode == M_MENU;
        if (popup && !was) A.pop_t0 = A.now;
        A.last_mode = A.mode;
    }
    if (A.find_open && !A.last_find) A.find_t0 = A.now;
    A.last_find = A.find_open;
    A.pop_k = ease(prog(A.pop_t0, 170));
    A.pop_dy = (int)lroundf((1.f - A.pop_k) * S(14));
    A.soff = scroll_off_now();
    Rect clip = cv_get_clip(c);
    zrec = clip.x == 0 && clip.y == 0 && clip.w == c->w && clip.h == c->h;
    if (zrec) A.nz = 0;
    layout();
    cv_fill(c, 0, 0, c->w, c->h, A.c.bg);
    draw_topbar(c);
    draw_editor(c);
    draw_status(c);
    if (A.find_open) draw_findbar(c);
    if (A.mode == M_GOTO) draw_goto(c);
    else if (A.mode == M_PALETTE) draw_palette(c);
    else if (A.mode == M_BROWSER) draw_browser(c);
    if (A.mode == M_DIALOG) draw_dialog(c);
    if (A.mode == M_SPELL) draw_spell_menu(c);
    if (A.mode == M_MENU) draw_menu(c);
    draw_toast(c);
    update_title();
    A.now = 0;
    if (A.anim_running) plat_redraw(); /* next frame, paced by the compositor's frame callbacks */
}

/* ================================================================== caret blink */
static int idle_ms(void) {
    const char *e = getenv("NOCTEDIT_IDLE_MS");
    return e && atoi(e) > 0 ? atoi(e) : 15000;
}

static void activity(void) {
    plat_timer(TIMER_IDLE, idle_ms(), 0);
    A.last_act = plat_now_ms();
    A.caret_on = true;
    if (A.cfg.blink && A.focused) plat_timer(TIMER_BLINK, 530, 530);
}

void app_timer(int id) {
    if (id == TIMER_BLINK) {
        if (plat_now_ms() - A.last_act > 10000 || !A.focused) {
            A.caret_on = true;
            plat_timer(TIMER_BLINK, 0, 0);
        } else {
            A.caret_on = !A.caret_on;
        }
        if (A.mode == M_EDIT && A.caret_rect.w) plat_damage(A.caret_rect);
        else redraw(); /* caret lives in an input field */
    } else if (id == TIMER_IDLE) {
        /* nothing happened for a while: give memory back */
        spell_unload();
        if (!D()->docmode && dfont[0]) { /* document fonts reload on demand */
            for (int i = 0; i < FN_COUNT; i++) { font_free(dfont[i]); dfont[i] = NULL; }
        }
        fonts_trim();
        plat_trim();
        malloc_trim(0);
    } else if (id == TIMER_TOAST) {
        if (anim_on()) A.toast_out = plat_now_ms();
        else A.toast[0] = 0;
        redraw();
    } else if (id == TIMER_AUTOSCROLL) {
        if (!A.drag_text) { plat_timer(TIMER_AUTOSCROLL, 0, 0); return; }
        Doc *d = D();
        int dir = A.my < A.r_text.y ? -1 : A.my > A.r_text.y + A.r_text.h ? 1 : 0;
        if (!dir) { plat_timer(TIMER_AUTOSCROLL, 0, 0); return; }
        int dist = (int)(dir < 0 ? A.r_text.y - A.my : A.my - A.r_text.y - A.r_text.h);
        int step = 1 + dist / MAX(1, A.lh);
        if (d->docmode) dv_scroll(d, dir * step * dv_body_lh(d));
        else step_rows(d, &d->top_line, &d->top_row, dir * step);
        clamp_scroll(d);
        app_motion(A.mx, A.my);
        redraw();
    }
}

/* ================================================================== keyboard */
static uint32_t norm(uint32_t sym) {
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return sym + 32;
    return sym;
}

static bool global_shortcut(uint32_t sym, uint32_t mods) {
    bool ctrl = mods & MOD_CTRL, shift = mods & MOD_SHIFT, alt = mods & MOD_ALT;
    sym = norm(sym);
    if (sym == XKB_KEY_F1 || (ctrl && shift && sym == XKB_KEY_p)) { cmd_palette(); return true; }
    if (sym == XKB_KEY_F3) { shift ? cmd_find_prev() : cmd_find_next(); return true; }
    if (sym == XKB_KEY_F7) { cmd_next_misspelling(); return true; }
    if (sym == XKB_KEY_F12) { shift ? cmd_bullets() : cmd_numbers(); return true; }
    if (ctrl && shift && sym == XKB_KEY_m) { cmd_docmode(); return true; }
    if (alt && !ctrl && sym == XKB_KEY_z) { cmd_wrap(); return true; }
    if (alt && !ctrl && sym >= XKB_KEY_1 && sym <= XKB_KEY_9) { activate(MIN((int)(sym - XKB_KEY_1), A.ndocs - 1)); return true; }
    if (!ctrl) return false;
    switch (sym) {
    case XKB_KEY_n: cmd_new(); return true;
    case XKB_KEY_o: cmd_open(); return true;
    case XKB_KEY_s: if (alt) cmd_save_all(); else if (shift) cmd_save_as(); else cmd_save(); return true;
    case XKB_KEY_e: if (shift) { cmd_export_pdf(); return true; } return false;
    case XKB_KEY_w: cmd_close(); return true;
    case XKB_KEY_q: cmd_quit(); return true;
    case XKB_KEY_p: cmd_palette(); return true;
    case XKB_KEY_f: cmd_find(); return true;
    case XKB_KEY_h: cmd_replace(); return true;
    case XKB_KEY_g: cmd_goto(); return true;
    case XKB_KEY_Tab: cmd_next_tab(); return true;
    case XKB_KEY_ISO_Left_Tab: cmd_prev_tab(); return true;
    case XKB_KEY_Page_Down: cmd_next_tab(); return true;
    case XKB_KEY_Page_Up: cmd_prev_tab(); return true;
    case XKB_KEY_equal: case XKB_KEY_plus: case XKB_KEY_KP_Add: cmd_zoom_in(); return true;
    case XKB_KEY_minus: case XKB_KEY_underscore: case XKB_KEY_KP_Subtract: cmd_zoom_out(); return true;
    case XKB_KEY_0: case XKB_KEY_KP_0: cmd_zoom_reset(); return true;
    }
    return false;
}

static void move_cursor(Doc *d, Pos p, bool shift) {
    d->cur = doc_clamp(d, p);
    if (!shift) d->anc = d->cur;
    ensure_visible(d);
}

static void move_vert(Doc *d, int delta, bool shift) {
    if (!shift && doc_has_sel(d) && d->want_x < 0) {
        Pos a, b;
        doc_sel(d, &a, &b);
        d->cur = delta < 0 ? a : b;
    }
    int row, x = col_to_x(d, d->cur.line, d->cur.col, &row);
    if (d->want_x < 0) d->want_x = x;
    int l = d->cur.line, r = row;
    int ol = l, orow = r;
    step_rows(d, &l, &r, delta);
    Pos p;
    if (l == ol && r == orow) p = delta < 0 ? (Pos){0, 0} : (Pos){d->nlines - 1, d->lines[d->nlines - 1].len};
    else p = (Pos){l, x_to_col(d, l, r, d->want_x)};
    int keep = d->want_x;
    move_cursor(d, p, shift);
    d->want_x = keep;
}

static void editor_key(uint32_t sym, uint32_t mods, const char *utf8) {
    Doc *d = D();
    bool ctrl = mods & MOD_CTRL, shift = mods & MOD_SHIFT, alt = mods & MOD_ALT;
    uint32_t k = norm(sym);
    if (d->docmode && ctrl && !alt) {
        switch (k) {
        case XKB_KEY_b: cmd_bold(); return;
        case XKB_KEY_i: cmd_italic(); return;
        case XKB_KEY_u: if (!shift) { cmd_underline(); return; } break;
        case XKB_KEY_0: if (!shift) { cmd_body(); return; } break;
        case XKB_KEY_1: cmd_h1(); return;
        case XKB_KEY_2: cmd_h2(); return;
        case XKB_KEY_3: cmd_h3(); return;
        case XKB_KEY_5: cmd_sp15(); return;
        case XKB_KEY_l: if (!shift) { cmd_al_left(); return; } break;
        case XKB_KEY_e: if (!shift) { cmd_al_center(); return; } break;
        case XKB_KEY_r: cmd_al_right(); return;
        case XKB_KEY_j: cmd_al_justify(); return;
        }
    }
    if (global_shortcut(sym, mods)) return;
    if (ctrl) {
        switch (k) {
        case XKB_KEY_z: shift ? cmd_redo() : cmd_undo(); return;
        case XKB_KEY_y: cmd_redo(); return;
        case XKB_KEY_x: cmd_cut(); return;
        case XKB_KEY_c: case XKB_KEY_Insert: cmd_copy(); return;
        case XKB_KEY_v: cmd_paste(); return;
        case XKB_KEY_a: cmd_select_all(); return;
        case XKB_KEY_d: cmd_duplicate(); return;
        case XKB_KEY_k: if (shift) cmd_delete_line(); return;
        case XKB_KEY_l: cmd_select_line(); return;
        case XKB_KEY_j: cmd_join_lines(); return;
        case XKB_KEY_u: shift ? cmd_lower() : cmd_upper(); return;
        case XKB_KEY_slash: case XKB_KEY_KP_Divide: case XKB_KEY_numbersign: cmd_toggle_comment(); return;
        case XKB_KEY_bracketright: cmd_indent(); return;
        case XKB_KEY_bracketleft: cmd_outdent(); return;
        }
    }
    if (shift && !ctrl && sym == XKB_KEY_Insert) { cmd_paste(); return; }
    if (shift && !ctrl && sym == XKB_KEY_Delete) { cmd_cut(); return; }
    if (alt && !ctrl && (sym == XKB_KEY_Up || sym == XKB_KEY_Down)) {
        if (shift) { cmd_duplicate(); return; } /* duplicate line */
        move_lines(sym == XKB_KEY_Up ? -1 : 1);
        return;
    }

    if (d->docmode && !alt) {
        bool done = true;
        PG g;
        switch (sym) {
        case XKB_KEY_Up: case XKB_KEY_KP_Up:
            if (ctrl) dv_scroll(d, -dv_body_lh(d)); else dv_move_vert(d, -1, shift);
            break;
        case XKB_KEY_Down: case XKB_KEY_KP_Down:
            if (ctrl) dv_scroll(d, dv_body_lh(d)); else dv_move_vert(d, 1, shift);
            break;
        case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up:
            dv_scroll(d, -(A.r_text.h - dv_body_lh(d)));
            dv_move_vert(d, -MAX(1, A.vis_rows - 1), shift);
            break;
        case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down:
            dv_scroll(d, A.r_text.h - dv_body_lh(d));
            dv_move_vert(d, MAX(1, A.vis_rows - 1), shift);
            break;
        case XKB_KEY_Home: case XKB_KEY_KP_Home:
            if (ctrl) { done = false; break; }
            d->want_x = -1;
            para_geo(d, d->cur.line, &g);
            {
                int r = dv_row_of(d->cur.col), t = g_para.rows[r].s;
                if (r == 0) t = d->cur.col == g_para.prefix || g_para.kind == BK_BODY ? 0 : g_para.prefix;
                move_cursor(d, (Pos){d->cur.line, t}, shift);
            }
            break;
        case XKB_KEY_End: case XKB_KEY_KP_End:
            if (ctrl) { done = false; break; }
            d->want_x = -1;
            para_geo(d, d->cur.line, &g);
            {
                int r = dv_row_of(d->cur.col);
                int e = r + 1 < g_para.nrows ? utf8_prev(d->lines[d->cur.line].s, g_para.rows[r].e) : d->lines[d->cur.line].len;
                move_cursor(d, (Pos){d->cur.line, e}, shift);
            }
            break;
        case XKB_KEY_Return: case XKB_KEY_KP_Enter:
            done = !ctrl && dv_enter();
            if (!done && !ctrl) { insert_text("\n", 1, EK_OTHER); done = true; }
            break;
        default: done = false;
        }
        if (done) {
            if (shift && doc_has_sel(d)) set_primary_from_sel();
            return;
        }
    }
    bool vertical = sym == XKB_KEY_Up || sym == XKB_KEY_Down || sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down;
    if (!vertical) d->want_x = -1;
    Line *l = &d->lines[d->cur.line];
    switch (sym) {
    case XKB_KEY_Left: case XKB_KEY_KP_Left:
        if (!shift && doc_has_sel(d) && !ctrl) { Pos a, b; doc_sel(d, &a, &b); move_cursor(d, a, false); }
        else if (ctrl) move_cursor(d, word_left(d, d->cur), shift);
        else if (d->cur.col > 0) move_cursor(d, (Pos){d->cur.line, utf8_prev(l->s, d->cur.col)}, shift);
        else if (d->cur.line > 0) move_cursor(d, (Pos){d->cur.line - 1, d->lines[d->cur.line - 1].len}, shift);
        else move_cursor(d, d->cur, shift);
        break;
    case XKB_KEY_Right: case XKB_KEY_KP_Right:
        if (!shift && doc_has_sel(d) && !ctrl) { Pos a, b; doc_sel(d, &a, &b); move_cursor(d, b, false); }
        else if (ctrl) move_cursor(d, word_right(d, d->cur), shift);
        else if (d->cur.col < l->len) move_cursor(d, (Pos){d->cur.line, utf8_next(l->s, l->len, d->cur.col)}, shift);
        else if (d->cur.line + 1 < d->nlines) move_cursor(d, (Pos){d->cur.line + 1, 0}, shift);
        else move_cursor(d, d->cur, shift);
        break;
    case XKB_KEY_Up: case XKB_KEY_KP_Up:
        if (ctrl) { step_rows(d, &d->top_line, &d->top_row, -1); clamp_scroll(d); break; }
        move_vert(d, -1, shift);
        break;
    case XKB_KEY_Down: case XKB_KEY_KP_Down:
        if (ctrl) { step_rows(d, &d->top_line, &d->top_row, 1); clamp_scroll(d); break; }
        move_vert(d, 1, shift);
        break;
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up:
        step_rows(d, &d->top_line, &d->top_row, -(A.vis_rows - 1));
        move_vert(d, -(A.vis_rows - 1), shift);
        break;
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down:
        step_rows(d, &d->top_line, &d->top_row, A.vis_rows - 1);
        clamp_scroll(d);
        move_vert(d, A.vis_rows - 1, shift);
        break;
    case XKB_KEY_Home: case XKB_KEY_KP_Home:
        if (ctrl) move_cursor(d, (Pos){0, 0}, shift);
        else {
            int f = first_nonws(l);
            int row;
            col_to_x(d, d->cur.line, d->cur.col, &row);
            int rs = A.cfg.wrap ? g_rows[row] : 0;
            int target = rs > 0 ? (d->cur.col == rs ? 0 : rs) : (d->cur.col == f ? 0 : f);
            move_cursor(d, (Pos){d->cur.line, target}, shift);
        }
        break;
    case XKB_KEY_End: case XKB_KEY_KP_End:
        if (ctrl) move_cursor(d, (Pos){d->nlines - 1, d->lines[d->nlines - 1].len}, shift);
        else {
            int row, n = wrap_line(d, d->cur.line);
            col_to_x(d, d->cur.line, d->cur.col, &row);
            int e = row + 1 < n ? utf8_prev(l->s, g_rows[row + 1]) : l->len;
            if (d->cur.col == e) e = l->len;
            move_cursor(d, (Pos){d->cur.line, e}, shift);
        }
        break;
    case XKB_KEY_BackSpace: {
        if (doc_has_sel(d)) { begin(EK_DELETE); delete_sel(d); break; }
        if (!ctrl && !d->readonly && ac_backspace(d)) break;
        Pos p = d->cur;
        if (ctrl) p = word_left(d, d->cur);
        else if (p.col > 0) {
            int f = first_nonws(l);
            if (d->use_spaces && p.col <= f && p.col > 0 && l->s[p.col - 1] == ' ') {
                int w = d->tab_w, target = ((p.col - 1) / w) * w;
                p.col = MAX(target, p.col - w);
                while (p.col < d->cur.col && l->s[p.col] != ' ') p.col++;
            } else p.col = utf8_prev(l->s, p.col);
        } else if (p.line > 0) p = (Pos){p.line - 1, d->lines[p.line - 1].len};
        if (pos_cmp(p, d->cur)) {
            begin(EK_DELETE);
            doc_delete(d, p, d->cur);
            d->cur = d->anc = p;
        }
        break;
    }
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: {
        if (doc_has_sel(d)) { begin(EK_DELETE); delete_sel(d); break; }
        Pos p = d->cur;
        if (ctrl) p = word_right(d, d->cur);
        else if (p.col < l->len) p.col = utf8_next(l->s, l->len, p.col);
        else if (p.line + 1 < d->nlines) p = (Pos){p.line + 1, 0};
        if (pos_cmp(p, d->cur)) { begin(EK_DELETE); doc_delete(d, d->cur, p); d->anc = d->cur; }
        break;
    }
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: {
        if (ctrl) { /* open a line below (above with shift) without splitting */
            begin(EK_OTHER);
            if (shift) { d->cur = d->anc = (Pos){d->cur.line, 0}; }
            else { d->cur = d->anc = (Pos){d->cur.line, l->len}; }
        }
        char buf[512];
        int n = 0;
        buf[n++] = '\n';
        Line *cl = &d->lines[d->cur.line];
        int f = A.cfg.autoindent ? MIN(first_nonws(cl), d->cur.col) : 0;
        f = MIN(f, 200);
        memcpy(buf + n, cl->s, f);
        n += f;
        if (ctrl && shift) {
            begin(EK_OTHER);
            /* insert the newline before the current line's content */
            char *t = xmalloc(f + 1);
            memcpy(t, cl->s, f);
            t[f] = '\n';
            doc_insert(d, (Pos){d->cur.line, 0}, t, f + 1);
            free(t);
            d->cur = d->anc = (Pos){d->cur.line, f};
            ensure_visible(d);
            break;
        }
        int pc = d->cur.col;
        while (pc > 0 && (cl->s[pc - 1] == ' ' || cl->s[pc - 1] == '\t')) pc--;
        char prev = pc > 0 ? cl->s[pc - 1] : 0;
        char next = d->cur.col < cl->len ? cl->s[d->cur.col] : 0;
        bool opener = prev == '{' || prev == '[' || prev == '(' ||
                      (prev == ':' && d->lang && (!strcmp(lang_name(d->lang), "Python") || !strcmp(lang_name(d->lang), "YAML")));
        if (A.cfg.autoindent && opener) {
            int un;
            const char *u = indent_unit(d, &un);
            memcpy(buf + n, u, un);
            n += un;
            if ((prev == '{' && next == '}') || (prev == '[' && next == ']') || (prev == '(' && next == ')')) {
                begin(EK_OTHER);
                if (doc_has_sel(d)) delete_sel(d);
                Pos mid = doc_insert(d, d->cur, buf, n);
                char tail[256];
                int tn = 0;
                tail[tn++] = '\n';
                memcpy(tail + tn, cl->s, MIN(f, 200));
                tn += MIN(f, 200);
                doc_insert(d, mid, tail, tn);
                d->cur = d->anc = mid;
                ensure_visible(d);
                break;
            }
        }
        insert_text(buf, n, EK_OTHER);
        break;
    }
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab:
        if (shift || sym == XKB_KEY_ISO_Left_Tab) { cmd_outdent(); break; }
        if (doc_has_sel(d) && d->cur.line != d->anc.line) { cmd_indent(); break; }
        if (d->use_spaces) {
            int x = (measure26(d, d->lines[d->cur.line].s, 0, d->cur.col) + A.sp26 / 2) / A.sp26;
            int n = d->tab_w - (x % d->tab_w);
            char sp[17];
            memset(sp, ' ', 16);
            insert_text(sp, MIN(n, 16), EK_TYPE);
        } else insert_text("\t", 1, EK_TYPE);
        break;
    case XKB_KEY_Escape:
        if (A.find_open) { A.find_open = false; break; }
        if (doc_has_sel(d)) { d->anc = d->cur; }
        break;
    default:
        if (!ctrl && utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
            int n = (int)strlen(utf8);
            if (n == 1 && !d->readonly && ac_type(d, utf8[0])) { d->want_x = -1; ensure_visible(d); return; }
            insert_text(utf8, n, utf8[0] == ' ' ? EK_OTHER : EK_TYPE);
        }
        return;
    }
    if (shift && doc_has_sel(d)) set_primary_from_sel();
}

static void find_key(uint32_t sym, uint32_t mods, const char *utf8) {
    bool shift = mods & MOD_SHIFT, alt = mods & MOD_ALT;
    uint32_t k = norm(sym);
    if (sym == XKB_KEY_Escape) { A.find_open = false; A.mode = M_EDIT; return; }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (A.find_focus == 1) { if (mods & MOD_CTRL) replace_all(); else replace_one(); }
        else find_next(shift ? -1 : 1);
        return;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
        if (A.replace_open) { A.find_focus ^= 1; Input *in = A.find_focus ? &A.rep : &A.fin; in->all = in->len > 0; in->cur = in->len; }
        return;
    }
    if (sym == XKB_KEY_Down) { find_next(1); return; }
    if (sym == XKB_KEY_Up) { find_next(-1); return; }
    if (alt && k == XKB_KEY_c) { A.find_case = !A.find_case; return; }
    if (alt && k == XKB_KEY_w) { A.find_word = !A.find_word; return; }
    if (alt && k == XKB_KEY_r) { replace_one(); return; }
    if (alt && k == XKB_KEY_a) { replace_all(); return; }
    Input *in = A.find_focus ? &A.rep : &A.fin;
    bool handled;
    bool changed = input_key(in, sym, mods, utf8, &handled);
    if (handled) {
        if (changed && A.find_focus == 0) {
            Doc *d = D();
            if (A.fin.len && !find_from(d, A.find_origin, 1, true)) { d->cur = d->anc = A.find_origin; }
            else if (!A.fin.len) { d->cur = d->anc = A.find_origin; }
        }
        return;
    }
    if (global_shortcut(sym, mods)) return;
    if (mods & MOD_CTRL) { A.mode = M_EDIT; editor_key(sym, mods, utf8); }
}

static void palette_key(uint32_t sym, uint32_t mods, const char *utf8) {
    switch (sym) {
    case XKB_KEY_Escape: A.mode = M_EDIT; return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: pal_run(A.pal_sel); return;
    case XKB_KEY_Up: if (A.pal_n) A.pal_sel = (A.pal_sel + A.pal_n - 1) % A.pal_n; return;
    case XKB_KEY_Down: case XKB_KEY_Tab: if (A.pal_n) A.pal_sel = (A.pal_sel + 1) % A.pal_n; return;
    case XKB_KEY_Page_Up: A.pal_sel = MAX(0, A.pal_sel - 8); return;
    case XKB_KEY_Page_Down: A.pal_sel = MIN(A.pal_n - 1, A.pal_sel + 8); return;
    }
    if ((mods & MOD_CTRL) && (mods & MOD_SHIFT) && norm(sym) == XKB_KEY_p) { A.mode = M_EDIT; return; }
    bool handled;
    if (input_key(&A.pin, sym, mods, utf8, &handled)) pal_build();
}

static void goto_key(uint32_t sym, uint32_t mods, const char *utf8) {
    if (sym == XKB_KEY_Escape) { A.mode = M_EDIT; return; }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        Doc *d = D();
        int line = 0, col = 0;
        if (sscanf(A.gin.s, "%d:%d", &line, &col) >= 1 && line != 0) {
            if (line < 0) line = d->nlines + line + 1;
            Pos p = doc_clamp(d, (Pos){line - 1, col > 0 ? col - 1 : 0});
            d->cur = d->anc = p;
            center_cursor(d);
        }
        A.mode = M_EDIT;
        return;
    }
    bool handled;
    if (utf8 && *utf8 && !(mods & MOD_CTRL) && !strchr("0123456789:-", utf8[0])) return;
    input_key(&A.gin, sym, mods, utf8, &handled);
}

static void dialog_choose(int b) {
    int i = A.dlg_arg;
    if (A.dlg_kind == DLG_CLOSE) {
        if (b == 0) {
            A.mode = M_EDIT;
            A.docs[i]->close_after_save = true;
            save_index(i);
        } else if (b == 1) {
            A.mode = M_EDIT;
            really_close(i);
        } else {
            close_modal();
        }
    } else if (A.dlg_kind == DLG_OVERWRITE) {
        if (b == 0) { A.mode = M_EDIT; if (A.br_mode == BR_EXPORT) do_export(i, A.dlg_path); else do_save(i, A.dlg_path); }
        else { A.mode = M_BROWSER; }
    }
    redraw();
}

static void dialog_key(uint32_t sym, uint32_t mods, const char *utf8) {
    (void)mods;
    switch (sym) {
    case XKB_KEY_Escape: if (A.dlg_kind == DLG_OVERWRITE) A.mode = M_BROWSER; else close_modal(); return;
    case XKB_KEY_Left: case XKB_KEY_ISO_Left_Tab: A.dlg_sel = (A.dlg_sel + A.dlg_nbtn - 1) % A.dlg_nbtn; return;
    case XKB_KEY_Right: case XKB_KEY_Tab: A.dlg_sel = (A.dlg_sel + 1) % A.dlg_nbtn; return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: dialog_choose(A.dlg_sel); return;
    }
    if (utf8 && A.dlg_kind == DLG_CLOSE) {
        char ch = tolower((unsigned char)utf8[0]);
        if (ch == 's') dialog_choose(0);
        else if (ch == 'd' || ch == 'n') dialog_choose(1);
        else if (ch == 'c') dialog_choose(2);
    }
}

static void browser_complete(void) {
    if (!A.br_fn) return;
    Entry *e = &A.br_ent[A.br_f[A.br_sel]];
    if (e->dir) {
        char p[PATH_MAX];
        br_path_of(e->name, p, sizeof p);
        br_load(p);
        A.bin = (Input){0};
        br_filter();
    } else {
        input_set(&A.bin, e->name);
        A.bin.all = false;
    }
}

static void browser_key(uint32_t sym, uint32_t mods, const char *utf8) {
    switch (sym) {
    case XKB_KEY_Escape: close_modal(); return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: br_accept(); return;
    case XKB_KEY_Up: A.br_nav = true; if (A.br_fn) A.br_sel = (A.br_sel + A.br_fn - 1) % A.br_fn; if (A.br_mode != BR_OPEN) A.bin.all = true; return;
    case XKB_KEY_Down: A.br_nav = true; if (A.br_fn) A.br_sel = (A.br_sel + 1) % A.br_fn; if (A.br_mode != BR_OPEN) A.bin.all = true; return;
    case XKB_KEY_Page_Up: A.br_nav = true; A.br_sel = MAX(0, A.br_sel - 10); return;
    case XKB_KEY_Page_Down: A.br_nav = true; A.br_sel = MAX(0, MIN(A.br_fn - 1, A.br_sel + 10)); return;
    case XKB_KEY_Tab: browser_complete(); return;
    case XKB_KEY_BackSpace:
        if (!A.bin.len || (A.bin.all && A.br_mode == BR_OPEN)) { if (!A.bin.len) { br_up(); return; } }
        break;
    }
    if ((mods & MOD_CTRL) && norm(sym) == XKB_KEY_h) { A.br_hidden = !A.br_hidden; br_filter(); return; }
    if ((mods & MOD_CTRL) && norm(sym) == XKB_KEY_l) { input_set(&A.bin, A.br_dir); A.bin.all = false; return; }
    bool handled;
    if (input_key(&A.bin, sym, mods, utf8, &handled)) br_filter();
}

void app_key(uint32_t sym, uint32_t mods, const char *utf8) {
    ScrollSnap ss = snap_scroll();
    app_key_inner(sym, mods, utf8);
    scroll_commit(ss);
    redraw();
}

static void app_key_inner(uint32_t sym, uint32_t mods, const char *utf8) {
    activity();
    if ((mods & MOD_CTRL) && norm(sym) == XKB_KEY_q && A.mode != M_DIALOG) { A.mode = M_EDIT; cmd_quit(); redraw(); return; }
    switch (A.mode) {
    case M_DIALOG: dialog_key(sym, mods, utf8); break;
    case M_PALETTE: palette_key(sym, mods, utf8); break;
    case M_BROWSER: browser_key(sym, mods, utf8); break;
    case M_GOTO: goto_key(sym, mods, utf8); break;
    case M_FIND: find_key(sym, mods, utf8); break;
    case M_SPELL: spell_key(sym, mods, utf8); break;
    case M_MENU: menu_key(sym, mods, utf8); break;
    default: editor_key(sym, mods, utf8); break;
    }
    redraw();
}

/* ================================================================== pointer */
static void update_hover(void) {
    Zone *z = zone_at(A.mx, A.my);
    int act = z ? z->act : A_NONE, arg = z ? z->arg : 0;
    if (act != A.hover_act || arg != A.hover_arg) {
        A.phv_act = A.hover_act;
        A.phv_arg = A.hover_arg;
        A.hv_t0 = plat_now_ms();
        A.hover_act = act;
        A.hover_arg = arg;
        redraw();
    }
    int cur = CUR_DEFAULT;
    if (act == A_TEXT && (A.mode == M_EDIT || A.mode == M_FIND)) cur = rect_hit(A.r_gutter, (int)A.mx, (int)A.my) ? CUR_DEFAULT : CUR_TEXT;
    else if (act == A_FIND_FIELD || act == A_REPL_FIELD) cur = CUR_TEXT;
    else if (act != A_NONE && act != A_TITLEBAR && act != A_MODAL_BG && act != A_PANEL && act != A_SCROLLBAR) cur = CUR_POINTER;
    plat_set_cursor(cur);
}

static void drag_to(Pos p) {
    Doc *d = D();
    if (A.drag_mode == 2) {
        Pos a, b;
        word_at(d, p, &a, &b);
        if (pos_cmp(p, A.drag_a) < 0) { d->anc = A.drag_b; d->cur = a; }
        else { d->anc = A.drag_a; d->cur = pos_cmp(b, A.drag_b) > 0 ? b : A.drag_b; }
    } else if (A.drag_mode == 3) {
        Pos ls = {p.line, 0}, le = p.line + 1 < d->nlines ? (Pos){p.line + 1, 0} : (Pos){p.line, d->lines[p.line].len};
        if (pos_cmp(ls, A.drag_a) < 0) { d->anc = A.drag_b; d->cur = ls; }
        else { d->anc = A.drag_a; d->cur = le; }
    } else {
        d->cur = p;
    }
    d->want_x = -1;
}

void app_motion(double x, double y) {
    A.mx = x;
    A.my = y;
    if (A.drag_sb) {
        Doc *d = D();
        int span = A.r_sb_track.h - A.r_sb_thumb.h;
        int maxtop = MAX(1, d->nlines - 1 - A.vis_rows / 3);
        double f = span > 0 ? (y - A.drag_sb_off - A.r_sb_track.y) / span : 0;
        d->top_line = CLAMP((int)lround(f * maxtop), 0, d->nlines - 1);
        d->top_row = 0;
        d->top_px = 0;
        clamp_scroll(d);
        redraw();
        return;
    }
    if (A.drag_text) {
        drag_to(pos_at(x, y));
        bool outside = y < A.r_text.y || y > A.r_text.y + A.r_text.h;
        if (outside) plat_timer(TIMER_AUTOSCROLL, 30, 30);
        else {
            Doc *d = D();
            if (!A.cfg.wrap) {
                if (x < A.r_text.x && d->scroll_x > 0) d->scroll_x = MAX(0, d->scroll_x - A.sp * 2);
                else if (x > A.r_text.x + A.r_text.w) d->scroll_x += A.sp * 2;
            }
        }
        redraw();
        return;
    }
    update_hover();
}

void app_pointer_leave(void) {
    if (A.hover_act != A_NONE) {
        A.phv_act = A.hover_act;
        A.phv_arg = A.hover_arg;
        A.hv_t0 = plat_now_ms();
        A.hover_act = A_NONE;
        A.hover_arg = 0;
        redraw();
    }
}

static void palette_pos_click(int idx) { A.pal_sel = idx; pal_run(idx); }

static void app_button_inner(int button, bool down, double x, double y, uint32_t mods, int clicks);
void app_button(int button, bool down, double x, double y, uint32_t mods, int clicks) {
    ScrollSnap ss = snap_scroll();
    app_button_inner(button, down, x, y, mods, clicks);
    if (!A.drag_sb && !A.drag_text) scroll_commit(ss);
}

static void app_button_inner(int button, bool down, double x, double y, uint32_t mods, int clicks) {
    A.mx = x;
    A.my = y;
    if (!down) {
        if (button == 1) {
            if (A.drag_text && doc_has_sel(D())) set_primary_from_sel();
            A.drag_text = false;
            A.drag_sb = false;
            plat_timer(TIMER_AUTOSCROLL, 0, 0);
            redraw();
        }
        return;
    }
    activity();
    Zone *z = zone_at(x, y);
    if (!z) return;
    int act = z->act, arg = z->arg;
    Doc *d = D();
    if (button == 2) {
        if (act == A_TAB) close_tab(arg);
        else if (act == A_TEXT && (A.mode == M_EDIT || A.mode == M_FIND)) {
            char *t = plat_get_clipboard(CLIP_PRIMARY);
            if (t) {
                A.mode = M_EDIT;
                d->cur = d->anc = pos_at(x, y);
                int n;
                normalize_eol(t, &n);
                insert_text(t, n, EK_OTHER);
                free(t);
            }
        } else if (act == A_FIND_FIELD || act == A_REPL_FIELD) {
            char *t = plat_get_clipboard(CLIP_PRIMARY);
            if (t) { input_insert(act == A_FIND_FIELD ? &A.fin : &A.rep, t, (int)strlen(t)); free(t); }
        }
        redraw();
        return;
    }
    if (button == 3) {
        if (act == A_TEXT && (A.mode == M_EDIT || A.mode == M_FIND)) {
            int ws, we;
            Pos p = pos_at(x, y);
            if (spell_word_at(d, p, &ws, &we)) { open_spell_menu(p.line, ws, we, (int)x, (int)y); return; }
            cmd_palette();
        }
        return;
    }
    switch (act) {
    case A_TEXT: {
        if (A.mode != M_EDIT && A.mode != M_FIND) break;
        A.mode = M_EDIT;
        Pos p = pos_at(x, y);
        bool gutter = rect_hit(A.r_gutter, (int)x, (int)y);
        if (gutter) clicks = 3;
        if (clicks == 1) {
            if (mods & MOD_SHIFT) d->cur = p; else d->cur = d->anc = p;
            A.drag_mode = 1;
        } else if (clicks == 2) {
            word_at(d, p, &A.drag_a, &A.drag_b);
            d->anc = A.drag_a;
            d->cur = A.drag_b;
            A.drag_mode = 2;
        } else {
            A.drag_a = (Pos){p.line, 0};
            A.drag_b = p.line + 1 < d->nlines ? (Pos){p.line + 1, 0} : (Pos){p.line, d->lines[p.line].len};
            d->anc = A.drag_a;
            d->cur = A.drag_b;
            A.drag_mode = 3;
        }
        d->want_x = -1;
        A.drag_text = true;
        break;
    }
    case A_SCROLLBAR:
        if (rect_hit(A.r_sb_thumb, (int)x, (int)y)) A.drag_sb_off = (int)y - A.r_sb_thumb.y;
        else A.drag_sb_off = A.r_sb_thumb.h / 2;
        A.drag_sb = true;
        app_motion(x, y);
        break;
    case A_TAB: activate(arg); A.mode = A.mode == M_FIND ? M_FIND : M_EDIT; break;
    case A_TAB_CLOSE: close_tab(arg); break;
    case A_NEWTAB: cmd_new(); break;
    case A_MENU: if (A.mode == M_PALETTE) A.mode = M_EDIT; else cmd_palette(); break;
    case A_WIN_MIN: plat_minimize(); break;
    case A_WIN_MAX: plat_toggle_maximize(); break;
    case A_WIN_CLOSE: app_close_request(); break;
    case A_TITLEBAR: if (clicks == 2) plat_toggle_maximize(); else plat_move(); break;
    case A_FIND_FIELD: A.mode = M_FIND; A.find_focus = 0; A.fin.all = false; break;
    case A_REPL_FIELD: A.mode = M_FIND; A.find_focus = 1; A.rep.all = false; break;
    case A_FIND_CASE: A.find_case = !A.find_case; break;
    case A_FIND_WORD: A.find_word = !A.find_word; break;
    case A_FIND_PREV: find_next(-1); break;
    case A_FIND_NEXT: find_next(1); break;
    case A_FIND_CLOSE: A.find_open = false; A.mode = M_EDIT; break;
    case A_FIND_TOGGLE: A.replace_open = !A.replace_open; break;
    case A_REPL_ONE: replace_one(); break;
    case A_REPL_ALL: replace_all(); break;
    case A_PAL_ITEM: palette_pos_click(arg); break;
    case A_DLG_BTN: dialog_choose(arg); break;
    case A_SPELL_ITEM: spell_apply(arg); break;
    case A_MENU_ITEM: menu_apply(arg); break;
    case A_TB_STYLE: open_menu(0, (int)x - S(20), A.r_tool.y + A.r_tool.h + S(4)); break;
    case A_TB_SPACING: open_menu(1, (int)x - S(20), A.r_tool.y + A.r_tool.h + S(4)); break;
    case A_TB_FMT: if (arg == ST_B) cmd_bold(); else if (arg == ST_I) cmd_italic(); else if (arg == ST_U) cmd_underline(); else cmd_strike(); break;
    case A_TB_ALIGN: set_align(arg); break;
    case A_TB_LIST: set_block(arg); break;
    case A_TB_INDENT: cmd_indent_first(); break;
    case A_ST_MODE: cmd_docmode(); break;
    case A_BR_ITEM:
        A.br_sel = arg;
        A.br_nav = true;
        if (clicks >= 2) br_accept();
        else if (A.br_mode != BR_OPEN && !A.br_ent[A.br_f[arg]].dir) { input_set(&A.bin, A.br_ent[A.br_f[arg]].name); A.bin.all = false; }
        break;
    case A_BR_UP: br_up(); break;
    case A_BR_HIDDEN: A.br_hidden = !A.br_hidden; br_filter(); break;
    case A_MODAL_BG:
        if (A.mode == M_SPELL || A.mode == M_MENU) A.mode = M_EDIT;
        else if (A.mode == M_DIALOG && A.dlg_kind == DLG_OVERWRITE) A.mode = M_BROWSER;
        else if (A.mode != M_DIALOG) close_modal();
        break;
    case A_ST_LANG: cmd_palette(); input_set(&A.pin, "Language: "); A.pin.all = false; pal_build(); break;
    case A_ST_EOL: cmd_eol(); break;
    case A_ST_INDENT: cmd_indent_mode(); break;
    case A_ST_WRAP: cmd_wrap(); break;
    case A_ST_SPELL: cmd_spell_toggle(); break;
    case A_ST_POS: cmd_goto(); break;
    case A_ST_DISK: if (!doc_modified(d)) reload_doc(d); else toast(true, "Unsaved changes — use “Reload from Disk” to discard them"); break;
    }
    redraw();
}

static void app_scroll_inner(double dx, double dy, bool discrete);
void app_scroll(double dx, double dy, bool discrete) {
    ScrollSnap ss = snap_scroll();
    app_scroll_inner(dx, dy, discrete);
    if (discrete) scroll_commit(ss); /* touchpads already scroll smoothly */
}

static void app_scroll_inner(double dx, double dy, bool discrete) {
    Doc *d = D();
    activity();
    if (plat_mods() & MOD_CTRL) {
        static double acc;
        acc += discrete ? dy : dy / S(40);
        int z = (int)acc;
        acc -= z;
        if (z) zoom_by(-z);
        return;
    }
    if (A.mode == M_PALETTE) {
        int s = dy > 0 ? 1 : dy < 0 ? -1 : 0;
        A.pal_sel = CLAMP(A.pal_sel + s, 0, MAX(0, A.pal_n - 1));
        redraw();
        return;
    }
    if (A.mode == M_BROWSER) {
        int s = discrete ? (int)dy * 3 : (int)(dy / S(10));
        A.br_sel = CLAMP(A.br_sel + s, 0, MAX(0, A.br_fn - 1));
        redraw();
        return;
    }
    if (rect_hit(A.r_top, (int)A.mx, (int)A.my)) {
        A.tabs_scroll += (int)((discrete ? dy * 40 : dy) * A.scale + (discrete ? dx * 40 : dx));
        if (discrete && dy) { activate(CLAMP(A.active + (dy > 0 ? 1 : -1), 0, A.ndocs - 1)); }
        redraw();
        return;
    }
    if (A.mode != M_EDIT && A.mode != M_FIND && A.mode != M_GOTO) return;
    if (d->docmode) {
        dv_scroll(d, discrete ? (int)dy * 3 * dv_body_lh(d) : (int)dy);
        redraw();
        return;
    }
    if (discrete) {
        if (dy) { step_rows(d, &d->top_line, &d->top_row, (int)dy * 3); }
        if (dx && !A.cfg.wrap) d->scroll_x = MAX(0, d->scroll_x + (int)dx * A.sp * 6);
    } else {
        A.scroll_acc += dy;
        int rows = (int)(A.scroll_acc / A.lh);
        A.scroll_acc -= rows * A.lh;
        if (rows) step_rows(d, &d->top_line, &d->top_row, rows);
        if (dx && !A.cfg.wrap) d->scroll_x = MAX(0, d->scroll_x + (int)dx);
    }
    clamp_scroll(d);
    redraw();
}

void app_focus(bool in) {
    A.focused = in;
    if (in) { activity(); check_disk(); }
    else plat_timer(TIMER_BLINK, 0, 0);
    redraw();
}

void app_close_request(void) {
    A.mode = M_EDIT;
    cmd_quit();
    redraw();
}

void app_drop_files(char **paths, int n) {
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            A.mode = M_BROWSER;
            A.br_mode = BR_OPEN;
            A.bin = (Input){0};
            br_load(paths[i]);
        } else {
            open_path(paths[i], 0);
        }
    }
    redraw();
}

void app_resize(int pw, int ph, double scale) {
    bool rescale = fabs(scale - A.scale) > 1e-6 || !A.mono;
    A.W = pw;
    A.H = ph;
    A.scale = scale;
    if (rescale) load_fonts();
    layout();
    for (int i = 0; i < A.ndocs; i++) clamp_scroll(A.docs[i]);
}

/* ================================================================== main */
static void usage(void) {
    printf("Noctedit %s — a tiny Noctalia-styled text editor for Wayland\n\n"
           "usage: noctedit [+LINE] [FILE...]\n\n"
           "  +LINE        jump to LINE in the first file (also FILE:LINE)\n"
           "  -h, --help   show this help\n"
           "  -v, --version\n\n"
           "Press Ctrl+Shift+P (or F1) inside the editor for every command.\n",
           NOCTEDIT_VERSION);
}

int main(int argc, char **argv) {
    int line = 0;
    char **files = xmalloc((argc + 1) * sizeof(char *));
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) { printf("Noctedit %s\n", NOCTEDIT_VERSION); return 0; }
        if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1])) { line = atoi(argv[i] + 1); continue; }
        if (!strcmp(argv[i], "--")) { for (i++; i < argc; i++) files[nfiles++] = argv[i]; break; }
        files[nfiles++] = argv[i];
    }
    config_load(&A.cfg);
    if (A.cfg.spell_lang[0]) spell_set_lang(A.cfg.spell_lang);
    palette_load(&A.pal);
    derive_colors();
    A.scale = 1;
    A.focused = true;
    A.caret_on = true;
    A.hover_act = A_NONE;

    if (!plat_init("noctedit", "Noctedit", 1100, 720)) return 1;
    load_fonts();
    A.W = 1100;
    A.H = 720;
    layout();

    for (int i = 0; i < nfiles; i++) {
        char *f = files[i];
        int fl = i == 0 ? line : 0;
        struct stat st;
        char *colon = strrchr(f, ':');
        if (colon && colon[1] && isdigit((unsigned char)colon[1]) && stat(f, &st) != 0) {
            *colon = 0;
            fl = atoi(colon + 1);
        }
        open_path(f, fl);
    }
    free(files);
    if (!nfiles && A.cfg.restore_session) session_restore();
    if (!A.ndocs) add_doc(new_doc());
    A.toast[0] = 0; /* errors while opening stay visible below */

    /* live-reload the Noctalia palette */
    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0) {
        const char *file;
        for (int i = 0; palette_watch_dir(i, &file); i++) {
            const char *dir = palette_watch_dir(i, &file);
            if (i == 0) mkdir(dir, 0755);
            inotify_add_watch(ino, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        }
        plat_watch_fd(ino, theme_watch_cb, NULL);
    }
    activity();
    int rc = plat_run();
    for (int i = 0; i < A.ndocs; i++) doc_free(A.docs[i]);
    free(A.docs);
    font_free(A.mono);
    font_free(A.ui);
    font_free(A.ui_big);
    font_free(A.icons);
    fonts_shutdown();
    return rc;
}
