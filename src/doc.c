/* Document model: array of lines borrowing from one load block,
 * copy-on-write per line, grouped undo/redo, atomic saving. */
#include "noctedit.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int untitled_counter;

Doc *doc_new(void) {
    Doc *d = calloc(1, sizeof *d);
    d->lcap = 16;
    d->lines = calloc(d->lcap, sizeof(Line));
    d->nlines = 1;
    d->untitled_id = ++untitled_counter;
    d->tab_w = 4;
    d->use_spaces = true;
    d->want_x = -1;
    d->lang = lang_at(0);
    return d;
}

static void free_lines(Doc *d) {
    for (int i = 0; i < d->nlines; i++) if (d->lines[i].cap) free(d->lines[i].s);
    free(d->block);
    d->block = NULL;
}

static void free_undo(Doc *d, int from) {
    for (int i = from; i < d->nundo; i++) free(d->undo[i].text);
    d->nundo = from;
}

void doc_free(Doc *d) {
    if (!d) return;
    free_lines(d);
    free(d->lines);
    free_undo(d, 0);
    free(d->undo);
    free(d->hl);
    free(d->path);
    free(d);
}

bool doc_modified(const Doc *d) { return d->undo_pos != d->saved_pos; }

const char *doc_name(const Doc *d) {
    static char buf[64];
    if (d->path) {
        const char *s = strrchr(d->path, '/');
        return s ? s + 1 : d->path;
    }
    snprintf(buf, sizeof buf, "Untitled %d", d->untitled_id);
    return buf;
}

long long file_mtime_ns(const char *path) {
    struct stat st;
    if (stat(path, &st)) return 0;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

void doc_detect_indent(Doc *d) {
    int tabs = 0, spaces = 0, hist[9] = {0};
    int prev = 0;
    for (int i = 0; i < d->nlines && i < 5000; i++) {
        Line *l = &d->lines[i];
        if (!l->len) continue;
        if (l->s[0] == '\t') { tabs++; continue; }
        int n = 0;
        while (n < l->len && l->s[n] == ' ') n++;
        if (n == l->len) continue;
        if (n) spaces++;
        int delta = abs(n - prev);
        if (delta >= 2 && delta <= 8) hist[delta]++;
        prev = n;
    }
    if (tabs > spaces) { d->use_spaces = false; return; }
    if (spaces) {
        d->use_spaces = true;
        int best = 0;
        for (int w = 2; w <= 8; w++) if (hist[w] > hist[best]) best = w;
        if (best == 2 || best == 4 || best == 8) d->tab_w = best;
    }
}

void doc_set_text(Doc *d, char *buf, size_t size) {
    int n = 1;
    for (size_t i = 0; i < size; i++) n += buf[i] == '\n';
    free_lines(d);
    if (n > d->lcap) {
        d->lcap = n + 16;
        d->lines = xrealloc(d->lines, d->lcap * sizeof(Line));
    }
    char *nl = memchr(buf, '\n', size);
    d->crlf = nl && nl > buf && nl[-1] == '\r';
    d->block = buf;
    size_t start = 0;
    int li = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || buf[i] == '\n') {
            int len = (int)(i - start);
            if (d->crlf && len && buf[start + len - 1] == '\r' && i < size) len--;
            d->lines[li++] = (Line){buf + start, len, 0};
            start = i + 1;
        }
    }
    d->nlines = li;
    free_undo(d, 0);
    d->undo_pos = d->saved_pos = 0;
    d->cur = d->anc = (Pos){0, 0};
    d->top_line = d->top_row = d->scroll_x = 0;
    d->want_x = -1;
    d->hl_valid = 0;
    d->dbl = d->dbl_tail = false;
    d->top_px = 0;
    d->version++;
}

bool doc_load(Doc *d, const char *path, char *err, int errn) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errn, "%s", strerror(errno)); return false; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        snprintf(err, errn, "not a regular file");
        return false;
    }
    size_t size = st.st_size;
    char *buf = xmalloc(size + 1);
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, buf + got, size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    size = got;
    if (memchr(buf, 0, MIN(size, (size_t)65536))) {
        free(buf);
        snprintf(err, errn, "looks like a binary file");
        return false;
    }
    doc_set_text(d, buf, size);
    if (d->path != path) { free(d->path); d->path = realpath(path, NULL); if (!d->path) d->path = xstrdup(path); }
    d->mtime_ns = file_mtime_ns(path);
    d->on_disk_changed = false;
    d->lang = lang_detect(path, d->lines[0].s, d->lines[0].len);
    doc_detect_indent(d);
    return true;
}

static bool write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        s += w;
        n -= w;
    }
    return true;
}

static bool write_doc(Doc *d, int fd) {
    /* buffered writes to keep syscalls low without duplicating the file in RAM */
    char buf[65536];
    size_t bl = 0;
    /* double-spaced files keep their blank line after every line on disk */
    const char *eol = d->dbl ? (d->crlf ? "\r\n\r\n" : "\n\n") : d->crlf ? "\r\n" : "\n";
    size_t el = (d->crlf ? 2 : 1) * (d->dbl ? 2 : 1);
    for (int i = 0; i < d->nlines; i++) {
        Line *l = &d->lines[i];
        const char *parts[2] = {l->s, eol};
        size_t lens[2] = {(size_t)l->len, i + 1 < d->nlines ? el : 0};
        for (int k = 0; k < 2; k++) {
            if (bl + lens[k] > sizeof buf) {
                if (!write_all(fd, buf, bl)) return false;
                bl = 0;
            }
            if (lens[k] > sizeof buf) {
                if (!write_all(fd, parts[k], lens[k])) return false;
            } else {
                memcpy(buf + bl, parts[k], lens[k]);
                bl += lens[k];
            }
        }
    }
    if (d->dbl && d->dbl_tail) {
        if (bl + 2 > sizeof buf) { if (!write_all(fd, buf, bl)) return false; bl = 0; }
        memcpy(buf + bl, eol, el / 2);
        bl += el / 2;
    }
    return write_all(fd, buf, bl);
}

bool doc_save(Doc *d, const char *path, char *err, int errn) {
    struct stat st;
    bool exists = stat(path, &st) == 0;
    mode_t mode = exists ? (st.st_mode & 07777) : 0644;
    bool ok = false;

    /* Atomic save via a temp file in the same directory, unless the target is
     * a symlink/hardlink or the directory is not writable — then write in place. */
    struct stat lst;
    bool link = exists && (lstat(path, &lst) == 0 && (S_ISLNK(lst.st_mode) || st.st_nlink > 1));
    char *dup = xstrdup(path);
    char *dir = dirname(dup);
    size_t tl = strlen(path) + 32;
    char *tmp = xmalloc(tl);
    snprintf(tmp, tl, "%s.noctedit~%d", path, (int)getpid());
    if (!link && access(dir, W_OK) == 0) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        if (fd >= 0) {
            if (exists && fchown(fd, st.st_uid, st.st_gid)) { /* keep our ownership */ }
            fchmod(fd, mode);
            ok = write_doc(d, fd) && fsync(fd) == 0;
            ok = close(fd) == 0 && ok;
            if (ok && rename(tmp, path) != 0) ok = false;
            if (!ok) unlink(tmp);
        }
    }
    if (!ok) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        if (fd < 0) {
            snprintf(err, errn, "%s", strerror(errno));
        } else {
            ok = write_doc(d, fd);
            if (!ok) snprintf(err, errn, "%s", strerror(errno));
            if (close(fd) != 0 && ok) { ok = false; snprintf(err, errn, "%s", strerror(errno)); }
        }
    }
    free(dup);
    free(tmp);
    if (!ok) return false;
    if (!d->path || strcmp(d->path, path)) {
        free(d->path);
        d->path = realpath(path, NULL);
        if (!d->path) d->path = xstrdup(path);
        const Lang *l = lang_detect(d->path, d->lines[0].s, d->lines[0].len);
        if (l != lang_at(0) || !d->lang) { d->lang = l; d->hl_valid = 0; }
    }
    d->saved_pos = d->undo_pos;
    d->mtime_ns = file_mtime_ns(d->path);
    d->on_disk_changed = false;
    return true;
}

/* ------------------------------------------------------------------ text access */
Pos doc_clamp(Doc *d, Pos p) {
    p.line = CLAMP(p.line, 0, d->nlines - 1);
    p.col = CLAMP(p.col, 0, d->lines[p.line].len);
    return p;
}
bool doc_has_sel(const Doc *d) { return pos_cmp(d->cur, d->anc) != 0; }
void doc_sel(const Doc *d, Pos *a, Pos *b) {
    if (pos_cmp(d->cur, d->anc) <= 0) { *a = d->cur; *b = d->anc; }
    else { *a = d->anc; *b = d->cur; }
}

char *doc_text(Doc *d, Pos a, Pos b, int *n) {
    size_t total = 0;
    for (int i = a.line; i <= b.line; i++) {
        int s = i == a.line ? a.col : 0, e = i == b.line ? b.col : d->lines[i].len;
        total += e - s + (i < b.line);
    }
    char *out = xmalloc(total + 1), *p = out;
    for (int i = a.line; i <= b.line; i++) {
        int s = i == a.line ? a.col : 0, e = i == b.line ? b.col : d->lines[i].len;
        memcpy(p, d->lines[i].s + s, e - s);
        p += e - s;
        if (i < b.line) *p++ = '\n';
    }
    *p = 0;
    if (n) *n = (int)total;
    return out;
}

static void line_reserve(Line *l, int need) {
    if (l->cap == 0) { /* borrowed: copy out */
        int cap = MAX(need, 8);
        char *s = xmalloc(cap);
        memcpy(s, l->s, l->len);
        l->s = s;
        l->cap = cap;
    } else if (need > l->cap) {
        l->cap = MAX(need, l->cap + l->cap / 2);
        l->s = xrealloc(l->s, l->cap);
    }
}

static void lines_insert(Doc *d, int at, int count) {
    if (d->nlines + count > d->lcap) {
        d->lcap = MAX(d->nlines + count, d->lcap * 2);
        d->lines = xrealloc(d->lines, d->lcap * sizeof(Line));
    }
    memmove(d->lines + at + count, d->lines + at, (d->nlines - at) * sizeof(Line));
    for (int i = 0; i < count; i++) d->lines[at + i] = (Line){NULL, 0, 0};
    d->nlines += count;
}

static void touched(Doc *d, int line) {
    if (d->hl_valid > line) d->hl_valid = line;
    d->version++;
}

static Pos raw_insert(Doc *d, Pos at, const char *s, int n) {
    touched(d, at.line);
    Line *l = &d->lines[at.line];
    int nl = 0;
    for (int i = 0; i < n; i++) nl += s[i] == '\n';
    if (!nl) {
        line_reserve(l, l->len + n);
        memmove(l->s + at.col + n, l->s + at.col, l->len - at.col);
        memcpy(l->s + at.col, s, n);
        l->len += n;
        return (Pos){at.line, at.col + n};
    }
    /* split: tail of the current line goes after the inserted text */
    int tail_len = l->len - at.col;
    char *tail = xstrndup(l->s + at.col, tail_len);
    lines_insert(d, at.line + 1, nl);
    l = &d->lines[at.line];
    const char *p = s, *end = s + n;
    const char *e = memchr(p, '\n', end - p);
    l->len = at.col;
    line_reserve(l, at.col + (int)(e - p));
    memcpy(l->s + at.col, p, e - p);
    l->len = at.col + (int)(e - p);
    int li = at.line;
    p = e + 1;
    while (1) {
        li++;
        e = memchr(p, '\n', end - p);
        if (!e) e = end;
        Line *nlp = &d->lines[li];
        int seg = (int)(e - p);
        bool last = e == end;
        int need = seg + (last ? tail_len : 0);
        nlp->cap = MAX(need, 8);
        nlp->s = xmalloc(nlp->cap);
        memcpy(nlp->s, p, seg);
        nlp->len = seg;
        if (last) {
            memcpy(nlp->s + seg, tail, tail_len);
            nlp->len += tail_len;
            free(tail);
            return (Pos){li, seg};
        }
        p = e + 1;
    }
}

static void raw_delete(Doc *d, Pos a, Pos b) {
    touched(d, a.line);
    Line *la = &d->lines[a.line];
    if (a.line == b.line) {
        if (la->cap == 0) line_reserve(la, la->len);
        memmove(la->s + a.col, la->s + b.col, la->len - b.col);
        la->len -= b.col - a.col;
        return;
    }
    Line *lb = &d->lines[b.line];
    int tail = lb->len - b.col;
    line_reserve(la, a.col + tail);
    memcpy(la->s + a.col, lb->s + b.col, tail);
    la->len = a.col + tail;
    for (int i = a.line + 1; i <= b.line; i++) if (d->lines[i].cap) free(d->lines[i].s);
    memmove(d->lines + a.line + 1, d->lines + b.line + 1, (d->nlines - b.line - 1) * sizeof(Line));
    d->nlines -= b.line - a.line;
}

/* ------------------------------------------------------------------ undo */
void doc_begin(Doc *d, int kind, uint64_t now) {
    bool merge = kind != EK_OTHER && kind == d->last_kind && now - d->last_edit_ms < 1200 &&
                 d->undo_pos == d->nundo && d->undo_pos > 0 && d->undo_pos != d->saved_pos;
    if (!merge) d->group++;
    d->last_kind = kind;
    d->last_edit_ms = now;
}

static UndoRec *push_rec(Doc *d) {
    if (d->undo_pos < d->nundo) {
        free_undo(d, d->undo_pos);
        if (d->saved_pos > d->undo_pos) d->saved_pos = -1;
    }
    if (d->nundo == d->undo_cap) {
        d->undo_cap = d->undo_cap ? d->undo_cap * 2 : 64;
        d->undo = xrealloc(d->undo, d->undo_cap * sizeof(UndoRec));
    }
    UndoRec *r = &d->undo[d->nundo++];
    d->undo_pos = d->nundo;
    memset(r, 0, sizeof *r);
    r->group = d->group;
    r->cur = d->cur;
    r->anc = d->anc;
    return r;
}

Pos doc_insert(Doc *d, Pos at, const char *s, int n) {
    if (n <= 0 || d->readonly) return at;
    /* merge contiguous typing into the previous insert record */
    if (d->undo_pos == d->nundo && d->nundo && d->undo_pos != d->saved_pos) {
        UndoRec *p = &d->undo[d->nundo - 1];
        if (p->group == d->group && p->type == 0 && pos_cmp(p->b, at) == 0 && !memchr(s, '\n', n)) {
            p->text = xrealloc(p->text, p->n + n + 1);
            memcpy(p->text + p->n, s, n);
            p->n += n;
            p->b = raw_insert(d, at, s, n);
            return p->b;
        }
    }
    UndoRec *r = push_rec(d);
    r->type = 0;
    r->a = at;
    r->text = xstrndup(s, n);
    r->n = n;
    r->b = raw_insert(d, at, s, n);
    return r->b;
}

void doc_delete(Doc *d, Pos a, Pos b) {
    if (d->readonly) return;
    if (pos_cmp(a, b) > 0) { Pos t = a; a = b; b = t; }
    if (pos_cmp(a, b) == 0) return;
    int n;
    char *t = doc_text(d, a, b, &n);
    UndoRec *r = push_rec(d);
    r->type = 1;
    r->a = a;
    r->b = b;
    r->text = t;
    r->n = n;
    raw_delete(d, a, b);
}

bool doc_undo(Doc *d) {
    if (d->undo_pos == 0) return false;
    uint32_t g = d->undo[d->undo_pos - 1].group;
    UndoRec *r = NULL;
    while (d->undo_pos > 0 && d->undo[d->undo_pos - 1].group == g) {
        r = &d->undo[--d->undo_pos];
        if (r->type == 0) raw_delete(d, r->a, r->b);
        else raw_insert(d, r->a, r->text, r->n);
    }
    d->cur = doc_clamp(d, r->cur);
    d->anc = doc_clamp(d, r->anc);
    d->last_kind = EK_OTHER;
    d->want_x = -1;
    return true;
}

bool doc_redo(Doc *d) {
    if (d->undo_pos >= d->nundo) return false;
    uint32_t g = d->undo[d->undo_pos].group;
    Pos end = d->cur;
    while (d->undo_pos < d->nundo && d->undo[d->undo_pos].group == g) {
        UndoRec *r = &d->undo[d->undo_pos++];
        if (r->type == 0) end = raw_insert(d, r->a, r->text, r->n);
        else { raw_delete(d, r->a, r->b); end = r->a; }
    }
    d->cur = d->anc = doc_clamp(d, end);
    d->last_kind = EK_OTHER;
    d->want_x = -1;
    return true;
}
