# Noctedit

A tiny, Noctalia-styled text editor for Wayland, written in C.

It speaks Wayland directly and draws into shared-memory buffers with FreeType. There is no GTK, Qt, SDL or GPU stack.

| | noctedit |
|---|---|
| Private memory | ~2.3 MB idle; ~7 MB while spellcheck and document fonts are active (trimmed again after 15 s idle) |
| Idle CPU | 0 (event-driven, the caret stops blinking after 10 s) |
| Full redraw at 2560×1440 | ~1–2 ms |
| Binary | ~215 KB stripped |
| Runtime deps | wayland-client, wayland-cursor, xkbcommon, freetype2, fontconfig, zlib |
| Optional | hunspell + a dictionary (spellcheck), poppler (opening PDFs) |

## Build & install

```sh
make
sudo make install            # /usr/local/bin/noctedit + .desktop entry
make install-noctalia-template   # optional: live theme sync, see below
```

## Text rendering

Glyphs are positioned at their true fractional widths, with quarter-pixel subpixel placement, instead of rounding every character to a whole pixel. This keeps proportional text evenly spaced and monospace columns exact.

## Animations & niceties

- Smooth scrolling (mouse wheel, Page Up/Down, jumps) and a gliding caret.
- Popups, the find bar and toasts slide in. Toasts fade out, the active tab pill slides between tabs, and hovers fade.
- Animations are driven by the compositor's frame callbacks and only run while something moves. Idle CPU stays at zero. Turn them off with `animations = false`, or change their speed with `animation_speed`.
- **Session restore:** files open at quit come back with their cursor positions when you start `noctedit` with no arguments.
- **Recent files:** recently opened files appear at the top of the command palette.
- **Auto-closing brackets and quotes:** typing over a closer steps past it, Backspace removes an empty pair, and typing a bracket with text selected wraps the selection. Quotes aren't paired in Document view, so apostrophes stay simple.
- **Indent guides** in Code view.
- **Idle memory trim:** after 15 s without input, the spellcheck dictionary, fontconfig state, spare window buffers and unused fonts are released. They reload on demand: Hunspell takes about 15 ms.

## Document view (Writer-style)

Prose files open in a word-processor view, while code stays in the code view. Markdown files, prose-like `.txt` files and new files get the document view. Switch with **Ctrl+Shift+M** or the *Document/Code* pill in the status bar.

- **Formatting toolbar:** a paragraph-style dropdown (Body, Heading 1–3, Quote, lists), **B** *I* <u>U</u> ~~S~~, left/center/right/justify, line spacing (Single, 1.15, 1.5, Double), paragraph spacing, bullets, numbering, and a first-line indent.
- **Character and paragraph styles** are stored as plain Markdown (`**bold**`, `*italic*`, `<u>underline</u>`, `~~strike~~`, `# Heading`, `- item`, `1. item`, `> quote`), so files stay readable anywhere. The symbols are hidden, except in the paragraph you're editing, where they're shown dimmed.
- **Double-spaced text is detected:** a hard-wrapped `.txt`/`.md` file with a blank line after every line opens with Double line spacing. It's saved back with its blank lines, so the file's layout on disk is unchanged. *Toggle Double-Spaced File Format* converts a file either way.
- Alignment, spacing, indent and view are remembered per file in `~/.local/state/noctedit/docprops`, never written into the file itself.
- Blank lines between Markdown paragraphs are drawn compactly, and ```` ``` ```` fence lines are hidden unless you're editing them.
- Each line is one paragraph, like pressing Enter in Writer. Enter continues a list; Enter on an empty item ends it.
- *Export as PDF* uses the document layout. It produces A4 pages with 1" margins, and embeds subsets of your document font (regular, bold, italic, bold italic) plus your monospace font.
- Word count is shown in the status bar.
- Markdown tables render as real tables, with column widths sized to their contents, wrapped cells and a shaded header row. The row you're editing shows its raw `| … |` text. Images are shown as plain text, and tables export to PDF as plain text.

## Spellcheck & PDF

```sh
sudo pacman -S hunspell hunspell-en_us poppler
```

- **Spellcheck:** wavy underlines for prose in plain text and Markdown, and only in comments for code. Words that look like code are skipped: identifiers, paths, URLs, camelCase and acronyms. Right-click a word, or press **F7** for the next mistake, to get suggestions, *Add to dictionary* (saved to `~/.config/noctedit/words`) or *Ignore*. Hunspell is loaded with `dlopen` the first time it's needed, so it costs nothing while it's unused. The language comes from `$LANG`, or set `spell_language`.
- **Export as PDF** (`Ctrl+Shift+E`): A4 pages with syntax colors, line numbers, wrapping and a header. Your monospace font is embedded as a subset (about 20 KB for a small file), and the text stays selectable and searchable. It's written by a built-in PDF writer with no extra dependency. Glyphs your monospace font lacks (e.g. CJK) print as blank boxes.
- **Open PDFs:** they open as read-only text tabs (via `pdftotext`) that you can search, copy from, or *Save As* `.txt`.

## Theming

noctedit uses the first palette it finds:

1. `$XDG_STATE_HOME/noctedit/theme.json`. Noctalia's template engine writes this file after `make install-noctalia-template`, and it updates instantly when the wallpaper or scheme changes.
2. `~/.config/gtk-4.0/noctalia.css`. Noctalia's GTK4 export, which works with no setup.
3. A built-in copy of the default Noctalia palette.

Both files are watched with inotify, so recoloring happens live. The icons come from Noctalia's bundled Tabler font.

## Features

- Tabs with modified dots, middle-click to close, drag empty bar space to move the window
- Open/Save/Save As through a built-in, keyboard-driven file browser (Tab completes, `~` and paths work)
- Unsaved-changes prompts on close/quit, overwrite confirmation, atomic saves that keep permissions
- Undo/redo with grouped typing
- Clipboard, primary selection (middle-click paste), drag & drop files
- Find and replace: incremental, match count, case/whole-word toggles, replace all in a single undo step
- Go to line[:col], command palette (`Ctrl+Shift+P` / `F1`) listing every command and shortcut
- Syntax highlighting: C, C++, Python, JS/TS, QML, Rust, Go, Java/Kotlin, Zig, Shell, Lua, JSON, TOML/INI, YAML, CSS, HTML/XML, Markdown, Makefile, CMake, Diff, Nix, Haskell
- Line numbers, current-line highlight, bracket matching, selection-occurrence highlight
- Word wrap, whitespace markers, zoom
- Auto-indent with brace expansion, smart Home, soft-tab backspace, indent detection
- Line operations: toggle comment, indent/outdent, duplicate, move, delete, join, sort, trim, upper/lower case
- LF/CRLF preserved, UTF-8 with font fallback (CJK and symbols), dead keys and compose
- Fractional scaling, server-side decorations when available (built-in window buttons otherwise), external-change detection

## Keys

| | |
|---|---|
| Ctrl+N / O / S / Shift+S / W / Q | new, open, save, save as, close tab, quit |
| Ctrl+Alt+S | save all |
| Ctrl+Tab, Ctrl+PgUp/PgDn, Alt+1…9 | switch tabs |
| Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y | undo / redo |
| Ctrl+F / Ctrl+H / F3 / Shift+F3 | find / replace / next / previous |
| Alt+C, Alt+W (in find) | case-sensitive, whole word |
| Ctrl+Enter (in replace field) | replace all |
| Ctrl+G | go to line |
| Ctrl+/ | toggle comment |
| Tab / Shift+Tab, Ctrl+] / Ctrl+[ | indent / outdent |
| Ctrl+D, Ctrl+Shift+K | duplicate, delete line |
| Alt+Up/Down | move lines |
| Ctrl+L, Ctrl+J | select line, join lines |
| Ctrl+U / Ctrl+Shift+U | upper / lower case |
| Alt+Z | word wrap |
| F7 | next spelling mistake |
| Ctrl+Shift+M | document / code view |
| Ctrl+B / I / U | bold / italic / underline (document view) |
| Ctrl+0 / 1 / 2 / 3 | body text / heading 1–3 (document view) |
| Ctrl+L / E / R / J | align left / center / right / justify (document view) |
| Ctrl+5 | 1.5 line spacing |
| F12 / Shift+F12 | numbered / bulleted list |
| Ctrl+Shift+E | export as PDF |
| Ctrl+= / Ctrl+- / Ctrl+0, Ctrl+wheel | zoom |
| Ctrl+Shift+P, F1, right-click | command palette |

Run `noctedit +42 file.c` or `noctedit file.c:42` to open a file at line 42.

## Config

Copy `data/config.example` to `~/.config/noctedit/config`.

## Tests

`make build/headless` builds the editor against a fake platform. It runs scripted keystrokes and writes PNG snapshots, so you can test without a compositor:

```sh
./build/headless file.c --script key:ctrl+f type:main snap:/tmp/find.png
```
