/* Table-driven, line-incremental syntax highlighter.
 * Each line is tokenized from a one-byte start state (multi-line comments/strings). */
#include "noctedit.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

enum {
    F_PREPROC = 1 << 0,  /* '#' directive lines (C-like) */
    F_TRIPLE = 1 << 1,   /* """ / ''' strings */
    F_BACKTICK = 1 << 2, /* multi-line `template` strings */
    F_DOLLAR = 1 << 3,   /* $variables */
    F_KEYS = 1 << 4,     /* key = / key: highlighted as keys */
    F_MARKDOWN = 1 << 5,
    F_TAGS = 1 << 6,     /* <tag attr="..."> */
    F_SECTIONS = 1 << 7, /* [section] lines */
    F_NOFUNC = 1 << 8,
    F_ATTR = 1 << 9,     /* @decorators / #[attrs] */
};

enum { S_NORMAL, S_BLOCK, S_TRIPLE2, S_TRIPLE1, S_BACKTICK, S_FENCE };

struct Lang {
    const char *name, *match; /* match: space-separated ".ext" or exact file names */
    const char *lc, *lc2, *bc0, *bc1;
    const char *kw, *ty, *cst;
    const char *quotes;
    unsigned flags;
};

#define C_KW "auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static struct switch typedef union volatile while _Alignas _Alignof _Atomic _Generic _Noreturn _Static_assert _Thread_local alignas alignof constexpr nullptr static_assert thread_local typeof"
#define C_TY "char double float int long short signed unsigned void bool _Bool size_t ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t FILE va_list off_t pid_t wchar_t"
#define CPP_KW C_KW " class namespace template typename public private protected virtual override final new delete this throw try catch using operator friend explicit mutable noexcept decltype consteval constinit co_await co_return co_yield concept requires export import module dynamic_cast static_cast reinterpret_cast const_cast"
#define CPP_TY C_TY " auto string vector map set unordered_map unique_ptr shared_ptr optional std QString QObject"
#define JS_KW "break case catch class const continue debugger default delete do else export extends finally for function if import in instanceof let new of return super switch this throw try typeof var void while with yield async await static get set from as"
#define TS_KW JS_KW " interface type enum implements namespace declare abstract readonly private public protected keyof infer is satisfies"
#define JS_TY "Array Object String Number Boolean Promise Map Set Symbol Date RegExp Error JSON Math console window document any unknown never string number boolean void bigint"
#define JS_CST "true false null undefined NaN Infinity"

static const Lang langs[] = {
    {"Plain Text", "", 0},
    {"C", ".c .h", "//", 0, "/*", "*/", C_KW, C_TY, "true false NULL EOF stdin stdout stderr", "\"'", F_PREPROC},
    {"C++", ".cpp .cc .cxx .hpp .hh .hxx .ino", "//", 0, "/*", "*/", CPP_KW, CPP_TY, "true false nullptr NULL", "\"'", F_PREPROC},
    {"Python", ".py .pyw .pyi SConstruct", "#", 0, 0, 0,
     "and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield match case print",
     "int float str bytes bool list dict set tuple object type range len self cls Exception", "True False None", "\"'", F_TRIPLE | F_ATTR},
    {"JavaScript", ".js .mjs .cjs .jsx", "//", 0, "/*", "*/", JS_KW, JS_TY, JS_CST, "\"'`", F_BACKTICK},
    {"TypeScript", ".ts .tsx .mts .cts", "//", 0, "/*", "*/", TS_KW, JS_TY, JS_CST, "\"'`", F_BACKTICK | F_ATTR},
    {"QML", ".qml .qmltypes", "//", 0, "/*", "*/", JS_KW " property signal readonly required alias component pragma Singleton",
     "int real bool string var url color list date point rect size Item Rectangle Text Column Row Repeater Loader QtObject Component Connections", JS_CST, "\"'`", F_BACKTICK},
    {"Rust", ".rs", "//", 0, "/*", "*/",
     "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait type unsafe use where while macro_rules",
     "i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box Rc Arc HashMap", "true false None Some Ok Err", "\"", F_ATTR},
    {"Go", ".go", "//", 0, "/*", "*/",
     "break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var",
     "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr any", "true false nil iota", "\"'`", F_BACKTICK},
    {"Java", ".java .kt .kts .scala", "//", 0, "/*", "*/",
     "abstract assert break case catch class const continue default do else enum extends final finally for goto if implements import instanceof interface native new package private protected public return static strictfp super switch synchronized this throw throws transient try volatile while fun val var when object companion data sealed override open internal",
     "boolean byte char double float int long short void String Object Integer List Map", "true false null", "\"'", F_ATTR},
    {"Zig", ".zig", "//", 0, 0, 0,
     "const var fn pub return if else while for switch break continue defer errdefer try catch orelse struct enum union error comptime inline export extern test and or unreachable",
     "u8 u16 u32 u64 usize i8 i16 i32 i64 isize f32 f64 bool void type anyerror anytype", "true false null undefined", "\"'", F_ATTR},
    {"Shell", ".sh .bash .zsh .fish .ksh PKGBUILD .bashrc .zshrc .profile .bash_profile .install APKBUILD", "#", 0, 0, 0,
     "if then else elif fi case esac for while until do done in function select return local export readonly declare typeset unset shift source alias exit break continue",
     "echo printf cd test read eval exec set trap wait", "true false", "\"'`", F_DOLLAR | F_NOFUNC},
    {"Lua", ".lua", "--", 0, "--[[", "]]",
     "and break do else elseif end for function goto if in local not or repeat return then until while", "self", "true false nil", "\"'", 0},
    {"JSON", ".json .jsonc .json5 .geojson .webmanifest", "//", 0, "/*", "*/", "", "", "true false null", "\"", F_KEYS | F_NOFUNC},
    {"TOML", ".toml .ini .conf .cfg .desktop .service .timer .socket .mount .path .target .lock config", "#", ";", 0, 0, "", "", "true false", "\"'", F_KEYS | F_SECTIONS | F_NOFUNC},
    {"YAML", ".yaml .yml", "#", 0, 0, 0, "", "", "true false null yes no on off ~", "\"'", F_KEYS | F_NOFUNC},
    {"CSS", ".css .scss .sass .less .rasi", "//", 0, "/*", "*/", "important media import keyframes font-face supports root hover active focus", "", "", "\"'", F_KEYS},
    {"HTML", ".html .htm .xml .svg .xhtml .ui .glade .plist .xsl .vue", 0, 0, "<!--", "-->", "", "", "", "\"'", F_TAGS | F_NOFUNC},
    {"Markdown", ".md .markdown .mdown .mkd README", 0, 0, 0, 0, "", "", "", "", F_MARKDOWN},
    {"Makefile", "Makefile makefile GNUmakefile .mk .mak", "#", 0, 0, 0,
     "ifeq ifneq ifdef ifndef else endif include define endef export override", "", "", "\"'", F_DOLLAR | F_NOFUNC},
    {"CMake", "CMakeLists.txt .cmake", "#", 0, 0, 0,
     "if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro return set unset option project add_executable add_library target_link_libraries target_include_directories find_package include install message",
     "", "ON OFF TRUE FALSE", "\"", F_DOLLAR},
    {"Diff", ".diff .patch", 0, 0, 0, 0, "", "", "", "", F_NOFUNC},
    {"Nix", ".nix", "#", 0, "/*", "*/", "let in with rec inherit if then else assert import", "", "true false null", "\"", F_KEYS},
    {"Haskell", ".hs", "--", 0, "{-", "-}", "case class data default deriving do else if import in infix instance let module newtype of then type where", "Int Integer Bool Char String IO Maybe Either Double Float", "True False Nothing Just", "\"'", 0},
};
#define NLANGS ((int)(sizeof langs / sizeof *langs))

const Lang *lang_at(int i) { return i >= 0 && i < NLANGS ? &langs[i] : NULL; }
const char *lang_name(const Lang *l) { return l ? l->name : "Plain Text"; }
const char *lang_comment(const Lang *l) { return l && l->lc ? l->lc : NULL; }

static bool word_in(const char *list, const char *w, int n) {
    if (!list) return false;
    const char *p = list;
    while (*p) {
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e - p == n && !memcmp(p, w, n)) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

const Lang *lang_detect(const char *path, const char *first, int fn) {
    if (path) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        const char *ext = strrchr(base, '.');
        for (int i = 1; i < NLANGS; i++) {
            if (word_in(langs[i].match, base, (int)strlen(base))) return &langs[i];
            if (ext && ext != base && word_in(langs[i].match, ext, (int)strlen(ext))) return &langs[i];
        }
        /* case-insensitive extension pass */
        if (ext) {
            char low[16];
            int n = (int)strlen(ext);
            if (n < (int)sizeof low) {
                for (int k = 0; k < n; k++) low[k] = tolower((unsigned char)ext[k]);
                for (int i = 1; i < NLANGS; i++) if (word_in(langs[i].match, low, n)) return &langs[i];
            }
        }
    }
    if (first && fn > 2 && first[0] == '#' && first[1] == '!') {
        struct { const char *k; int lang; } sb[] = {{"python", 3}, {"node", 4}, {"bash", 11}, {"sh", 11}, {"zsh", 11}, {"lua", 12}};
        for (size_t k = 0; k < sizeof sb / sizeof *sb; k++) {
            int kl = (int)strlen(sb[k].k);
            for (int i = 2; i + kl <= fn; i++)
                if (!memcmp(first + i, sb[k].k, kl)) return &langs[sb[k].lang];
        }
    }
    return &langs[0];
}

static inline bool is_id0(unsigned char c) { return isalpha(c) || c == '_' || c >= 0x80; }
static inline bool is_id(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }
static inline bool at(const char *s, int n, int i, const char *t) {
    int l = (int)strlen(t);
    return i + l <= n && !memcmp(s + i, t, l);
}
static inline void mark(uint8_t *o, int a, int b, uint8_t t) { if (o && b > a) memset(o + a, t, b - a); }

static int find(const char *s, int n, int from, const char *t) {
    int l = (int)strlen(t);
    for (int i = from; i + l <= n; i++) if (!memcmp(s + i, t, l)) return i;
    return -1;
}

/* scan a quoted string starting after the opening quote; returns index after close, or n */
static int scan_str(const char *s, int n, int i, char q, bool *closed) {
    while (i < n) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == q) { *closed = true; return i + 1; }
        i++;
    }
    *closed = false;
    return n;
}

static uint8_t hl_markdown(const char *s, int n, uint8_t st, uint8_t *o) {
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (at(s, n, i, "```") || at(s, n, i, "~~~")) { mark(o, 0, n, T_STR); return st == S_FENCE ? S_NORMAL : S_FENCE; }
    if (st == S_FENCE) { mark(o, 0, n, T_STR); return st; }
    mark(o, 0, n, T_TEXT);
    if (i < n && s[i] == '#') { mark(o, 0, n, T_HEAD); return st; }
    if (i < n && s[i] == '>') { mark(o, 0, n, T_COMMENT); return st; }
    if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') mark(o, i, i + 1, T_KW);
    else {
        int j = i;
        while (j < n && isdigit((unsigned char)s[j])) j++;
        if (j > i && j + 1 < n && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ') mark(o, i, j + 1, T_KW);
    }
    if (!o) return st;
    for (int k = i; k < n; k++) {
        if (s[k] == '`') {
            int e = find(s, n, k + 1, "`");
            if (e < 0) break;
            mark(o, k, e + 1, T_STR);
            k = e;
        } else if (s[k] == '*' || s[k] == '_') {
            char c = s[k];
            bool dbl = k + 1 < n && s[k + 1] == c;
            int e = dbl ? find(s, n, k + 2, c == '*' ? "**" : "__") : -1;
            if (dbl && e > k + 2) { mark(o, k, e + 2, T_CONST); k = e + 1; }
        } else if (s[k] == '[') {
            int e = find(s, n, k, "](");
            int e2 = e > 0 ? find(s, n, e, ")") : -1;
            if (e > 0 && e2 > 0) { mark(o, k, e + 1, T_KW); mark(o, e + 1, e2 + 1, T_FUNC); k = e2; }
        }
    }
    return st;
}

uint8_t hl_line(const Lang *L, const char *s, int n, uint8_t st, uint8_t *o) {
    if (!L || L == &langs[0]) { mark(o, 0, n, T_TEXT); return 0; }
    if (L->flags & F_MARKDOWN) return hl_markdown(s, n, st, o);
    if (!strcmp(L->name, "Diff")) {
        uint8_t t = T_TEXT;
        if (n && (s[0] == '+')) t = T_STR;
        else if (n && s[0] == '-') t = T_PRE;
        else if (n && s[0] == '@') t = T_KW;
        else if (at(s, n, 0, "diff ") || at(s, n, 0, "index ")) t = T_HEAD;
        mark(o, 0, n, t);
        return 0;
    }
    int i = 0;
    mark(o, 0, n, T_TEXT);
    if (st == S_BLOCK) {
        int e = find(s, n, 0, L->bc1);
        if (e < 0) { mark(o, 0, n, T_COMMENT); return S_BLOCK; }
        i = e + (int)strlen(L->bc1);
        mark(o, 0, i, T_COMMENT);
    } else if (st == S_TRIPLE2 || st == S_TRIPLE1) {
        int e = find(s, n, 0, st == S_TRIPLE2 ? "\"\"\"" : "'''");
        if (e < 0) { mark(o, 0, n, T_STR); return st; }
        i = e + 3;
        mark(o, 0, i, T_STR);
    } else if (st == S_BACKTICK) {
        bool closed;
        i = scan_str(s, n, 0, '`', &closed);
        mark(o, 0, i, T_STR);
        if (!closed) return S_BACKTICK;
    }
    bool in_tag = false;
    int first = 0;
    while (first < n && (s[first] == ' ' || s[first] == '\t')) first++;

    if ((L->flags & F_PREPROC) && first < n && s[first] == '#' && i == 0) {
        /* directive: highlight up to a trailing comment */
        int e = find(s, n, first, "//");
        int e2 = find(s, n, first, "/*");
        int end = n;
        if (e >= 0) end = e;
        if (e2 >= 0 && e2 < end) end = e2;
        mark(o, first, end, T_PRE);
        /* keep strings in includes readable */
        i = end;
    }
    if ((L->flags & F_SECTIONS) && first < n && s[first] == '[' && i == 0) {
        int e = find(s, n, first, "]");
        if (e > 0) { mark(o, first, e + 1, T_KW); i = e + 1; }
    }

    while (i < n) {
        unsigned char c = s[i];
        if (L->lc && at(s, n, i, L->lc) && !(L->bc0 && at(s, n, i, L->bc0))) {
            /* '#' in shell only starts a comment at a word boundary */
            if (!((L->flags & F_DOLLAR) && i > 0 && !isspace((unsigned char)s[i - 1]) && s[i - 1] != ';')) {
                mark(o, i, n, T_COMMENT);
                return S_NORMAL;
            }
        }
        if (L->lc2 && at(s, n, i, L->lc2) && i == first) { mark(o, i, n, T_COMMENT); return S_NORMAL; }
        if (L->bc0 && at(s, n, i, L->bc0)) {
            int e = find(s, n, i + (int)strlen(L->bc0), L->bc1);
            if (e < 0) { mark(o, i, n, T_COMMENT); return S_BLOCK; }
            e += (int)strlen(L->bc1);
            mark(o, i, e, T_COMMENT);
            i = e;
            continue;
        }
        if ((L->flags & F_TRIPLE) && (at(s, n, i, "\"\"\"") || at(s, n, i, "'''"))) {
            const char *q = c == '"' ? "\"\"\"" : "'''";
            int e = find(s, n, i + 3, q);
            if (e < 0) { mark(o, i, n, T_STR); return c == '"' ? S_TRIPLE2 : S_TRIPLE1; }
            mark(o, i, e + 3, T_STR);
            i = e + 3;
            continue;
        }
        if (L->flags & F_TAGS) {
            if (c == '<') {
                int j = i + 1;
                if (j < n && (s[j] == '/' || s[j] == '?' || s[j] == '!')) j++;
                int k = j;
                while (k < n && (is_id(s[k]) || s[k] == '-' || s[k] == ':' || s[k] == '.')) k++;
                mark(o, i, j, T_PUNCT);
                mark(o, j, k, T_KW);
                in_tag = true;
                i = k;
                continue;
            }
            if (c == '>' && in_tag) { mark(o, i, i + 1, T_PUNCT); in_tag = false; i++; continue; }
            if (!in_tag) {
                if (c == '&') {
                    int e = find(s, n, i, ";");
                    if (e > i && e - i < 10) { mark(o, i, e + 1, T_CONST); i = e + 1; continue; }
                }
                i++;
                continue;
            }
        }
        if (L->quotes && strchr(L->quotes, c) && c) {
            /* apostrophes inside words (Rust lifetimes, prose) are not strings */
            if (c == '\'' && i > 0 && is_id(s[i - 1]) && !(L->flags & F_DOLLAR)) { i++; continue; }
            bool closed;
            int e = (L->flags & F_DOLLAR) && c == '\'' ? (find(s, n, i + 1, "'") < 0 ? n : find(s, n, i + 1, "'") + 1)
                                                       : scan_str(s, n, i + 1, c, &closed);
            if ((L->flags & F_DOLLAR) && c == '\'') closed = e < n || (e == n && n > i + 1 && s[n - 1] == '\'');
            uint8_t t = T_STR;
            if ((L->flags & F_KEYS) || (L->flags & F_TAGS)) {
                int k = e;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < n && s[k] == ':' && !(L->flags & F_TAGS)) t = T_TYPE;
            }
            mark(o, i, e, t);
            if (o && t == T_STR && (L->flags & F_DOLLAR) && c == '"') {
                for (int k = i + 1; k < e; k++)
                    if (s[k] == '$' && k + 1 < e && (is_id0(s[k + 1]) || s[k + 1] == '{')) {
                        int m = k + 1;
                        if (s[m] == '{') { int z = find(s, e, m, "}"); m = z < 0 ? e : z + 1; }
                        else while (m < e && is_id(s[m])) m++;
                        mark(o, k, m, T_TYPE);
                        k = m - 1;
                    }
            }
            if (c == '`' && (L->flags & F_BACKTICK) && !closed) return S_BACKTICK;
            i = e;
            continue;
        }
        if ((L->flags & F_DOLLAR) && c == '$' && i + 1 < n) {
            int j = i + 1;
            if (s[j] == '{' || s[j] == '(') {
                char close = s[j] == '{' ? '}' : ')';
                int depth = 0;
                while (j < n) { if (s[j] == s[i + 1]) depth++; else if (s[j] == close && --depth == 0) { j++; break; } j++; }
            } else if (is_id0(s[j]) || isdigit((unsigned char)s[j])) {
                while (j < n && is_id(s[j])) j++;
            } else if (strchr("@#?$!*-_", s[j])) j++;
            mark(o, i, j, T_TYPE);
            i = j;
            continue;
        }
        if ((L->flags & F_ATTR) && (c == '@' || (c == '#' && i + 1 < n && (s[i + 1] == '[' || s[i + 1] == '!')))) {
            int j = i + 1;
            if (c == '#') { int e = find(s, n, i, "]"); j = e < 0 ? n : e + 1; }
            else while (j < n && (is_id(s[j]) || s[j] == '.')) j++;
            mark(o, i, j, T_PRE);
            i = j;
            continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            if (i > 0 && is_id(s[i - 1])) { i++; continue; }
            int j = i + 1;
            while (j < n && (is_id(s[j]) || s[j] == '.' || ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E') && !(s[i + 1] == 'x' || s[i + 1] == 'X')))) j++;
            mark(o, i, j, T_NUM);
            i = j;
            continue;
        }
        if (is_id0(c) || ((L->flags & F_TAGS) && in_tag)) {
            int j = i;
            while (j < n && (is_id(s[j]) || ((L->flags & (F_KEYS | F_TAGS)) && s[j] == '-'))) j++;
            if (j == i) { i++; continue; }
            if (o) {
                uint8_t t = T_TEXT;
                int len = j - i;
                if (word_in(L->kw, s + i, len)) t = T_KW;
                else if (word_in(L->ty, s + i, len)) t = T_TYPE;
                else if (word_in(L->cst, s + i, len)) t = T_CONST;
                else {
                    int k = j;
                    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                    if ((L->flags & (F_KEYS | F_TAGS)) && k < n && (s[k] == '=' || (s[k] == ':' && !(L->flags & F_TAGS))))
                        t = T_TYPE;
                    else if (!(L->flags & F_NOFUNC) && k < n && s[k] == '(') t = T_FUNC;
                    else if (len > 1) {
                        bool caps = true;
                        for (int m = i; m < j; m++) if (islower((unsigned char)s[m])) { caps = false; break; }
                        if (caps && isupper(c)) t = T_CONST;
                    }
                }
                mark(o, i, j, t);
            }
            i = j;
            continue;
        }
        if (strchr("+-*/%=<>!&|^~?:;,.(){}[]", c)) mark(o, i, i + 1, T_PUNCT);
        i++;
    }
    return S_NORMAL;
}
