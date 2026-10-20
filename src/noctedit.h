/* noctedit — a tiny, Noctalia-styled text editor for Wayland.
 * Shared declarations for all translation units. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOCTEDIT_VERSION "1.0"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---------------------------------------------------------------- draw.c */
typedef uint32_t Color; /* 0xAARRGGBB */
typedef struct { int x, y, w, h; } Rect;

typedef struct {
    uint32_t *px;
    int w, h, stride;      /* stride in pixels */
    int cx0, cy0, cx1, cy1; /* clip, half-open */
} Canvas;

static inline bool rect_hit(Rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
Rect rect_union(Rect a, Rect b);
Rect rect_intersect(Rect a, Rect b);

void cv_clip(Canvas *c, Rect r);   /* sets clip = r ∩ canvas */
Rect cv_get_clip(const Canvas *c);
void cv_fill(Canvas *c, int x, int y, int w, int h, Color col);
void cv_rrect(Canvas *c, int x, int y, int w, int h, int r, Color col);
void cv_rrect_border(Canvas *c, int x, int y, int w, int h, int r, int bw, Color border, Color fill);
Color color_mix(Color a, Color b, float t);
Color color_alpha(Color c, int a);

typedef struct Font Font;
Font *font_open(const char *fc_pattern, int px);   /* fontconfig pattern */
Font *font_open_file(const char *path, int px);
void font_free(Font *f);
int font_ascent(const Font *f);
int font_height(const Font *f);
int font_adv(Font *f, uint32_t cp);   /* rounded px */
int font_adv26(Font *f, uint32_t cp); /* 26.6 fixed point */
int draw_glyph26(Canvas *c, Font *f, int x26, int baseline, uint32_t cp, Color col);
int draw_glyph(Canvas *c, Font *f, int x, int baseline, uint32_t cp, Color col);
int draw_text(Canvas *c, Font *f, int x, int baseline, const char *s, int n, Color col);
int text_width(Font *f, const char *s, int n);
void fonts_shutdown(void);
void fonts_trim(void);         /* drop fontconfig state until next needed */

uint32_t utf8_dec(const char *s, int n, int *len);
int utf8_enc(uint32_t cp, char *out);
int utf8_prev(const char *s, int i); /* index of previous char start */
int utf8_next(const char *s, int n, int i);

/* ---------------------------------------------------------------- syntax.c */
enum {
    T_TEXT, T_KW, T_TYPE, T_STR, T_NUM, T_COMMENT, T_PRE, T_FUNC,
    T_PUNCT, T_CONST, T_HEAD, T_COUNT
};
typedef struct Lang Lang;
const Lang *lang_detect(const char *path, const char *first_line, int n);
const Lang *lang_at(int i); /* NULL past end; index 0 is Plain Text */
const char *lang_name(const Lang *l);
const char *lang_comment(const Lang *l);
uint8_t hl_line(const Lang *l, const char *s, int n, uint8_t state, uint8_t *out);

/* ---------------------------------------------------------------- doc.c */
typedef struct { int line, col; } Pos; /* col is a byte offset */
typedef struct { char *s; int len, cap; } Line; /* cap==0: borrowed from load block */

typedef struct {
    uint8_t type; /* 0 insert, 1 delete */
    Pos a, b;
    char *text; int n;
    Pos cur, anc; /* selection before the op */
    uint32_t group;
} UndoRec;

typedef struct Doc {
    Line *lines; int nlines, lcap;
    char *block;              /* original file contents (lines borrow from it) */
    char *path;
    int untitled_id;
    bool crlf, use_spaces;
    int tab_w;
    Pos cur, anc;
    int want_x;               /* sticky x for vertical motion, -1 = unset */
    int top_line, top_row, scroll_x;
    UndoRec *undo; int nundo, undo_cap, undo_pos, saved_pos;
    uint32_t group;
    uint64_t last_edit_ms; int last_kind;
    uint64_t version;
    uint8_t *hl; int hl_cap, hl_valid;
    const Lang *lang;
    long long mtime_ns;
    bool on_disk_changed;
    bool close_after_save;
    bool readonly, is_pdf;
    /* document (word-processor) view */
    bool docmode, dbl, dbl_tail;
    uint8_t align, para_sp, indent; /* para_sp in pt at 12pt body; indent: first-line indent on/off */
    uint16_t spacing;               /* line spacing x100 */
    int top_px;
} Doc;

static inline int pos_cmp(Pos a, Pos b) {
    return a.line != b.line ? (a.line < b.line ? -1 : 1) : (a.col < b.col ? -1 : a.col > b.col);
}
Doc *doc_new(void);
void doc_free(Doc *d);
bool doc_load(Doc *d, const char *path, char *err, int errn);
void doc_set_text(Doc *d, char *buf, size_t size); /* takes ownership */
bool doc_save(Doc *d, const char *path, char *err, int errn);
bool doc_modified(const Doc *d);
const char *doc_name(const Doc *d);
char *doc_text(Doc *d, Pos a, Pos b, int *n);
Pos doc_clamp(Doc *d, Pos p);
bool doc_has_sel(const Doc *d);
void doc_sel(const Doc *d, Pos *a, Pos *b);

enum { EK_OTHER, EK_TYPE, EK_DELETE };
void doc_begin(Doc *d, int kind, uint64_t now_ms); /* start (or continue) an undo group */
Pos doc_insert(Doc *d, Pos at, const char *s, int n);
void doc_delete(Doc *d, Pos a, Pos b);
bool doc_undo(Doc *d);
bool doc_redo(Doc *d);
void doc_detect_indent(Doc *d);
long long file_mtime_ns(const char *path);

/* ---------------------------------------------------------------- docview.c */
enum { ST_B = 1, ST_I = 2, ST_U = 4, ST_S = 8, ST_CODE = 16, ST_MARK = 32, ST_HIDE = 64, ST_LINK = 128, ST_LIT = 256 };
enum { BK_BODY, BK_H1, BK_H2, BK_H3, BK_BULLET, BK_NUMBER, BK_QUOTE, BK_RULE };
enum { FN_REG = 0, FN_B = 1, FN_I = 2, FN_BI = 3, FN_H1, FN_H2, FN_H3, FN_CODE, FN_COUNT };
enum { AL_LEFT, AL_CENTER, AL_RIGHT, AL_JUSTIFY };
typedef struct { int s, e, w, nsp; } DRow;
typedef struct {
    uint8_t kind;
    int level, number, prefix;
    uint16_t *st; /* per-byte style flags (caller-owned, >= line length) */
    DRow *rows;
    int nrows, rcap;
} Para;
typedef struct { int (*adv)(void *ctx, int font, uint32_t cp); void *ctx; } Measurer;
void para_parse(const char *s, int n, Para *p, bool show_markup);
int para_font(const Para *p, int i);
int para_measure(const char *s, int n, const Para *p, int a, int b, const Measurer *m);
void para_wrap(const char *s, int n, Para *p, int w_first, int w_rest, const Measurer *m);
double para_row_x(const char *s, int n, const Para *p, const DRow *r, int i, double jx, const Measurer *m);
bool doc_detect_double(const Doc *d);
void doc_collapse_double(Doc *d);

/* ---------------------------------------------------------------- spell.c */
bool spell_ready(void);
const char *spell_status(void);
void spell_set_lang(const char *lang);
int spell_check(const char *w, int n); /* 1 ok, 0 misspelled, -1 unavailable */
int spell_suggest(const char *w, int n, char out[][64], int max);
void spell_add(const char *w, int n);
void spell_ignore(const char *w, int n);
void spell_unload(void);       /* free the dictionary; reloads (~15 ms) on demand */

/* ---------------------------------------------------------------- pdf.c */
bool pdf_export(Doc *d, const char *path, const char *font_pattern, bool line_numbers, char *err, int errn);
bool pdf_export_doc(Doc *d, const char *path, const char *doc_font, const char *mono_font, char *err, int errn);
bool is_pdf_file(const char *path);
char *pdf_extract_text(const char *path, size_t *len, char *err, int errn);

/* ---------------------------------------------------------------- theme.c */
typedef struct {
    Color primary, on_primary, secondary, tertiary, error;
    Color surface, surface_container, surface_variant, on_surface, on_surface_variant, outline;
    bool light;
    char source[64];
} Palette;
void palette_load(Palette *p);
const char *palette_watch_dir(int i, const char **file); /* dirs to watch for live reload */

typedef struct {
    char font[128], ui_font[128];
    int font_size, ui_font_size, tab_w;
    bool use_spaces, wrap, line_numbers, highlight_line, blink, autoindent, whitespace, spellcheck, pdf_line_numbers;
    char spell_lang[32];
    char doc_font[128];
    int doc_font_size, page_width;
    int doc_default; /* 0 never, 1 prose files + new files (auto), 2 always for text */
    bool animations, smooth_caret, restore_session, auto_close, indent_guides;
    float anim_speed;
    int radius;
} Config;
void config_load(Config *c);

/* ---------------------------------------------------------------- wl.c (platform) */
enum { MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_SUPER = 8 };
enum { CUR_DEFAULT, CUR_TEXT, CUR_POINTER };
enum { TIMER_BLINK, TIMER_TOAST, TIMER_AUTOSCROLL, TIMER_IDLE, TIMER_COUNT };
enum { CLIP_CLIPBOARD, CLIP_PRIMARY };

bool plat_init(const char *app_id, const char *title, int w, int h);
int plat_run(void);
void plat_quit(void);
void plat_damage(Rect r);       /* in buffer pixels */
void plat_redraw(void);         /* full damage */
void plat_set_title(const char *t);
void plat_set_cursor(int shape);
void plat_set_clipboard(int which, const char *s, int n);
char *plat_get_clipboard(int which); /* malloc'd, NULL if empty */
void plat_timer(int id, int first_ms, int repeat_ms); /* first_ms 0 disarms */
void plat_move(void);
void plat_toggle_maximize(void);
void plat_minimize(void);
bool plat_csd(void);
uint32_t plat_mods(void);
void plat_trim(void);          /* release spare buffers while idle */            /* true when no server-side decorations */
uint64_t plat_now_ms(void);
void plat_watch_fd(int fd, void (*cb)(int fd, void *), void *ud);

/* app callbacks (app.c) */
void app_resize(int pw, int ph, double scale);
void app_render(Canvas *c);
void app_key(uint32_t sym, uint32_t mods, const char *utf8);
void app_button(int button, bool down, double x, double y, uint32_t mods, int clicks);
void app_motion(double x, double y);
void app_scroll(double dx, double dy, bool discrete);
void app_pointer_leave(void);
void app_focus(bool in);
void app_close_request(void);
void app_timer(int id);
void app_drop_files(char **paths, int n);
