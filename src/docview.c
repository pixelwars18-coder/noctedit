/* Document-mode engine shared by the screen renderer and the PDF exporter:
 *  - block parsing (headings, lists, quotes, rules) from Markdown prefixes
 *  - inline styles (**bold**, *italic*, <u>underline</u>, ~~strike~~, `code`, [links](url))
 *  - word-wrapping a paragraph with per-glyph fonts, hidden markup and alignment data. */
#include "noctedit.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int block_parse(const char *s, int n, Para *p) {
    int i = 0;
    while (i < n && s[i] == ' ') i++;
    int lead = i;
    p->level = lead / 2;
    p->number = 0;
    p->kind = BK_BODY;
    /* horizontal rule: 3+ of the same -*_ with optional spaces */
    if (i < n && (s[i] == '-' || s[i] == '*' || s[i] == '_')) {
        char c = s[i];
        int cnt = 0, j = i;
        while (j < n && (s[j] == c || s[j] == ' ')) { cnt += s[j] == c; j++; }
        if (j == n && cnt >= 3) { p->kind = BK_RULE; return n; }
    }
    if (i < n && s[i] == '#') {
        int h = 0, j = i;
        while (j < n && s[j] == '#' && h < 6) { h++; j++; }
        if (j == n || s[j] == ' ') {
            p->kind = h == 1 ? BK_H1 : h == 2 ? BK_H2 : BK_H3;
            p->level = 0;
            return j < n ? j + 1 : j;
        }
    }
    if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') { p->kind = BK_BULLET; return i + 2; }
    if (i < n && isdigit((unsigned char)s[i])) {
        int j = i, num = 0;
        while (j < n && isdigit((unsigned char)s[j]) && j - i < 9) num = num * 10 + (s[j++] - '0');
        if (j + 1 < n && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ') { p->kind = BK_NUMBER; p->number = num; return j + 2; }
    }
    if (i < n && s[i] == '>') { p->kind = BK_QUOTE; p->level = 0; return i + 1 + (i + 1 < n && s[i + 1] == ' '); }
    p->level = 0;
    return 0;
}

static bool free_at(const uint16_t *st, int a, int b) {
    for (int k = a; k < b; k++) if (st[k] & (ST_MARK | ST_CODE | ST_LIT)) return false;
    return true;
}

/* pair up open/close markers on one line and style what's between them */
static void span(const char *s, int n, uint16_t *st, int from, const char *open, const char *close, uint16_t flag, int flank) {
    int ol = (int)strlen(open), cl = (int)strlen(close);
    for (int i = from; i + ol <= n; i++) {
        if (memcmp(s + i, open, ol) || !free_at(st, i, i + ol)) continue;
        if (flank) {
            if (i + ol >= n || s[i + ol] == ' ') continue;
            if (flank == 2 && i > 0 && isalnum((unsigned char)s[i - 1])) continue; /* snake_case */
            if (ol == 1 && i + 1 < n && s[i + 1] == open[0]) continue;              /* part of ** */
        }
        int j = -1;
        for (int k = i + ol + (flank ? 1 : 0); k + cl <= n; k++) {
            if (memcmp(s + k, close, cl) || !free_at(st, k, k + cl)) continue;
            if (flank) {
                if (s[k - 1] == ' ') continue;
                if (flank == 2 && k + cl < n && isalnum((unsigned char)s[k + cl])) continue;
                if (cl == 1 && k + 1 < n && s[k + 1] == close[0]) { k++; continue; }
            }
            j = k;
            break;
        }
        if (j < 0) continue;
        for (int k = i; k < i + ol; k++) st[k] |= ST_MARK;
        for (int k = j; k < j + cl; k++) st[k] |= ST_MARK;
        for (int k = i + ol; k < j; k++) st[k] |= flag;
        i += ol - 1;
    }
}

void para_parse(const char *s, int n, Para *p, bool show_markup) {
    memset(p->st, 0, n * sizeof(uint16_t));
    p->prefix = block_parse(s, n, p);
    int from = p->prefix;
    if (p->kind == BK_RULE) {
        for (int k = 0; k < n; k++) p->st[k] = ST_MARK | (show_markup ? 0 : ST_HIDE);
        return;
    }
    for (int k = 0; k < p->prefix; k++) p->st[k] |= ST_MARK;
    /* escapes */
    for (int i = from; i + 1 < n; i++)
        if (s[i] == '\\' && ispunct((unsigned char)s[i + 1])) { p->st[i] |= ST_MARK; p->st[i + 1] |= ST_LIT; i++; }
    /* code spans */
    for (int i = from; i < n; i++) {
        if (s[i] != '`' || (p->st[i] & (ST_MARK | ST_LIT))) continue;
        int j = i + 1;
        while (j < n && s[j] != '`') j++;
        if (j >= n) break;
        p->st[i] |= ST_MARK;
        p->st[j] |= ST_MARK;
        for (int k = i + 1; k < j; k++) p->st[k] |= ST_CODE;
        i = j;
    }
    /* links: [text](url) → text underlined, the rest is markup */
    for (int i = from; i < n; i++) {
        if (s[i] != '[' || !free_at(p->st, i, i + 1)) continue;
        int j = i + 1;
        while (j < n && s[j] != ']') j++;
        if (j + 1 >= n || s[j + 1] != '(') continue;
        int k = j + 2;
        while (k < n && s[k] != ')') k++;
        if (k >= n) continue;
        p->st[i] |= ST_MARK;
        for (int m = j; m <= k; m++) p->st[m] |= ST_MARK;
        for (int m = i + 1; m < j; m++) p->st[m] |= ST_LINK;
        i = k;
    }
    span(s, n, p->st, from, "<u>", "</u>", ST_U, 0);
    span(s, n, p->st, from, "~~", "~~", ST_S, 1);
    span(s, n, p->st, from, "**", "**", ST_B, 1);
    span(s, n, p->st, from, "__", "__", ST_B, 2);
    span(s, n, p->st, from, "*", "*", ST_I, 1);
    span(s, n, p->st, from, "_", "_", ST_I, 2);
    if (!show_markup)
        for (int k = 0; k < n; k++) if (p->st[k] & ST_MARK) p->st[k] |= ST_HIDE;
}

int para_font(const Para *p, int i) {
    uint16_t f = p->st[i];
    if (f & ST_CODE) return FN_CODE;
    if (p->kind == BK_H1) return FN_H1;
    if (p->kind == BK_H2) return FN_H2;
    if (p->kind == BK_H3) return FN_H3;
    return ((f & ST_B) ? FN_B : FN_REG) | ((f & ST_I) ? FN_I : 0);
}

static int glyph_w(const char *s, int n, const Para *p, int i, uint32_t cp, const Measurer *m) {
    if (p->st[i] & ST_HIDE) return 0;
    if (cp == '\t') return 4 * m->adv(m->ctx, para_font(p, i), ' ');
    return m->adv(m->ctx, para_font(p, i), cp);
}

int para_measure(const char *s, int n, const Para *p, int a, int b, const Measurer *m) {
    int w = 0;
    for (int i = a; i < b;) {
        int cl;
        uint32_t cp = utf8_dec(s + i, n - i, &cl);
        w += glyph_w(s, n, p, i, cp, m);
        i += cl;
    }
    return w;
}

static void push_row(const char *s, int n, Para *p, int a, int b, const Measurer *m) {
    if (p->nrows == p->rcap) {
        p->rcap = p->rcap ? p->rcap * 2 : 16;
        p->rows = xrealloc(p->rows, p->rcap * sizeof(DRow));
    }
    int e = b;
    while (e > a && (s[e - 1] == ' ' || (p->st[e - 1] & ST_HIDE))) e--; /* trailing spaces don't count */
    int nsp = 0;
    for (int k = a; k < e; k++) if (s[k] == ' ' && !(p->st[k] & ST_HIDE)) nsp++;
    p->rows[p->nrows++] = (DRow){a, b, para_measure(s, n, p, a, e, m), nsp};
}

void para_wrap(const char *s, int n, Para *p, int w_first, int w_rest, const Measurer *m) {
    p->nrows = 0;
    int rs = 0, x = 0, brk = -1, W = w_first;
    for (int i = 0; i < n;) {
        int cl;
        uint32_t cp = utf8_dec(s + i, n - i, &cl);
        int w = glyph_w(s, n, p, i, cp, m);
        if (w > 0 && x + w > W && i > rs) {
            int b = brk > rs ? brk : i;
            push_row(s, n, p, rs, b, m);
            rs = b;
            brk = -1;
            W = w_rest;
            x = para_measure(s, n, p, rs, i, m);
            if (x + w > W && b == i) { x += w; i += cl; continue; } /* glyph wider than the column */
            continue;
        }
        x += w;
        if (cp == ' ') brk = i + cl;
        i += cl;
    }
    push_row(s, n, p, rs, n, m);
}

/* x offset of byte i inside row r, before alignment; jx = extra px per space when justified */
double para_row_x(const char *s, int n, const Para *p, const DRow *r, int i, double jx, const Measurer *m) {
    double x = 0;
    for (int k = r->s; k < i && k < n;) {
        int cl;
        uint32_t cp = utf8_dec(s + k, n - k, &cl);
        x += glyph_w(s, n, p, k, cp, m);
        if (cp == ' ' && !(p->st[k] & ST_HIDE)) x += jx;
        k += cl;
    }
    return x;
}

/* double-spaced plain text: every logical line is followed by one blank line */
bool doc_detect_double(const Doc *d) {
    if (d->nlines < 12) return false;
    int blank_odd = 0, odd = 0, text_even = 0, even = 0;
    int lens[4096], nl = 0;
    for (int i = 0; i < d->nlines; i++) {
        bool blank = d->lines[i].len == 0;
        if (i & 1) { odd++; blank_odd += blank; }
        else {
            even++;
            if (!blank) { text_even++; if (nl < 4096) lens[nl++] = d->lines[i].len; }
        }
    }
    /* allow a missing trailing blank */
    if (odd == 0 || blank_odd < odd - 1) return false; /* tolerate one stray line */
    if (text_even < 6 || text_even * 2 < even) return false;
    /* hard-wrapped typing (short lines) — not one-paragraph-per-line prose */
    int longer = 0;
    for (int k = 0; k < nl; k++) longer += lens[k] > 110;
    return longer * 4 < nl;
}

void doc_collapse_double(Doc *d) {
    int j = 0;
    bool tail = d->nlines % 2 == 0;
    for (int i = 0; i < d->nlines; i += 2) d->lines[j++] = d->lines[i];
    for (int i = 1; i < d->nlines; i += 2) if (d->lines[i].cap) free(d->lines[i].s);
    d->nlines = MAX(1, j);
    d->dbl = true;
    d->dbl_tail = tail;
    d->hl_valid = 0;
    d->version++;
}
