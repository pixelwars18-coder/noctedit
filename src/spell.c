/* Spellcheck via Hunspell, loaded with dlopen() only when first needed, so the
 * editor has no hard dependency and pays nothing while spellcheck is unused.
 * Results are cached per word; a personal word list lives in
 * $XDG_CONFIG_HOME/noctedit/words. */
#include "noctedit.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct Hunhandle Hunhandle;
static struct {
    void *lib;
    Hunhandle *h;
    Hunhandle *(*create)(const char *, const char *);
    void (*destroy)(Hunhandle *);
    int (*spell)(Hunhandle *, const char *);
    int (*suggest)(Hunhandle *, char ***, const char *);
    void (*free_list)(Hunhandle *, char ***, int);
    int (*add)(Hunhandle *, const char *);
    bool tried, utf8;
    char lang[32];
    char status[160];
} H;

typedef struct { char *w; int8_t ok; } Entry;
static Entry *cache;
static int ccap, ccount;

static uint32_t hash(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void cache_clear(void) {
    for (int i = 0; i < ccap; i++) free(cache[i].w);
    free(cache);
    cache = NULL;
    ccap = ccount = 0;
}

static Entry *cache_slot(const char *w, int n) {
    if (!ccap) { ccap = 4096; cache = calloc(ccap, sizeof(Entry)); }
    uint32_t m = ccap - 1, i = hash(w, n) & m;
    while (cache[i].w && !(strncmp(cache[i].w, w, n) == 0 && cache[i].w[n] == 0)) i = (i + 1) & m;
    return &cache[i];
}

static void cache_put(const char *w, int n, int ok) {
    if ((ccount + 1) * 10 > ccap * 7) {
        if (ccap >= 65536) cache_clear(); /* bounded memory: start over */
        else {
            Entry *old = cache;
            int oc = ccap;
            ccap *= 2;
            cache = calloc(ccap, sizeof(Entry));
            for (int i = 0; i < oc; i++) if (old[i].w) *cache_slot(old[i].w, (int)strlen(old[i].w)) = old[i];
            free(old);
        }
    }
    Entry *e = cache_slot(w, n);
    if (!e->w) { e->w = xstrndup(w, n); ccount++; }
    e->ok = (int8_t)ok;
}

static void words_path(char *out, size_t n) {
    const char *x = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/noctedit/words", x);
    else snprintf(out, n, "%s/.config/noctedit/words", home ? home : "/tmp");
}

static bool find_dict(const char *lang, char *aff, char *dic, size_t n) {
    const char *home = getenv("HOME");
    char local[512];
    snprintf(local, sizeof local, "%s/.local/share/hunspell", home ? home : "");
    const char *dirs[] = {local, "/usr/share/hunspell", "/usr/share/myspell/dicts", "/usr/share/myspell", "/usr/local/share/hunspell"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(aff, n, "%s/%s.aff", dirs[i], lang);
        snprintf(dic, n, "%s/%s.dic", dirs[i], lang);
        if (access(aff, R_OK) == 0 && access(dic, R_OK) == 0) return true;
    }
    return false;
}

void spell_set_lang(const char *lang) {
    if (H.h) { H.destroy(H.h); H.h = NULL; }
    cache_clear();
    H.tried = false;
    snprintf(H.lang, sizeof H.lang, "%s", lang && *lang ? lang : "");
}

static bool load(void) {
    if (H.h) return true;
    if (H.tried) return false;
    H.tried = true;
    if (!H.lib) {
        const char *names[] = {"libhunspell-1.7.so.0", "libhunspell-1.7.so", "libhunspell.so", "libhunspell-1.6.so.0"};
        for (size_t i = 0; i < sizeof names / sizeof *names && !H.lib; i++) H.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (!H.lib) { snprintf(H.status, sizeof H.status, "Hunspell not installed (pacman -S hunspell hunspell-en_us)"); return false; }
        H.create = (Hunhandle * (*)(const char *, const char *)) dlsym(H.lib, "Hunspell_create");
        H.destroy = (void (*)(Hunhandle *))dlsym(H.lib, "Hunspell_destroy");
        H.spell = (int (*)(Hunhandle *, const char *))dlsym(H.lib, "Hunspell_spell");
        H.suggest = (int (*)(Hunhandle *, char ***, const char *))dlsym(H.lib, "Hunspell_suggest");
        H.free_list = (void (*)(Hunhandle *, char ***, int))dlsym(H.lib, "Hunspell_free_list");
        H.add = (int (*)(Hunhandle *, const char *))dlsym(H.lib, "Hunspell_add");
        if (!H.create || !H.spell || !H.suggest || !H.free_list || !H.destroy) {
            snprintf(H.status, sizeof H.status, "Unsupported Hunspell library");
            dlclose(H.lib);
            H.lib = NULL;
            return false;
        }
    }
    if (!H.lang[0]) {
        const char *l = getenv("LC_ALL");
        if (!l || !*l) l = getenv("LC_MESSAGES");
        if (!l || !*l) l = getenv("LANG");
        snprintf(H.lang, sizeof H.lang, "%s", l && *l && strcmp(l, "C") && strcmp(l, "POSIX") ? l : "en_US");
        char *dot = strpbrk(H.lang, ".@");
        if (dot) *dot = 0;
    }
    char aff[600], dic[600];
    if (!find_dict(H.lang, aff, dic, sizeof aff) && !find_dict("en_US", aff, dic, sizeof aff)) {
        snprintf(H.status, sizeof H.status, "No %s dictionary (pacman -S hunspell-en_us)", H.lang);
        return false;
    }
    H.h = H.create(aff, dic);
    if (!H.h) { snprintf(H.status, sizeof H.status, "Could not load %s", dic); return false; }
    /* dictionary encoding: only feed non-ASCII words to UTF-8 dictionaries */
    H.utf8 = false;
    FILE *f = fopen(aff, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "SET ", 4)) { H.utf8 = strstr(line, "UTF-8") || strstr(line, "UTF8") || strstr(line, "utf-8"); break; }
        fclose(f);
    }
    char wp[600];
    words_path(wp, sizeof wp);
    f = fopen(wp, "r");
    if (f && H.add) {
        char w[256];
        while (fgets(w, sizeof w, f)) {
            w[strcspn(w, "\r\n")] = 0;
            if (*w) H.add(H.h, w);
        }
    }
    if (f) fclose(f);
    const char *base = strrchr(dic, '/');
    snprintf(H.status, sizeof H.status, "%.*s", (int)(strlen(base + 1) - 4), base + 1);
    return true;
}

const char *spell_status(void) { load(); return H.status; }

void spell_unload(void) {
    if (!H.h) return;
    H.destroy(H.h);
    H.h = NULL;
    H.tried = false;
}
bool spell_ready(void) { return load(); }

int spell_check(const char *w, int n) {
    if (n <= 0 || n >= 100) return 1;
    Entry *e = ccap ? cache_slot(w, n) : NULL;
    if (e && e->w) return e->ok;
    if (!load()) return -1;
    bool ascii = true;
    for (int i = 0; i < n; i++) if ((unsigned char)w[i] >= 0x80) ascii = false;
    int ok = 1;
    if (ascii || H.utf8) {
        char buf[128];
        memcpy(buf, w, n);
        buf[n] = 0;
        ok = H.spell(H.h, buf) != 0;
    }
    cache_put(w, n, ok);
    return ok;
}

int spell_suggest(const char *w, int n, char out[][64], int max) {
    if (!load() || n <= 0 || n >= 100) return 0;
    char buf[128];
    memcpy(buf, w, n);
    buf[n] = 0;
    char **list = NULL;
    int k = H.suggest(H.h, &list, buf), m = 0;
    for (int i = 0; i < k && m < max; i++) snprintf(out[m++], 64, "%s", list[i]);
    if (list) H.free_list(H.h, &list, k);
    return m;
}

void spell_ignore(const char *w, int n) { cache_put(w, n, 1); }

void spell_add(const char *w, int n) {
    cache_put(w, n, 1);
    char buf[128];
    if (n <= 0 || n >= (int)sizeof buf) return;
    memcpy(buf, w, n);
    buf[n] = 0;
    if (H.h && H.add) H.add(H.h, buf);
    char wp[600];
    words_path(wp, sizeof wp);
    char dir[600];
    snprintf(dir, sizeof dir, "%s", wp);
    char *s = strrchr(dir, '/');
    if (s) { *s = 0; mkdir(dir, 0755); }
    FILE *f = fopen(wp, "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}
