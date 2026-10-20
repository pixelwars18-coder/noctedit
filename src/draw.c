/* Software rasterizer: rects, anti-aliased rounded rects, FreeType glyphs. */
#include "noctedit.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "noctedit: out of memory\n"); abort(); }
    return p;
}
void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "noctedit: out of memory\n"); abort(); }
    return p;
}
char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

/* ------------------------------------------------------------------ utf8 */
uint32_t utf8_dec(const char *s, int n, int *len) {
    const unsigned char *u = (const unsigned char *)s;
    if (n <= 0) { *len = 0; return 0; }
    unsigned c = u[0];
    if (c < 0x80) { *len = 1; return c; }
    int need; uint32_t cp;
    if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else { *len = 1; return 0xFFFD; }
    if (need >= n) { *len = 1; return 0xFFFD; }
    for (int i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        cp = (cp << 6) | (u[i] & 0x3F);
    }
    *len = need + 1;
    return cp;
}
int utf8_enc(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
    o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 0x3F);
    o[2] = 0x80 | ((cp >> 6) & 0x3F); o[3] = 0x80 | (cp & 0x3F);
    return 4;
}
int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    int j = i - 1;
    while (j > 0 && i - j < 4 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
    int l;
    utf8_dec(s + j, i - j, &l);
    return j + l == i ? j : i - 1;
}
int utf8_next(const char *s, int n, int i) {
    if (i >= n) return n;
    int l;
    utf8_dec(s + i, n - i, &l);
    return i + (l ? l : 1);
}

/* ------------------------------------------------------------------ rects */
Rect rect_union(Rect a, Rect b) {
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return (Rect){x0, y0, x1 - x0, y1 - y0};
}
Rect rect_intersect(Rect a, Rect b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return (Rect){0, 0, 0, 0};
    return (Rect){x0, y0, x1 - x0, y1 - y0};
}
void cv_clip(Canvas *c, Rect r) {
    r = rect_intersect(r, (Rect){0, 0, c->w, c->h});
    c->cx0 = r.x; c->cy0 = r.y; c->cx1 = r.x + r.w; c->cy1 = r.y + r.h;
}
Rect cv_get_clip(const Canvas *c) { return (Rect){c->cx0, c->cy0, c->cx1 - c->cx0, c->cy1 - c->cy0}; }

static inline uint32_t blend(uint32_t d, uint32_t c, unsigned a) {
    unsigned ia = 255 - a;
    uint32_t rb = (c & 0xff00ff) * a + (d & 0xff00ff) * ia + 0x800080;
    rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
    uint32_t g = (c & 0xff00) * a + (d & 0xff00) * ia + 0x8000;
    g = ((g + ((g >> 8) & 0xff00)) >> 8) & 0xff00;
    return 0xff000000 | rb | g;
}

Color color_mix(Color a, Color b, float t) {
    unsigned k = (unsigned)(CLAMP(t, 0.f, 1.f) * 255.f + .5f);
    return blend(a, b, k);
}
Color color_alpha(Color c, int a) { return (c & 0xffffff) | ((uint32_t)CLAMP(a, 0, 255) << 24); }

void cv_fill(Canvas *c, int x, int y, int w, int h, Color col) {
    int x0 = MAX(x, c->cx0), y0 = MAX(y, c->cy0);
    int x1 = MIN(x + w, c->cx1), y1 = MIN(y + h, c->cy1);
    if (x1 <= x0 || y1 <= y0) return;
    unsigned a = col >> 24;
    if (!a) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *p = c->px + (size_t)yy * c->stride + x0;
        int n = x1 - x0;
        if (a == 255) {
            uint32_t v = col | 0xff000000;
            for (int i = 0; i < n; i++) p[i] = v;
        } else {
            for (int i = 0; i < n; i++) p[i] = blend(p[i], col, a);
        }
    }
}

static inline void put(Canvas *c, int x, int y, Color col, unsigned cov) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1 || !cov) return;
    unsigned a = ((col >> 24) * cov + 127) / 255;
    uint32_t *p = c->px + (size_t)y * c->stride + x;
    *p = blend(*p, col, a);
}

void cv_rrect(Canvas *c, int x, int y, int w, int h, int r, Color col) {
    if (w <= 0 || h <= 0) return;
    r = MIN(r, MIN(w, h) / 2);
    if (r <= 0) { cv_fill(c, x, y, w, h, col); return; }
    if (y + h <= c->cy0 || y >= c->cy1 || x + w <= c->cx0 || x >= c->cx1) return;
    cv_fill(c, x, y + r, w, h - 2 * r, col);
    for (int j = 0; j < r; j++) {
        float dy = r - (j + .5f);
        int yt = y + j, yb = y + h - 1 - j;
        int solid_from = r; /* first column (from the edge) fully covered */
        for (int i = 0; i < r; i++) {
            float dx = r - (i + .5f);
            float cov = r - sqrtf(dx * dx + dy * dy) + .5f;
            if (cov >= 1.f) { solid_from = i; break; }
            if (cov <= 0.f) continue;
            unsigned cv8 = (unsigned)(cov * 255.f);
            put(c, x + i, yt, col, cv8);
            put(c, x + w - 1 - i, yt, col, cv8);
            put(c, x + i, yb, col, cv8);
            put(c, x + w - 1 - i, yb, col, cv8);
        }
        cv_fill(c, x + solid_from, yt, w - 2 * solid_from, 1, col);
        cv_fill(c, x + solid_from, yb, w - 2 * solid_from, 1, col);
    }
}

void cv_rrect_border(Canvas *c, int x, int y, int w, int h, int r, int bw, Color border, Color fill) {
    cv_rrect(c, x, y, w, h, r, border);
    cv_rrect(c, x + bw, y + bw, w - 2 * bw, h - 2 * bw, MAX(0, r - bw), fill);
}

/* ------------------------------------------------------------------ fonts */
static FT_Library ft;
static bool fc_ready;

typedef struct {
    uint32_t key;     /* codepoint | subpixel phase << 21 */
    int16_t left, top;
    int32_t adv;      /* 26.6 fixed point, unrounded */
    uint16_t w, h;
    uint32_t off;
    bool used;
} Glyph;

struct Font {
    FT_Face face;
    int px, asc, height;
    Glyph *tab;
    int tcap, tcount;
    uint8_t *slab;
    size_t slab_len, slab_cap;
};

typedef struct { char *path; int index; FT_Face face; } Fallback;
static Fallback fallbacks[12];
static int nfallbacks;
static uint8_t gamma_lut[256];

static bool ft_init(void) {
    if (ft) return true;
    if (FT_Init_FreeType(&ft)) return false;
    for (int i = 0; i < 256; i++) gamma_lut[i] = (uint8_t)(powf(i / 255.f, 0.85f) * 255.f + .5f);
    return true;
}

static bool fc_resolve(const char *pattern, char **path, int *index) {
    if (!fc_ready) { if (!FcInit()) return false; fc_ready = true; }
    FcPattern *p = FcNameParse((const FcChar8 *)pattern);
    if (!p) return false;
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, p, &res);
    FcPatternDestroy(p);
    if (!m) return false;
    FcChar8 *file = NULL;
    int idx = 0;
    bool ok = FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch;
    FcPatternGetInteger(m, FC_INDEX, 0, &idx);
    if (ok) { *path = xstrdup((const char *)file); *index = idx; }
    FcPatternDestroy(m);
    return ok;
}

static Font *font_from_face(FT_Face face, int px) {
    if (FT_Set_Pixel_Sizes(face, 0, px)) { FT_Done_Face(face); return NULL; }
    Font *f = calloc(1, sizeof *f);
    f->face = face;
    f->px = px;
    f->asc = (int)((face->size->metrics.ascender + 63) >> 6);
    f->height = (int)((face->size->metrics.height + 63) >> 6);
    int desc = (int)((-face->size->metrics.descender + 63) >> 6);
    if (f->height < f->asc + desc) f->height = f->asc + desc;
    f->tcap = 256;
    f->tab = calloc(f->tcap, sizeof(Glyph));
    return f;
}

Font *font_open_file(const char *path, int px) {
    if (!ft_init()) return NULL;
    FT_Face face;
    if (FT_New_Face(ft, path, 0, &face)) return NULL;
    return font_from_face(face, px);
}

Font *font_open(const char *pattern, int px) {
    if (!ft_init()) return NULL;
    char *path = NULL;
    int idx = 0;
    if (!fc_resolve(pattern, &path, &idx)) return NULL;
    FT_Face face;
    int err = FT_New_Face(ft, path, idx, &face);
    free(path);
    if (err) return NULL;
    return font_from_face(face, px);
}

void font_free(Font *f) {
    if (!f) return;
    FT_Done_Face(f->face);
    free(f->tab);
    free(f->slab);
    free(f);
}

void fonts_trim(void) {
    FcFini(); /* also drops state pdf.c may have initialized */
    fc_ready = false;
}

void fonts_shutdown(void) {
    for (int i = 0; i < nfallbacks; i++) {
        if (fallbacks[i].face) FT_Done_Face(fallbacks[i].face);
        free(fallbacks[i].path);
    }
    nfallbacks = 0;
    if (ft) FT_Done_FreeType(ft);
    ft = NULL;
    if (fc_ready) FcFini();
    fc_ready = false;
}

int font_ascent(const Font *f) { return f->asc; }
int font_height(const Font *f) { return f->height; }

static FT_Face fallback_for(uint32_t cp) {
    for (int i = 0; i < nfallbacks; i++)
        if (fallbacks[i].face && FT_Get_Char_Index(fallbacks[i].face, cp)) return fallbacks[i].face;
    if (!fc_ready) { if (!FcInit()) return NULL; fc_ready = true; }
    FcPattern *p = FcPatternCreate();
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(p, FC_CHARSET, cs);
    FcPatternAddBool(p, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, p, &res);
    FcPatternDestroy(p);
    FcCharSetDestroy(cs);
    FT_Face face = NULL;
    if (m) {
        FcChar8 *file;
        int idx = 0;
        if (FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
            FcPatternGetInteger(m, FC_INDEX, 0, &idx);
            for (int i = 0; i < nfallbacks; i++)
                if (!strcmp(fallbacks[i].path, (char *)file) && fallbacks[i].index == idx) { FcPatternDestroy(m); return NULL; }
            if (nfallbacks < (int)(sizeof fallbacks / sizeof *fallbacks) &&
                !FT_New_Face(ft, (char *)file, idx, &face)) {
                if (FT_HAS_FIXED_SIZES(face) && !FT_IS_SCALABLE(face)) { FT_Done_Face(face); face = NULL; }
                fallbacks[nfallbacks++] = (Fallback){xstrdup((char *)file), idx, face};
                if (face && !FT_Get_Char_Index(face, cp)) face = NULL;
            }
        }
        FcPatternDestroy(m);
    }
    return face;
}

static Glyph *glyph_slot(Font *f, uint32_t key) {
    uint32_t mask = f->tcap - 1, h = (key * 2654435761u) & mask;
    while (f->tab[h].used && f->tab[h].key != key) h = (h + 1) & mask;
    return &f->tab[h];
}

/* Glyphs are cached per quarter-pixel phase so text can sit at fractional x positions:
 * spacing stays true to the font instead of rounding every advance to whole pixels. */
static Glyph *glyph_get(Font *f, uint32_t cp, int phase) {
    uint32_t key = cp | (uint32_t)phase << 21;
    Glyph *g = glyph_slot(f, key);
    if (g->used) return g;
    if ((f->tcount + 1) * 10 > f->tcap * 7) { /* grow */
        Glyph *old = f->tab;
        int oc = f->tcap;
        f->tcap *= 2;
        f->tab = calloc(f->tcap, sizeof(Glyph));
        for (int i = 0; i < oc; i++) if (old[i].used) *glyph_slot(f, old[i].key) = old[i];
        free(old);
        g = glyph_slot(f, key);
    }
    FT_Face face = f->face;
    FT_UInt idx = FT_Get_Char_Index(face, cp);
    if (!idx && cp > 0x7f) {
        FT_Face fb = fallback_for(cp);
        if (fb && !FT_Set_Pixel_Sizes(fb, 0, f->px)) { face = fb; idx = FT_Get_Char_Index(fb, cp); }
    }
    Glyph ng = {.key = key, .used = true};
    if (!FT_Load_Glyph(face, idx, FT_LOAD_TARGET_LIGHT)) {
        FT_GlyphSlot s = face->glyph;
        ng.adv = (int32_t)((s->linearHoriAdvance + 512) >> 10);
        if (!ng.adv) ng.adv = (int32_t)s->advance.x;
        if (s->format == FT_GLYPH_FORMAT_OUTLINE && phase) FT_Outline_Translate(&s->outline, phase * 16, 0);
        if (!FT_Render_Glyph(s, FT_RENDER_MODE_LIGHT)) {
            FT_Bitmap *bm = &s->bitmap;
            ng.left = s->bitmap_left;
            ng.top = s->bitmap_top;
            if (bm->pixel_mode == FT_PIXEL_MODE_GRAY && bm->width && bm->rows) {
                ng.w = bm->width;
                ng.h = bm->rows;
                size_t need = (size_t)ng.w * ng.h;
                if (f->slab_len + need > f->slab_cap) {
                    f->slab_cap = MAX(f->slab_cap * 2, f->slab_len + need + 16384);
                    f->slab = xrealloc(f->slab, f->slab_cap);
                }
                ng.off = (uint32_t)f->slab_len;
                for (unsigned r = 0; r < bm->rows; r++)
                    memcpy(f->slab + f->slab_len + (size_t)r * ng.w, bm->buffer + (long)r * bm->pitch, ng.w);
                f->slab_len += need;
            }
        }
    }
    *g = ng;
    f->tcount++;
    return g;
}

int font_adv26(Font *f, uint32_t cp) { return glyph_get(f, cp, 0)->adv; }
int font_adv(Font *f, uint32_t cp) { return (glyph_get(f, cp, 0)->adv + 32) >> 6; }

static void blit_glyph(Canvas *c, Font *f, Glyph *g, int x, int baseline, Color col) {
    if (!g->w) return;
    int gx = x + g->left, gy = baseline - g->top;
    int x0 = MAX(gx, c->cx0), y0 = MAX(gy, c->cy0);
    int x1 = MIN(gx + g->w, c->cx1), y1 = MIN(gy + g->h, c->cy1);
    if (x1 <= x0 || y1 <= y0) return;
    unsigned ca = col >> 24;
    const uint8_t *bm = f->slab + g->off;
    for (int y = y0; y < y1; y++) {
        const uint8_t *src = bm + (size_t)(y - gy) * g->w + (x0 - gx);
        uint32_t *dst = c->px + (size_t)y * c->stride + x0;
        for (int i = 0; i < x1 - x0; i++) {
            unsigned a = src[i];
            if (!a) continue;
            a = gamma_lut[a];
            if (ca != 255) a = (a * ca + 127) / 255;
            dst[i] = a == 255 ? (col | 0xff000000) : blend(dst[i], col, a);
        }
    }
}

/* x26: 26.6 fixed-point pen position; returns the 26.6 advance */
int draw_glyph26(Canvas *c, Font *f, int x26, int baseline, uint32_t cp, Color col) {
    int px = x26 >> 6, phase = ((x26 & 63) + 8) >> 4;
    if (phase == 4) { px++; phase = 0; }
    Glyph *g = glyph_get(f, cp, phase);
    blit_glyph(c, f, g, px, baseline, col);
    return g->adv;
}

int draw_glyph(Canvas *c, Font *f, int x, int baseline, uint32_t cp, Color col) {
    return (draw_glyph26(c, f, x << 6, baseline, cp, col) + 32) >> 6;
}

int draw_text(Canvas *c, Font *f, int x, int baseline, const char *s, int n, Color col) {
    int x26 = x << 6, lim = c->cx1 << 6;
    for (int i = 0; i < n;) {
        int l;
        uint32_t cp = utf8_dec(s + i, n - i, &l);
        x26 += x26 < lim ? draw_glyph26(c, f, x26, baseline, cp, col) : font_adv26(f, cp);
        i += l;
    }
    return (x26 + 32) >> 6;
}

int text_width(Font *f, const char *s, int n) {
    int w = 0;
    for (int i = 0; i < n;) {
        int l;
        uint32_t cp = utf8_dec(s + i, n - i, &l);
        w += font_adv26(f, cp);
        i += l;
    }
    return (w + 32) >> 6;
}
