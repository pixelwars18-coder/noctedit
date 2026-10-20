/* Noctalia palette discovery + config file. */
#include "noctedit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *slurp(const char *path, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = xmalloc(max + 1);
    size_t n = fread(b, 1, max, f);
    fclose(f);
    b[n] = 0;
    return b;
}

static bool parse_hex(const char *p, Color *out) {
    while (*p == ' ' || *p == '"' || *p == ':' || *p == '\t') p++;
    if (*p != '#') return false;
    p++;
    unsigned v = 0;
    int n = 0;
    while (isxdigit((unsigned char)p[n]) && n < 8) {
        v = v * 16 + (isdigit((unsigned char)p[n]) ? p[n] - '0' : (tolower((unsigned char)p[n]) - 'a' + 10));
        n++;
    }
    if (n == 3) v = ((v & 0xf00) * 0x1100) | ((v & 0xf0) * 0x110) | ((v & 0xf) * 0x11);
    else if (n == 8) v >>= 8; /* #rrggbbaa: drop alpha */
    else if (n != 6) return false;
    *out = 0xff000000 | v;
    return true;
}

/* find `"key"` in a JSON-ish blob and parse the following color */
static bool json_color(const char *buf, const char *key, Color *out) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    return p && parse_hex(p + strlen(k), out);
}

static bool css_color(const char *buf, const char *name, Color *out) {
    char k[96];
    snprintf(k, sizeof k, "@define-color %s ", name);
    const char *p = strstr(buf, k);
    return p && parse_hex(p + strlen(k), out);
}

static const char *home(void) { const char *h = getenv("HOME"); return h ? h : "/tmp"; }

static void xdg_path(char *out, size_t n, const char *env, const char *fallback, const char *rel) {
    const char *e = getenv(env);
    if (e && *e) snprintf(out, n, "%s/%s", e, rel);
    else snprintf(out, n, "%s/%s/%s", home(), fallback, rel);
}

static float luminance(Color c) {
    return (0.2126f * ((c >> 16) & 255) + 0.7152f * ((c >> 8) & 255) + 0.0722f * (c & 255)) / 255.f;
}

static char state_dir[512], gtk_dir[512];

const char *palette_watch_dir(int i, const char **file) {
    if (!state_dir[0]) {
        xdg_path(state_dir, sizeof state_dir, "XDG_STATE_HOME", ".local/state", "noctedit");
        xdg_path(gtk_dir, sizeof gtk_dir, "XDG_CONFIG_HOME", ".config", "gtk-4.0");
    }
    if (i == 0) { *file = "theme.json"; return state_dir; }
    if (i == 1) { *file = "noctalia.css"; return gtk_dir; }
    return NULL;
}

void palette_load(Palette *p) {
    const char *f;
    palette_watch_dir(0, &f);
    char path[600];

    /* Built-in fallback: Noctalia's default scheme. */
    *p = (Palette){
        .primary = 0xfffff59b, .on_primary = 0xff0e0e43, .secondary = 0xffa9aefe, .tertiary = 0xff9bfece,
        .error = 0xfffd4663, .surface = 0xff070722, .surface_container = 0xff11112d,
        .surface_variant = 0xff191940, .on_surface = 0xfff3edf7, .on_surface_variant = 0xff7c80b4,
        .outline = 0xff4a4e86,
    };
    snprintf(p->source, sizeof p->source, "built-in");

    /* 1) our own Noctalia template output */
    snprintf(path, sizeof path, "%s/theme.json", state_dir);
    char *b = slurp(path, 16384);
    if (b && json_color(b, "mPrimary", &p->primary) && json_color(b, "mSurface", &p->surface)) {
        json_color(b, "mOnPrimary", &p->on_primary);
        json_color(b, "mSecondary", &p->secondary);
        json_color(b, "mTertiary", &p->tertiary);
        json_color(b, "mError", &p->error);
        if (!json_color(b, "mSurfaceContainer", &p->surface_container))
            p->surface_container = color_mix(p->surface, p->on_surface, 0.05f);
        json_color(b, "mSurfaceVariant", &p->surface_variant);
        json_color(b, "mOnSurface", &p->on_surface);
        json_color(b, "mOnSurfaceVariant", &p->on_surface_variant);
        json_color(b, "mOutline", &p->outline);
        snprintf(p->source, sizeof p->source, "noctalia template");
        free(b);
        goto done;
    }
    free(b);

    /* 2) Noctalia's GTK4 color export (enabled by default in Noctalia) */
    snprintf(path, sizeof path, "%s/noctalia.css", gtk_dir);
    b = slurp(path, 65536);
    if (b && css_color(b, "accent_color", &p->primary) && css_color(b, "window_bg_color", &p->surface)) {
        css_color(b, "accent_fg_color", &p->on_primary);
        css_color(b, "window_fg_color", &p->on_surface);
        css_color(b, "error_bg_color", &p->error);
        if (!css_color(b, "card_bg_color", &p->surface_container))
            p->surface_container = color_mix(p->surface, p->on_surface, 0.05f);
        p->surface_variant = color_mix(p->surface_container, p->on_surface, 0.12f);
        p->on_surface_variant = color_mix(p->on_surface, p->surface, 0.25f);
        p->outline = color_mix(p->on_surface, p->surface, 0.5f);
        p->secondary = color_mix(p->primary, p->on_surface, 0.45f);
        /* tertiary: rotate the accent's channels for a related but distinct hue */
        Color a = p->primary;
        p->tertiary = color_mix(0xff000000 | ((a & 0xff) << 16) | ((a >> 16) & 0xff) << 8 | ((a >> 8) & 0xff), p->on_surface, 0.3f);
        snprintf(p->source, sizeof p->source, "noctalia gtk4");
    }
    free(b);
done:
    p->light = luminance(p->surface) > 0.5f;
}

/* ------------------------------------------------------------------ config */
void config_load(Config *c) {
    *c = (Config){
        .font = "monospace", .ui_font = "sans-serif:weight=medium",
        .font_size = 15, .ui_font_size = 14, .tab_w = 4,
        .use_spaces = true, .wrap = false, .line_numbers = true, .highlight_line = true,
        .blink = true, .autoindent = true, .whitespace = false, .radius = 12,
        .spellcheck = true, .pdf_line_numbers = true, .doc_font = "sans-serif", .doc_font_size = 16, .page_width = 760, .doc_default = 1,
        .animations = true, .smooth_caret = true, .anim_speed = 1.0f, .restore_session = true, .auto_close = true, .indent_guides = true,
    };
    char path[600];
    xdg_path(path, sizeof path, "XDG_CONFIG_HOME", ".config", "noctedit/config");
    char *b = slurp(path, 16384);
    if (!b) return;
    for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
        while (isspace((unsigned char)*line)) line++;
        if (*line == '#' || *line == ';' || *line == '[' || !*line) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        for (char *e = eq - 1; e >= k && isspace((unsigned char)*e); e--) *e = 0;
        while (isspace((unsigned char)*v) || *v == '"') v++;
        for (char *e = v + strlen(v) - 1; e >= v && (isspace((unsigned char)*e) || *e == '"'); e--) *e = 0;
        bool bv = !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "on");
        int iv = atoi(v);
        if (!strcmp(k, "font")) snprintf(c->font, sizeof c->font, "%s", v);
        else if (!strcmp(k, "ui_font")) snprintf(c->ui_font, sizeof c->ui_font, "%s", v);
        else if (!strcmp(k, "font_size") && iv > 4) c->font_size = iv;
        else if (!strcmp(k, "ui_font_size") && iv > 4) c->ui_font_size = iv;
        else if (!strcmp(k, "tab_width") && iv > 0 && iv <= 16) c->tab_w = iv;
        else if (!strcmp(k, "use_spaces")) c->use_spaces = bv;
        else if (!strcmp(k, "word_wrap")) c->wrap = bv;
        else if (!strcmp(k, "line_numbers")) c->line_numbers = bv;
        else if (!strcmp(k, "highlight_line")) c->highlight_line = bv;
        else if (!strcmp(k, "cursor_blink")) c->blink = bv;
        else if (!strcmp(k, "auto_indent")) c->autoindent = bv;
        else if (!strcmp(k, "show_whitespace")) c->whitespace = bv;
        else if (!strcmp(k, "radius") && iv >= 0) c->radius = iv;
        else if (!strcmp(k, "spellcheck")) c->spellcheck = bv;
        else if (!strcmp(k, "spell_language")) snprintf(c->spell_lang, sizeof c->spell_lang, "%s", v);
        else if (!strcmp(k, "pdf_line_numbers")) c->pdf_line_numbers = bv;
        else if (!strcmp(k, "document_font")) snprintf(c->doc_font, sizeof c->doc_font, "%s", v);
        else if (!strcmp(k, "animations")) c->animations = bv;
        else if (!strcmp(k, "smooth_caret")) c->smooth_caret = bv;
        else if (!strcmp(k, "animation_speed") && atof(v) > 0.05) c->anim_speed = (float)atof(v);
        else if (!strcmp(k, "restore_session")) c->restore_session = bv;
        else if (!strcmp(k, "auto_close")) c->auto_close = bv;
        else if (!strcmp(k, "indent_guides")) c->indent_guides = bv;
        else if (!strcmp(k, "document_font_size") && iv > 4) c->doc_font_size = iv;
        else if (!strcmp(k, "page_width") && iv >= 300) c->page_width = iv;
        else if (!strcmp(k, "document_view")) c->doc_default = !strcmp(v, "never") || !strcmp(v, "off") ? 0 : !strcmp(v, "always") ? 2 : 1;
    }
    free(b);
}
