/* PDF support.
 *  - pdf_export(): a small self-contained PDF writer. Embeds the editor's monospace
 *    TrueType font (Identity-H, with a ToUnicode map so text stays selectable/searchable),
 *    prints syntax-colored text with line numbers, wrapping and page headers.
 *  - pdf_extract_text(): runs poppler's `pdftotext` to open a PDF as text. */
#define _GNU_SOURCE
#include "noctedit.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

extern char **environ;

/* ------------------------------------------------------------------ growable buffer */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const void *s, size_t n) {
    if (b->n + n > b->cap) { b->cap = MAX(b->cap * 2, b->n + n + 4096); b->p = xrealloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void bprintf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof tmp) {
        char *big = xmalloc(n + 1);
        va_start(ap, fmt);
        vsnprintf(big, n + 1, fmt, ap);
        va_end(ap);
        bput(b, big, n);
        free(big);
    } else bput(b, tmp, n);
}

/* ------------------------------------------------------------------ font selection */
static bool is_plain_ttf(const char *path) {
    unsigned char m[4];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(m, 1, 4, f) == 4 && ((m[0] == 0 && m[1] == 1 && m[2] == 0 && m[3] == 0) || !memcmp(m, "true", 4));
    fclose(f);
    return ok;
}

/* first TrueType (glyf-based, non-collection) match for the pattern */
static char *pick_font(const char *pattern) {
    if (!FcInit()) return NULL;
    char *result = NULL;
    const char *pats[] = {pattern, "monospace", "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono"};
    for (size_t k = 0; k < sizeof pats / sizeof *pats && !result; k++) {
        FcPattern *p = FcNameParse((const FcChar8 *)pats[k]);
        if (!p) continue;
        FcConfigSubstitute(NULL, p, FcMatchPattern);
        FcDefaultSubstitute(p);
        FcResult r;
        FcFontSet *fs = FcFontSort(NULL, p, FcFalse, NULL, &r);
        FcPatternDestroy(p);
        if (!fs) continue;
        for (int i = 0; i < fs->nfont && i < 40 && !result; i++) {
            FcChar8 *file;
            int idx = 0, spacing = 0;
            if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) != FcResultMatch) continue;
            FcPatternGetInteger(fs->fonts[i], FC_INDEX, 0, &idx);
            FcPatternGetInteger(fs->fonts[i], FC_SPACING, 0, &spacing);
            if (idx == 0 && spacing >= FC_MONO && is_plain_ttf((char *)file)) result = xstrdup((char *)file);
        }
        FcFontSetDestroy(fs);
    }
    return result;
}

/* ------------------------------------------------------------------ print colors */
static const char *print_rgb(uint8_t t) {
    switch (t) {
    case T_KW: return "0.48 0.20 0.62";
    case T_TYPE: return "0.08 0.40 0.70";
    case T_STR: return "0.16 0.48 0.20";
    case T_NUM: case T_CONST: return "0.70 0.38 0.06";
    case T_COMMENT: return "0.50 0.50 0.50";
    case T_PRE: return "0.70 0.18 0.20";
    case T_FUNC: return "0.05 0.45 0.50";
    case T_PUNCT: return "0.35 0.35 0.35";
    case T_HEAD: return "0.48 0.20 0.62";
    default: return "0.10 0.10 0.10";
    }
}

typedef struct { uint32_t cp; uint16_t gid; } UMap;

static int umap_cmp(const void *a, const void *b) {
    const UMap *x = a, *y = b;
    return x->gid != y->gid ? (x->gid < y->gid ? -1 : 1) : (x->cp < y->cp ? -1 : x->cp > y->cp);
}


/* ------------------------------------------------------------------ TrueType subsetting
 * Keeps glyph ids stable (needed for Identity CID mapping) but empties every glyph we
 * don't use, and drops layout tables a PDF viewer never reads. */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

static bool subset_ttf(const uint8_t *in, size_t n, const uint8_t *used_in, Buf *out) {
    if (n < 12) return false;
    int nt = rd16(in + 4);
    if (12 + nt * 16 > (int)n) return false;
    const uint8_t *head = NULL, *maxp = NULL, *loca = NULL, *glyf = NULL;
    uint32_t glyf_len = 0, loca_len = 0;
    for (int i = 0; i < nt; i++) {
        const uint8_t *r = in + 12 + i * 16;
        uint32_t off = rd32(r + 8), len = rd32(r + 12);
        if ((size_t)off + len > n) return false;
        if (!memcmp(r, "head", 4)) head = in + off;
        else if (!memcmp(r, "maxp", 4)) maxp = in + off;
        else if (!memcmp(r, "loca", 4)) { loca = in + off; loca_len = len; }
        else if (!memcmp(r, "glyf", 4)) { glyf = in + off; glyf_len = len; }
    }
    if (!head || !maxp || !loca || !glyf) return false;
    int ng = rd16(maxp + 4), longfmt = (int16_t)rd16(head + 50);
    if ((size_t)(ng + 1) * (longfmt ? 4 : 2) > loca_len) return false;
#define LOCA(i) (longfmt ? rd32(loca + 4 * (i)) : 2u * rd16(loca + 2 * (i)))
    uint8_t *used = calloc(ng, 1);
    for (int g = 0; g < ng && g < 65536; g++) used[g] = used_in[g];
    used[0] = 1;
    /* pull in composite glyph components until stable */
    for (bool changed = true; changed;) {
        changed = false;
        for (int g = 0; g < ng; g++) {
            if (!used[g]) continue;
            uint32_t a = LOCA(g), b = LOCA(g + 1);
            if (b <= a || b > glyf_len || b - a < 10 || (int16_t)rd16(glyf + a) >= 0) continue;
            const uint8_t *p = glyf + a + 10, *e = glyf + b;
            while (p + 4 <= e) {
                uint16_t fl = rd16(p), gi = rd16(p + 2);
                if (gi < ng && !used[gi]) { used[gi] = 1; changed = true; }
                p += 4 + ((fl & 1) ? 4 : 2) + ((fl & 8) ? 2 : (fl & 0x40) ? 4 : (fl & 0x80) ? 8 : 0);
                if (!(fl & 0x20)) break;
            }
        }
    }
    /* new glyf + long loca */
    Buf ng_glyf = {0};
    uint8_t *nloca = xmalloc((ng + 1) * 4);
    for (int g = 0; g < ng; g++) {
        wr32(nloca + 4 * g, (uint32_t)ng_glyf.n);
        uint32_t a = LOCA(g), b = LOCA(g + 1);
        if (used[g] && b > a && b <= glyf_len) {
            bput(&ng_glyf, glyf + a, b - a);
            while (ng_glyf.n & 3) bput(&ng_glyf, "", 1);
        }
    }
    wr32(nloca + 4 * ng, (uint32_t)ng_glyf.n);
#undef LOCA
    static const char *keep[] = {"OS/2", "cvt ", "fpgm", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name", "post", "prep"};
    const uint8_t *tdata[16];
    uint32_t tlen[16];
    char ttag[16][4];
    int k = 0;
    for (int i = 0; i < nt && k < 16; i++) {
        const uint8_t *r = in + 12 + i * 16;
        bool want = false;
        for (size_t j = 0; j < sizeof keep / sizeof *keep; j++) if (!memcmp(r, keep[j], 4)) want = true;
        if (!want) continue;
        memcpy(ttag[k], r, 4);
        tdata[k] = in + rd32(r + 8);
        tlen[k] = rd32(r + 12);
        if (!memcmp(r, "glyf", 4)) { tdata[k] = (uint8_t *)ng_glyf.p; tlen[k] = (uint32_t)ng_glyf.n; }
        if (!memcmp(r, "loca", 4)) { tdata[k] = nloca; tlen[k] = (ng + 1) * 4; }
        k++;
    }
    uint8_t hdr[12 + 16 * 16];
    memcpy(hdr, in, 4);
    wr16(hdr + 4, k);
    int es = 0;
    while ((2 << es) <= k) es++;
    wr16(hdr + 6, (1 << es) * 16);
    wr16(hdr + 8, es);
    wr16(hdr + 10, k * 16 - (1 << es) * 16);
    uint32_t off = 12 + 16 * k;
    for (int i = 0; i < k; i++) {
        uint8_t *r = hdr + 12 + 16 * i;
        memcpy(r, ttag[i], 4);
        uint32_t sum = 0;
        for (uint32_t j = 0; j < tlen[i]; j += 4) {
            uint8_t w[4] = {0};
            memcpy(w, tdata[i] + j, MIN(4u, tlen[i] - j));
            sum += rd32(w);
        }
        wr32(r + 4, sum);
        wr32(r + 8, off);
        wr32(r + 12, tlen[i]);
        off += (tlen[i] + 3) & ~3u;
    }
    bput(out, hdr, 12 + 16 * k);
    for (int i = 0; i < k; i++) {
        size_t at = out->n;
        bput(out, tdata[i], tlen[i]);
        if (!memcmp(ttag[i], "head", 4)) { wr16((uint8_t *)out->p + at + 50, 1); wr32((uint8_t *)out->p + at + 8, 0); }
        while (out->n & 3) bput(out, "", 1);
    }
    free(used);
    free(nloca);
    free(ng_glyf.p);
    return true;
}

/* ------------------------------------------------------------------ export */
bool pdf_export(Doc *d, const char *path, const char *font_pattern, bool line_numbers, char *err, int errn) {
    char *font = pick_font(font_pattern);
    if (!font) { snprintf(err, errn, "no TrueType monospace font found"); return false; }
    FT_Library ft;
    FT_Face face;
    if (FT_Init_FreeType(&ft)) { free(font); snprintf(err, errn, "FreeType init failed"); return false; }
    if (FT_New_Face(ft, font, 0, &face)) {
        FT_Done_FreeType(ft);
        snprintf(err, errn, "cannot load %s", font);
        free(font);
        return false;
    }
    int upem = face->units_per_EM ? face->units_per_EM : 1000;
#define FU(v) ((int)((long)(v) * 1000 / upem))
    FT_UInt space_gid = FT_Get_Char_Index(face, ' ');
    FT_Load_Glyph(face, space_gid, FT_LOAD_NO_SCALE);
    int adv1000 = FU(face->glyph->advance.x);
    if (adv1000 <= 0) adv1000 = 600;

    /* page geometry (points) */
    const double PW = 595.28, PH = 841.89, FS = 8.5, LEAD = 11.2;
    const double ML = 42, MR = 36, MT = 54, MB = 42;
    double gutter = line_numbers ? FS * adv1000 / 1000.0 * 6 : 0;
    double tx = ML + gutter;
    int cols = (int)((PW - MR - tx) / (FS * adv1000 / 1000.0));
    int rows_per_page = (int)((PH - MT - MB) / LEAD);
    if (cols < 10) cols = 10;

    /* pass 1: count visual rows to know the page total */
    long total_rows = 0;
    for (int i = 0; i < d->nlines; i++) {
        Line *l = &d->lines[i];
        int c = 0, rows = 1;
        for (int k = 0; k < l->len;) {
            int cl;
            uint32_t cp = utf8_dec(l->s + k, l->len - k, &cl);
            int w = cp == '\t' ? d->tab_w - (c % d->tab_w) : 1;
            if (c + w > cols) { rows++; c = 0; w = cp == '\t' ? d->tab_w : 1; }
            c += w;
            k += cl;
        }
        total_rows += rows;
    }
    int npages = (int)MAX(1, (total_rows + rows_per_page - 1) / rows_per_page);

    /* glyph bookkeeping */
    uint8_t *used = calloc(65536, 1);
    uint32_t *gid_cp = calloc(65536, sizeof(uint32_t));
    Buf *pages = calloc(npages, sizeof(Buf));

    const char *name = d->path ? strrchr(d->path, '/') : NULL;
    name = name ? name + 1 : doc_name(d);
    char date[32];
    time_t now = time(NULL);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&now));

    uint8_t *cls = NULL;
    int cls_cap = 0;
    uint8_t st = 0;
    int page = -1, row_on_page = rows_per_page;
    Buf *pb = NULL;

#define GLYPH(cp_) ({ uint32_t _c = (cp_); FT_UInt _g = FT_Get_Char_Index(face, _c); if (_g > 65535) _g = 0; \
                      if (!used[_g]) { used[_g] = 1; gid_cp[_g] = _c; } _g; })

    void text_at(double x, double y, const char *s, int n, const char *rgb) {
        bprintf(pb, "%s rg BT /F1 %.2f Tf 1 0 0 1 %.2f %.2f Tm <", rgb, FS, x, y);
        for (int k = 0; k < n;) {
            int cl;
            uint32_t cp = utf8_dec(s + k, n - k, &cl);
            bprintf(pb, "%04X", GLYPH(cp));
            k += cl;
        }
        bput(pb, "> Tj ET\n", 8);
    }

    void new_page(void) {
        page++;
        pb = &pages[page];
        row_on_page = 0;
        double hy = PH - MT + 22;
        text_at(ML, hy, name, (int)strlen(name), "0.35 0.35 0.35");
        char right[64];
        int rn = snprintf(right, sizeof right, "%s   %d / %d", date, page + 1, npages);
        text_at(PW - MR - rn * FS * adv1000 / 1000.0, hy, right, rn, "0.55 0.55 0.55");
        bprintf(pb, "0.8 0.8 0.8 RG 0.5 w %.2f %.2f m %.2f %.2f l S\n", ML, hy - 6, PW - MR, hy - 6);
    }

    for (int i = 0; i < d->nlines; i++) {
        Line *l = &d->lines[i];
        if (cls_cap < l->len + 1) { cls_cap = l->len + 256; cls = xrealloc(cls, cls_cap); }
        st = hl_line(d->lang, l->s, l->len, st, cls);
        int k = 0, c = 0;
        bool first = true;
        do {
            if (row_on_page >= rows_per_page) new_page();
            double y = PH - MT - row_on_page * LEAD;
            if (first && line_numbers) {
                char num[16];
                int nn = snprintf(num, sizeof num, "%d", i + 1);
                text_at(tx - (nn + 1.5) * FS * adv1000 / 1000.0, y, num, nn, "0.62 0.62 0.62");
            }
            first = false;
            /* emit runs of equal class on this visual row */
            char run[4096];
            int rl = 0, run_col = c;
            uint8_t rcls = 255;
            c = 0;
            run_col = 0;
            while (k < l->len) {
                int cl;
                uint32_t cp = utf8_dec(l->s + k, l->len - k, &cl);
                int w = cp == '\t' ? d->tab_w - (c % d->tab_w) : 1;
                if (c + w > cols && c > 0) break;
                uint8_t t = cls[k];
                bool blank = cp == ' ' || cp == '\t' || cp < 0x20;
                if ((t != rcls && !blank) || rl + 8 >= (int)sizeof run) {
                    if (rl) text_at(tx + run_col * FS * adv1000 / 1000.0, y, run, rl, print_rgb(rcls));
                    rl = 0;
                    rcls = t;
                    run_col = c;
                }
                if (blank) {
                    if (!rl) run_col = c + w;
                    else for (int s = 0; s < w; s++) run[rl++] = ' ';
                } else {
                    memcpy(run + rl, l->s + k, cl);
                    rl += cl;
                }
                c += w;
                k += cl;
            }
            if (rl) text_at(tx + run_col * FS * adv1000 / 1000.0, y, run, rl, print_rgb(rcls));
            row_on_page++;
        } while (k < l->len);
    }
    if (page < 0) new_page();
    npages = page + 1;

    /* read the font file */
    FILE *ff = fopen(font, "rb");
    Buf fontdata = {0};
    if (ff) {
        char tmp[65536];
        size_t r;
        while ((r = fread(tmp, 1, sizeof tmp, ff)) > 0) bput(&fontdata, tmp, r);
        fclose(ff);
    }

    /* assemble the file */
    Buf out = {0};
    long *off = calloc(16 + npages * 2, sizeof(long));
    int nobj = 0;
#define OBJ(id_) (off[id_] = (long)out.n, bprintf(&out, "%d 0 obj\n", id_), (nobj = MAX(nobj, id_)))
    void stream(int n, const char *dict, const void *data, size_t len, bool deflate) {
        OBJ(n);
        if (deflate) {
            uLongf zn = compressBound(len);
            unsigned char *z = xmalloc(zn);
            compress2(z, &zn, data, len, 9);
            bprintf(&out, "<< %s /Length %lu /Filter /FlateDecode >>\nstream\n", dict, (unsigned long)zn);
            bput(&out, z, zn);
            free(z);
        } else {
            bprintf(&out, "<< %s /Length %zu >>\nstream\n", dict, len);
            bput(&out, data, len);
        }
        bprintf(&out, "\nendstream\nendobj\n");
    }
    bprintf(&out, "%%PDF-1.5\n%%\xe2\xe3\xcf\xd3\n");
    /* 1 catalog, 2 pages, 3 type0 font, 4 cidfont, 5 descriptor, 6 fontfile, 7 tounicode, 8 info, 9.. pages */
    OBJ(1); bprintf(&out, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    OBJ(2); bprintf(&out, "<< /Type /Pages /Count %d /Kids [", npages);
    for (int p = 0; p < npages; p++) bprintf(&out, " %d 0 R", 9 + p * 2);
    bprintf(&out, " ] >>\nendobj\n");
    OBJ(3); bprintf(&out, "<< /Type /Font /Subtype /Type0 /BaseFont /NoctMono /Encoding /Identity-H "
                          "/DescendantFonts [4 0 R] /ToUnicode 7 0 R >>\nendobj\n");
    OBJ(4); bprintf(&out, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /NoctMono "
                          "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
                          "/FontDescriptor 5 0 R /CIDToGIDMap /Identity /DW %d /W [", adv1000);
    for (int g = 0; g < 65536; g++) {
        if (!used[g]) continue;
        if (FT_Load_Glyph(face, g, FT_LOAD_NO_SCALE)) continue;
        int w = FU(face->glyph->advance.x);
        if (w != adv1000) bprintf(&out, " %d [%d]", g, w);
    }
    bprintf(&out, " ] >>\nendobj\n");
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    OBJ(5); bprintf(&out, "<< /Type /FontDescriptor /FontName /NoctMono /Flags 33 /FontBBox [%d %d %d %d] "
                          "/ItalicAngle 0 /Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 6 0 R >>\nendobj\n",
                    FU(face->bbox.xMin), FU(face->bbox.yMin), FU(face->bbox.xMax), FU(face->bbox.yMax),
                    FU(face->ascender), FU(face->descender), os2 && os2->sCapHeight ? FU(os2->sCapHeight) : FU(face->ascender));
    Buf sub = {0};
    Buf *emb = &fontdata;
    if (subset_ttf((const uint8_t *)fontdata.p, fontdata.n, used, &sub)) emb = &sub;
    char lendict[64];
    snprintf(lendict, sizeof lendict, "/Length1 %zu", emb->n);
    stream(6, lendict, emb->p, emb->n, true);
    free(sub.p);
    /* ToUnicode CMap */
    Buf cm = {0};
    bprintf(&cm, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                 "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                 "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
    UMap *um = xmalloc(65536 * sizeof(UMap));
    int nu = 0;
    for (int g = 1; g < 65536; g++) if (used[g]) um[nu++] = (UMap){gid_cp[g], (uint16_t)g};
    qsort(um, nu, sizeof(UMap), umap_cmp);
    for (int s = 0; s < nu; s += 100) {
        int e = MIN(nu, s + 100);
        bprintf(&cm, "%d beginbfchar\n", e - s);
        for (int k = s; k < e; k++) {
            uint32_t cp = um[k].cp;
            if (cp > 0xFFFF) {
                cp -= 0x10000;
                bprintf(&cm, "<%04X> <%04X%04X>\n", um[k].gid, 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
            } else bprintf(&cm, "<%04X> <%04X>\n", um[k].gid, cp);
        }
        bprintf(&cm, "endbfchar\n");
    }
    bprintf(&cm, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
    stream(7, "", cm.p, cm.n, true);
    OBJ(8);
    bprintf(&out, "<< /Producer (Noctedit " NOCTEDIT_VERSION ") /Title (");
    for (const char *p = name; *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') bput(&out, "\\", 1);
        if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) bput(&out, p, 1);
    }
    bprintf(&out, ") >>\nendobj\n");
    for (int p = 0; p < npages; p++) {
        OBJ(9 + p * 2);
        bprintf(&out, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /Font << /F1 3 0 R >> >> "
                      "/Contents %d 0 R >>\nendobj\n", PW, PH, 10 + p * 2);
        stream(10 + p * 2, "", pages[p].p, pages[p].n, true);
    }
    long xref = (long)out.n;
    bprintf(&out, "xref\n0 %d\n0000000000 65535 f \n", nobj + 1);
    for (int n = 1; n <= nobj; n++) bprintf(&out, "%010ld 00000 n \n", off[n]);
    bprintf(&out, "trailer\n<< /Size %d /Root 1 0 R /Info 8 0 R >>\nstartxref\n%ld\n%%%%EOF\n", nobj + 1, xref);

    bool ok = false;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) snprintf(err, errn, "%s", strerror(errno));
    else {
        size_t w = 0;
        while (w < out.n) {
            ssize_t r = write(fd, out.p + w, out.n - w);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            w += r;
        }
        ok = w == out.n && close(fd) == 0;
        if (!ok) snprintf(err, errn, "write failed: %s", strerror(errno));
    }
    for (int p = 0; p < npages; p++) free(pages[p].p);
    free(pages);
    free(out.p);
    free(cm.p);
    free(um);
    free(fontdata.p);
    free(off);
    free(used);
    free(gid_cp);
    free(cls);
    free(font);
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    return ok;
}

/* ------------------------------------------------------------------ import */
bool is_pdf_file(const char *path) {
    char m[5];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(m, 1, 5, f) == 5 && !memcmp(m, "%PDF-", 5);
    fclose(f);
    return ok;
}

char *pdf_extract_text(const char *path, size_t *len, char *err, int errn) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) { snprintf(err, errn, "%s", strerror(errno)); return NULL; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {"pdftotext", "-layout", "-enc", "UTF-8", (char *)path, "-", NULL};
    pid_t pid;
    int rc = posix_spawnp(&pid, "pdftotext", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (rc) {
        close(p[0]);
        snprintf(err, errn, rc == ENOENT ? "pdftotext not found (pacman -S poppler)" : "%s", strerror(rc));
        return NULL;
    }
    Buf b = {0};
    char tmp[65536];
    ssize_t r;
    while ((r = read(p[0], tmp, sizeof tmp)) != 0) {
        if (r < 0) { if (errno == EINTR) continue; break; }
        bput(&b, tmp, r);
    }
    close(p[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(b.p);
        snprintf(err, errn, "pdftotext failed (encrypted or damaged PDF?)");
        return NULL;
    }
    /* page breaks (form feeds) become blank lines; drop the trailing one */
    size_t j = 0;
    for (size_t i = 0; i < b.n; i++) {
        if (b.p[i] == '\f') { if (i + 1 < b.n) b.p[j++] = '\n'; continue; }
        b.p[j++] = b.p[i];
    }
    bput(&b, "", 1);
    *len = j;
    return b.p;
}

/* ================================================================== document export
 * Word-processor style PDF: proportional fonts (regular/bold/italic/bold-italic/mono, each
 * embedded as a subset), headings, lists, quotes, alignment, line/paragraph spacing,
 * first-line indent, underline/strike, page numbers. Uses the same paragraph engine as
 * the on-screen document view. */
#include FT_ADVANCES_H

typedef struct {
    char *path;
    FT_Face face;
    int upem;
    uint8_t *used;
    uint32_t *gid_cp;
} PFont;

typedef struct { PFont *f[5]; double size[FN_COUNT]; int pf[FN_COUNT]; } PCtx;

static char *pick_ttf(const char *pattern) {
    if (!FcInit()) return NULL;
    FcPattern *p = FcNameParse((const FcChar8 *)pattern);
    if (!p) return NULL;
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult r;
    FcFontSet *fs = FcFontSort(NULL, p, FcFalse, NULL, &r);
    FcPatternDestroy(p);
    char *res = NULL;
    for (int i = 0; fs && i < fs->nfont && i < 40 && !res; i++) {
        FcChar8 *file;
        int idx = 0;
        if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) != FcResultMatch) continue;
        FcPatternGetInteger(fs->fonts[i], FC_INDEX, 0, &idx);
        if (idx == 0 && is_plain_ttf((char *)file)) res = xstrdup((char *)file);
    }
    if (fs) FcFontSetDestroy(fs);
    return res;
}

/* advance in 1/100 pt */
static int pdf_adv(void *ctx, int font, uint32_t cp) {
    PCtx *c = ctx;
    PFont *f = c->f[c->pf[font]];
    FT_UInt g = FT_Get_Char_Index(f->face, cp);
    FT_Fixed a = 0;
    FT_Get_Advance(f->face, g, FT_LOAD_NO_SCALE, &a);
    return (int)lround((double)a * c->size[font] * 100.0 / f->upem);
}

static uint16_t pdf_gid(PFont *f, uint32_t cp) {
    FT_UInt g = FT_Get_Char_Index(f->face, cp);
    if (g > 65535) g = 0;
    if (!f->used[g]) { f->used[g] = 1; f->gid_cp[g] = cp; }
    return (uint16_t)g;
}

bool pdf_export_doc(Doc *d, const char *path, const char *doc_font, const char *mono_font, char *err, int errn) {
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) { snprintf(err, errn, "FreeType init failed"); return false; }
    const char *sfx[4] = {"", ":weight=bold", ":slant=italic", ":weight=bold:slant=italic"};
    PFont fonts[5] = {0};
    int nf = 0;
    PCtx C = {0};
    for (int k = 0; k < 5; k++) {
        char pat[256], *file;
        if (k < 4) {
            snprintf(pat, sizeof pat, "%s%s", doc_font, sfx[k]);
            file = pick_ttf(pat);
            if (!file) { snprintf(pat, sizeof pat, "sans-serif%s", sfx[k]); file = pick_ttf(pat); }
        } else {
            file = pick_font(mono_font);
        }
        if (!file && k > 0) { C.f[k] = C.f[0]; continue; }
        if (!file) { FT_Done_FreeType(ft); snprintf(err, errn, "no TrueType font for “%s”", doc_font); return false; }
        int same = -1;
        for (int j = 0; j < nf; j++) if (!strcmp(fonts[j].path, file)) same = j;
        if (same >= 0) { C.f[k] = &fonts[same]; free(file); continue; }
        PFont *f = &fonts[nf];
        if (FT_New_Face(ft, file, 0, &f->face)) { free(file); C.f[k] = C.f[0]; continue; }
        f->path = file;
        f->upem = f->face->units_per_EM ? f->face->units_per_EM : 1000;
        f->used = calloc(65536, 1);
        f->gid_cp = calloc(65536, sizeof(uint32_t));
        C.f[k] = f;
        nf++;
    }
    for (int k = 0; k < 5; k++) if (!C.f[k]) C.f[k] = &fonts[0];
    /* the C.pf map indexes C.f by slot: regular, bold, italic, bold-italic, code */
    const double BODY = 12;
    C.size[FN_REG] = C.size[FN_B] = C.size[FN_I] = C.size[FN_BI] = BODY;
    C.size[FN_H1] = BODY * 1.75; C.size[FN_H2] = BODY * 1.4; C.size[FN_H3] = BODY * 1.18; C.size[FN_CODE] = BODY * 0.9;
    int map[FN_COUNT] = {[FN_REG] = 0, [FN_B] = 1, [FN_I] = 2, [FN_BI] = 3, [FN_H1] = 1, [FN_H2] = 1, [FN_H3] = 1, [FN_CODE] = 4};
    memcpy(C.pf, map, sizeof map);
    Measurer M = {pdf_adv, &C};

    const double PW = 595.28, PH = 841.89, MX = 72, MT = 72, MB = 72;
    const int W = (int)lround((PW - 2 * MX) * 100);
    Buf *pages = NULL;
    int npages = 0;
    Buf *pb = NULL;
    double y = 0;
    Para P = {0};
    uint16_t *st = NULL;
    int stcap = 0;
    uint8_t hst = 0;
    bool markdown = !strcmp(lang_name(d->lang), "Markdown");

    void new_page(void) {
        pages = xrealloc(pages, (npages + 1) * sizeof(Buf));
        pages[npages] = (Buf){0};
        pb = &pages[npages++];
        y = PH - MT;
        char num[16];
        int nn = snprintf(num, sizeof num, "%d", npages);
        PFont *f = C.f[0];
        double w = 0;
        for (int k = 0; k < nn; k++) w += pdf_adv(&C, FN_REG, num[k]) / 100.0 * 9 / BODY;
        bprintf(pb, "0.55 0.55 0.55 rg BT /F%d 9 Tf 1 0 0 1 %.2f %.2f Tm <", (int)(f - fonts), (PW - w) / 2, MB / 2);
        for (int k = 0; k < nn; k++) bprintf(pb, "%04X", pdf_gid(f, num[k]));
        bprintf(pb, "> Tj ET\n");
    }
    void run(int font, double x, double yy, const char *s, int n, const uint16_t *sty, const char *rgb) {
        PFont *f = C.f[C.pf[font]];
        bprintf(pb, "%s rg BT /F%d %.2f Tf 1 0 0 1 %.2f %.2f Tm <", rgb, (int)(f - fonts), C.size[font], x, yy);
        for (int k = 0; k < n;) {
            int cl;
            uint32_t cp = utf8_dec(s + k, n - k, &cl);
            if (!(sty[k] & ST_HIDE)) bprintf(pb, "%04X", pdf_gid(f, cp == '\t' ? ' ' : cp));
            k += cl;
        }
        bprintf(pb, "> Tj ET\n");
    }
    new_page();
    for (int li = 0; li < d->nlines; li++) {
        Line *l = &d->lines[li];
        uint8_t before = hst;
        hst = hl_line(d->lang, l->s, l->len, hst, NULL);
        if (stcap < l->len + 1) { stcap = l->len + 256; st = xrealloc(st, stcap * sizeof(uint16_t)); }
        P.st = st;
        int f0 = 0;
        while (f0 < l->len && l->s[f0] == ' ') f0++;
        bool code = markdown && (before == 5 || (l->len - f0 >= 3 && (!memcmp(l->s + f0, "```", 3) || !memcmp(l->s + f0, "~~~", 3))));
        if (code) {
            P.kind = BK_BODY; P.prefix = P.level = 0;
            for (int k = 0; k < l->len; k++) st[k] = ST_CODE;
            if (l->len - f0 >= 3 && (!memcmp(l->s + f0, "```", 3) || !memcmp(l->s + f0, "~~~", 3)))
                for (int k = 0; k < l->len; k++) st[k] |= ST_HIDE; /* fence markers aren't printed */
        } else para_parse(l->s, l->len, &P, false);
        int k = P.kind;
        bool head = k >= BK_H1 && k <= BK_H3;
        int font = code ? FN_CODE : k == BK_H1 ? FN_H1 : k == BK_H2 ? FN_H2 : k == BK_H3 ? FN_H3 : FN_REG;
        PFont *pf = C.f[C.pf[font]];
        double fs = C.size[font];
        double bh = (double)pf->face->height / pf->upem * fs, asc = (double)pf->face->ascender / pf->upem * fs;
        if (code) { bh = (double)C.f[0]->face->height / C.f[0]->upem * BODY; }
        double lh = bh * (head ? 1.05 : code ? 1.0 : d->spacing / 100.0);
        double body_h = (double)C.f[0]->face->height / C.f[0]->upem * BODY;
        double sb = li == 0 ? 0 : k == BK_H1 ? body_h : k == BK_H2 ? body_h * .75 : k == BK_H3 ? body_h * .5 : 0;
        double sa = head ? body_h / 4 : code ? 0 : d->para_sp;
        double left = 0, first = 0;
        if (k == BK_BULLET || k == BK_NUMBER) left = BODY * 1.7 * (P.level + 1);
        else if (k == BK_QUOTE) left = BODY * 1.4;
        else if (code) left = 8;
        else if (k == BK_BODY && d->indent && l->len) first = BODY * 2;
        int Wp = W - (int)lround(left * 100) - (code ? 800 : 0);
        para_wrap(l->s, l->len, &P, Wp - (int)lround(first * 100), Wp, &M);
        if (y < PH - MT - 0.01) y -= sb;
        bool all_hidden = true;
        for (int q = 0; q < l->len; q++) if (!(st[q] & ST_HIDE)) all_hidden = false;
        if (code && all_hidden) continue; /* fence line */
        for (int r = 0; r < P.nrows; r++) {
            if (y - lh < MB) new_page();
            double top = y;
            double base = y - (lh - bh) / 2 - asc;
            DRow *row = &P.rows[r];
            double avail = Wp / 100.0 - (r == 0 ? first : 0);
            double extra = avail - row->w / 100.0, jx = 0, off = 0;
            int al = d->align;
            if (head && al == AL_JUSTIFY) al = AL_LEFT;
            if (!code && extra > 0) {
                if (al == AL_CENTER) off = extra / 2;
                else if (al == AL_RIGHT) off = extra;
                else if (al == AL_JUSTIFY && r + 1 < P.nrows && row->nsp) jx = extra / row->nsp;
            }
            double x0 = MX + left + (r == 0 ? first : 0) + off;
            if (code) bprintf(pb, "0.95 0.95 0.95 rg %.2f %.2f %.2f %.2f re f\n", MX, top - lh, PW - 2 * MX, lh);
            if (k == BK_QUOTE) bprintf(pb, "0.7 0.7 0.7 rg %.2f %.2f 2.5 %.2f re f\n", MX + 2, top - lh, lh);
            if (k == BK_RULE) bprintf(pb, "0.6 0.6 0.6 RG 0.6 w %.2f %.2f m %.2f %.2f l S\n", MX, top - lh / 2, PW - MX, top - lh / 2);
            if (r == 0 && (k == BK_BULLET || k == BK_NUMBER)) {
                char m[16];
                int mn;
                if (k == BK_BULLET) mn = snprintf(m, sizeof m, "%s", FT_Get_Char_Index(C.f[0]->face, 0x2022) ? "•" : "-");
                else mn = snprintf(m, sizeof m, "%d.", P.number);
                double mw = 0;
                for (int q = 0; q < mn;) { int cl; uint32_t cp = utf8_dec(m + q, mn - q, &cl); mw += pdf_adv(&C, FN_REG, cp) / 100.0; q += cl; }
                uint16_t z[16] = {0};
                run(FN_REG, MX + left - mw - BODY / 2, base, m, mn, z, "0.2 0.2 0.2");
            }
            /* glyph runs: split at spaces and style changes */
            double x = x0;
            int rs = -1, rf = -1;
            double rx = 0;
            const char *rcol = NULL;
            for (int i = row->s; i <= row->e;) {
                int cl = 1;
                uint32_t cp = i < row->e ? utf8_dec(l->s + i, l->len - i, &cl) : ' ';
                bool hidden = i < row->e && (st[i] & ST_HIDE);
                int f = i < row->e ? para_font(&P, i) : -1;
                uint16_t sv = i < row->e ? st[i] : 0;
                const char *col = (sv & ST_LINK) ? "0.10 0.30 0.70" : (sv & ST_CODE) ? "0.25 0.25 0.25"
                                : k == BK_QUOTE ? "0.35 0.35 0.35" : head ? "0.12 0.12 0.12" : "0.05 0.05 0.05";
                bool brk = i == row->e || cp == ' ' || f != rf || (rcol && strcmp(col, rcol));
                if (brk && rs >= 0) { run(rf, rx, base, l->s + rs, i - rs, st + rs, rcol); rs = -1; }
                if (i == row->e) break;
                if (hidden) { i += cl; continue; }
                double w = pdf_adv(&C, f, cp == '\t' ? ' ' : cp) / 100.0 * (cp == '\t' ? 4 : 1);
                if ((sv & ST_CODE) && !code) bprintf(pb, "0.93 0.93 0.93 rg %.2f %.2f %.2f %.2f re f\n", x, base - fs * 0.25, w, fs * 1.15);
                if (sv & (ST_U | ST_LINK)) bprintf(pb, "%s rg %.2f %.2f %.2f 0.6 re f\n", col, x, base - 1.8, w + (cp == ' ' ? jx : 0));
                if (sv & ST_S) bprintf(pb, "%s rg %.2f %.2f %.2f 0.6 re f\n", col, x, base + asc * 0.3, w);
                if (cp != ' ' && cp != '\t') {
                    if (rs < 0) { rs = i; rf = f; rx = x; rcol = col; }
                }
                x += w + (cp == ' ' ? jx : 0);
                i += cl;
            }
            y -= lh;
        }
        y -= sa;
    }

    /* assemble */
    Buf out = {0};
    int font_base = 3, info = font_base + 5 * nf, page0 = info + 1;
    int nobj = page0 + 2 * npages - 1;
    long *off = calloc(nobj + 2, sizeof(long));
#define OBJN(id_) (off[id_] = (long)out.n, bprintf(&out, "%d 0 obj\n", id_))
    void stream(int id, const char *dict, const void *data, size_t len) {
        OBJN(id);
        uLongf zn = compressBound(len);
        unsigned char *z = xmalloc(zn);
        compress2(z, &zn, data, len, 9);
        bprintf(&out, "<< %s /Length %lu /Filter /FlateDecode >>\nstream\n", dict, (unsigned long)zn);
        bput(&out, z, zn);
        free(z);
        bprintf(&out, "\nendstream\nendobj\n");
    }
    bprintf(&out, "%%PDF-1.5\n%%\xe2\xe3\xcf\xd3\n");
    OBJN(1); bprintf(&out, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    OBJN(2); bprintf(&out, "<< /Type /Pages /Count %d /Kids [", npages);
    for (int p = 0; p < npages; p++) bprintf(&out, " %d 0 R", page0 + 2 * p);
    bprintf(&out, " ] >>\nendobj\n");
    for (int k = 0; k < nf; k++) {
        PFont *f = &fonts[k];
        int b = font_base + 5 * k;
        int up = f->upem;
#define FU2(v) ((int)((long)(v) * 1000 / up))
        OBJN(b); bprintf(&out, "<< /Type /Font /Subtype /Type0 /BaseFont /NoctDoc%d /Encoding /Identity-H /DescendantFonts [%d 0 R] /ToUnicode %d 0 R >>\nendobj\n", k, b + 1, b + 4);
        OBJN(b + 1); bprintf(&out, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /NoctDoc%d /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
                                   "/FontDescriptor %d 0 R /CIDToGIDMap /Identity /W [", k, b + 2);
        for (int g = 0; g < 65536; g++) {
            if (!f->used[g]) continue;
            FT_Fixed a = 0;
            FT_Get_Advance(f->face, g, FT_LOAD_NO_SCALE, &a);
            bprintf(&out, " %d [%d]", g, FU2(a));
        }
        bprintf(&out, " ] >>\nendobj\n");
        TT_OS2 *os2 = FT_Get_Sfnt_Table(f->face, FT_SFNT_OS2);
        OBJN(b + 2); bprintf(&out, "<< /Type /FontDescriptor /FontName /NoctDoc%d /Flags 32 /FontBBox [%d %d %d %d] /ItalicAngle 0 /Ascent %d /Descent %d "
                                   "/CapHeight %d /StemV 80 /FontFile2 %d 0 R >>\nendobj\n", k,
                         FU2(f->face->bbox.xMin), FU2(f->face->bbox.yMin), FU2(f->face->bbox.xMax), FU2(f->face->bbox.yMax),
                         FU2(f->face->ascender), FU2(f->face->descender), os2 && os2->sCapHeight ? FU2(os2->sCapHeight) : FU2(f->face->ascender), b + 3);
#undef FU2
        Buf fd = {0}, sub = {0};
        FILE *ff = fopen(f->path, "rb");
        if (ff) { char tmp[65536]; size_t r; while ((r = fread(tmp, 1, sizeof tmp, ff)) > 0) bput(&fd, tmp, r); fclose(ff); }
        Buf *emb = subset_ttf((const uint8_t *)fd.p, fd.n, f->used, &sub) ? &sub : &fd;
        char ld[64];
        snprintf(ld, sizeof ld, "/Length1 %zu", emb->n);
        stream(b + 3, ld, emb->p, emb->n);
        free(fd.p);
        free(sub.p);
        Buf cm = {0};
        bprintf(&cm, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                     "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
        int cnt = 0;
        for (int g = 1; g < 65536; g++) cnt += f->used[g];
        int done = 0;
        for (int g = 1; g < 65536 && done < cnt;) {
            int blk = MIN(100, cnt - done);
            bprintf(&cm, "%d beginbfchar\n", blk);
            for (int q = 0; q < blk; g++) {
                if (!f->used[g]) continue;
                uint32_t cp = f->gid_cp[g];
                if (cp > 0xFFFF) { cp -= 0x10000; bprintf(&cm, "<%04X> <%04X%04X>\n", g, 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)); }
                else bprintf(&cm, "<%04X> <%04X>\n", g, cp);
                q++;
            }
            bprintf(&cm, "endbfchar\n");
            done += blk;
        }
        bprintf(&cm, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
        stream(b + 4, "", cm.p, cm.n);
        free(cm.p);
    }
    const char *name = d->path ? strrchr(d->path, '/') : NULL;
    name = name ? name + 1 : doc_name(d);
    OBJN(info);
    bprintf(&out, "<< /Producer (Noctedit " NOCTEDIT_VERSION ") /Title (");
    for (const char *p = name; *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') bput(&out, "\\", 1);
        if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) bput(&out, p, 1);
    }
    bprintf(&out, ") >>\nendobj\n");
    Buf fres = {0};
    for (int k = 0; k < nf; k++) bprintf(&fres, "/F%d %d 0 R ", k, font_base + 5 * k);
    bput(&fres, "", 1);
    for (int p = 0; p < npages; p++) {
        OBJN(page0 + 2 * p);
        bprintf(&out, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /Font << %s>> >> /Contents %d 0 R >>\nendobj\n",
                PW, PH, fres.p, page0 + 2 * p + 1);
        stream(page0 + 2 * p + 1, "", pages[p].p, pages[p].n);
    }
#undef OBJN
    long xref = (long)out.n;
    bprintf(&out, "xref\n0 %d\n0000000000 65535 f \n", nobj + 1);
    for (int n = 1; n <= nobj; n++) bprintf(&out, "%010ld 00000 n \n", off[n]);
    bprintf(&out, "trailer\n<< /Size %d /Root 1 0 R /Info %d 0 R >>\nstartxref\n%ld\n%%%%EOF\n", nobj + 1, info, xref);

    bool ok = false;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) snprintf(err, errn, "%s", strerror(errno));
    else {
        size_t w = 0;
        while (w < out.n) {
            ssize_t r = write(fd, out.p + w, out.n - w);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            w += r;
        }
        ok = w == out.n && close(fd) == 0;
        if (!ok) snprintf(err, errn, "write failed: %s", strerror(errno));
    }
    for (int p = 0; p < npages; p++) free(pages[p].p);
    free(pages);
    free(out.p);
    free(fres.p);
    free(off);
    free(st);
    free(P.rows);
    for (int k = 0; k < nf; k++) { FT_Done_Face(fonts[k].face); free(fonts[k].path); free(fonts[k].used); free(fonts[k].gid_cp); }
    FT_Done_FreeType(ft);
    return ok;
}
