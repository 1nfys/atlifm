# atlifm

terminal style file manager with human friendly features

![atlifm](atlifm-logo/screenshot.png)

why: default file managers on Linux are either GNOME Nautilus with user-friendly macOS-like design with huge paddings and rounded corners, or terminal ones like yazi or ranger, where the philosophy is that keyboard is strictly more important than mouse

atlifm takes terminal aesthetics (multi-pane layout, pseudographics, monospace font, zero animations) while adding mouse controls, drag and drop, tabs and previews

---

## features

- **multi-pane layout:** parent folder on the left, current folder in the center, preview on the right, collapses automatically in narrow windows
- **mouse interaction:** items open strictly on **double click** (no accidental opening on selection), single click only focuses
- **drag and drop:** drag files into Telegram, Discord, browser, terminal or a graphic editor with a dark badge, Alt+drag opens copy / move / symlink menu
- **top tabs like in neovim/kitty:** shrink dynamically, scroll with mouse wheel, closed with middle click, independent navigation history per tab
- **lag-free preview:** images and SVG with scaling, syntax highlighting for code, directory tree and hex view for binaries
- **interactive archives:** browse zip, tar.gz, 7z, rar directly in preview pane, open files or drag them out without full extraction
- **archive extraction and creation:** extract here, extract in and compress in the context menu
- **properties and disk usage:** Alt+Enter or right click opens inspector with permissions `[ ]`/`[x]`, drive root adds disk usage with progress bars
- **xdg desktop portal:** atlifm can act as the system file picker for browsers and other apps
- **full undo / redo:** Ctrl+Z and Ctrl+Y, pressing Ctrl+Z during a running copy/move cancels it with rollback
- **plugins:** custom actions, shortcuts, overrides of built-in operations, preview plugins, hooks, shell scripts and C++ plugins
- **customizable window & font:** window size, sidebar width, font family and size via `theme.conf`
- **strict visual style:** no rounded corners, monospace `JetBrainsMono Nerd Font`, selection as row highlight plus status bar counter

---

## shortcuts and hotkeys

| key | action |
| :--- | :--- |
| `↑` / `↓` or `k` / `j` | move cursor |
| `Enter` / `Double Click` / `→` | open file or enter folder |
| `Backspace` / `←` | go to parent folder |
| `Space` | select file and move to next |
| `/` or `Ctrl+F` | quick filter by name (transient: Enter or Esc resets) |
| `Ctrl+T` / `Ctrl+W` | new tab / close active tab |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | switch between tabs |
| `Mouse wheel on tab bar` | scroll tabs |
| `Alt+←` / `Alt+→` | back / forward in navigation history |
| `Ctrl+B` / `F9` | toggle bookmarks sidebar on the left |
| `F2` | inline rename |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | copy / cut / paste |
| `Ctrl+Z` / `Ctrl+Y` | undo / redo file operations |
| `Delete` / `Shift+Delete` | move to trash / delete permanently |
| `Ctrl+Shift+N` | create new folder |
| `Ctrl+Alt+N` | create new empty file |
| `Alt+Enter` | file properties and permissions |
| `Ctrl+Alt+X` | extract archive here (Extract Here) |
| `Ctrl+Q` | quit manager |

any key can be rebound in `~/.config/atlifm/keybinds.conf` (created automatically on first launch with all actions and defaults):

```ini
new_tab = Ctrl+T
close_tab = Ctrl+W, Ctrl+F4
quick_jump =          # empty value = action disabled
```

full list of action IDs is available in the cheat sheet on F1. if config does not exist, defaults from the table above are used

cheat sheet (F1) opens as an overlay, and bindings can be remapped directly inside it: click key -> press new combination (Backspace to disable, Esc to cancel), right click on key allows resetting to default (`Reset to default`) or disabling. changes are immediately applied and saved to `keybinds.conf`

---

## building

compiler with C++23 support is required

### dependencies

Arch Linux / Manjaro:
```bash
sudo pacman -S base-devel gcc cmake ninja qt6-base qt6-svg
```

Fedora / RHEL:
```bash
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel
```

Ubuntu / Debian:
```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-base-private-dev libqt6svg6-dev
```

### compilation

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

---

## installation

```bash
cmake --install build --prefix ~/.local
update-desktop-database ~/.local/share/applications
atlifm --setup-portal
```

---

## xdg desktop portal

to use atlifm as the default file chooser portal for browsers and desktop apps:

```bash
atlifm --setup-portal
```

or via the script:

```bash
./scripts/setup-portal.sh
```

---

## theme customization

theme config is located at `~/.config/atlifm/theme.conf`: flat `key = #rrggbb` (or `#aarrggbb` with alpha). if color is not specified, default is used.

**window & font:** `window_width` / `window_height` (default `1200` / `750`), `sidebar_width` (default `180`), `font_family` (default auto: JetBrainsMono Nerd Font → JetBrainsMono NF → DejaVu Sans Mono), `font_size` in px (default `13`, scales UI text and direct-drawn rows/tabs).

**palette:** `base`, `mantle`, `crust`, `surface0`, `surface1`, `surface2`, `overlay0`, `text`, `subtext`, `dim`, `accent`, `accent_fg`, `blue`, `sapphire`, `yellow`, `green`, `red`, `mauve`, `peach`, `teal`, `dark = true/false`.

**ui components:** `border`, `selection_bg` / `selection_fg`, `hover_bg`, `drop_bg` / `drop_border`, `tab_*`, `menu_*`, `input_*`, `button_*`, `checkbox_*`, `scrollbar_*`, `rubberband_*`, `overlay_bg` / `overlay_border`, `badge_*` (full table with defaults at https://1nfys.github.io/atlifml/docs/configuration-reference).

example:

```ini
base = #101418
accent = #2aa198
accent_fg = #101418
surface0 = #1c2228
text = #e0e6ec

selection_bg = #5f5faf
border = #2a3238
tab_active_fg = #e0e6ec
overlay_border = #2aa198
```

---

## plugins

context menu contains Plugins submenu with external commands from `~/.config/atlifm/plugins/*.conf` (one file = one plugin, id = file name without `.conf`, on first launch the directory and example.conf are created automatically):

```ini
Name = Open in VS Code        # required: menu label
Exec = code                   # required: command
Extensions = .cpp, .hpp       # matching extensions only (empty = any)
AppliesTo = files             # files | dirs | any (default any)
Detach = false                # true: launch detached without capturing output (gui apps)
Description = Open in VS Code
```

on launch plugin receives current directory in `ATLIFM_DIR`, selected paths in `ATLIFM_SELECTION` (one per line) and arguments. additionally: `ATLIFM_FILE` (first selected path, empty if none), `ATLIFM_TARGET` (first selected path, fallback: current directory), `ATLIFM_PID` and `ATLIFM_ACTION` (action id when the plugin runs as `Override`). plugin output is streamed into overlay in bottom-right corner (Esc closes it).

custom actions and overriding built-in operations (all keys optional):

```ini
Action = open_in_vscode           # custom action: bound in keybinds.conf as standard id
Keys = Ctrl+Alt+V                 # default shortcut (can be rebound or disabled in keybinds.conf)
Override = rename, open_terminal  # override: plugin command runs instead of built-in operation
```

`Action` appears in cheat sheet (F1) in PLUGINS section. `Override` intercepts any built-in operation, keyboard actions, context menu items, mouse paths, tab bar menu and bookmark panel actions. flows with sudo, polkit or desktop portal are never routed through plugins and override IDs containing `sudo` are rejected at parse time.

preview plugins and hooks (all keys optional):

```ini
Type = preview                  # preview plugin: stdout replaces the preview pane for matching files
Hook = on_start, on_navigate    # run detached on events, output ignored
```

`Type = preview` runs the plugin for matching files (`Extensions` / `AppliesTo` honored) and its stdout replaces the built-in preview, falling back if it fails or prints nothing. `Hook` events: `on_start` (launched), `on_navigate` (directory changed), `on_select` (selection changed).

scripts: executable `*.sh`, `*.py`, `*.bash` files in `~/.config/atlifm/plugins/` are picked up as plugins automatically (id = file name without extension) unless a same-named `.conf` exists.

C++ plugins: a `*.so` in `~/.config/atlifm/plugins/` exporting `atlifm_plugin_init` is loaded in-process (ABI v1, SDK header `atlifm-core/atlifm-plugin-api.h`):

```c
#include "atlifm-plugin-api.h"

static int hello(const AtlifmInvocation* inv, void* user) {
    AtlifmHostApi* api = user;
    api->output_write("hello from plugin\n");
    return 0;
}

extern "C" int atlifm_plugin_init(const AtlifmHostApi* api) {
    if (api->abi_version != ATLIFM_PLUGIN_ABI_VERSION) return 1;
    AtlifmActionDesc d = {0};
    d.struct_size = sizeof d;
    d.id = "hello_native";
    d.name = "Hello Native";
    d.keys = "Ctrl+Alt+H";
    d.run = hello;
    d.user = (void*)api;
    return api->register_action(&d);
}
```

build and install:

```sh
c++ -shared -fPIC -Iatlifm-core -O2 -o hello.so hello-plugin.cpp
cp hello.so ~/.config/atlifm/plugins/
```

the host API exposes action/override/preview/hook registration, status bar messages, output overlay streaming (`run_command` with `detach = 0`), `refresh()` and logging. plugins go through the same override dispatcher as `.conf` plugins (a same-named `.conf` wins) and never see sudo, polkit or portal flows. full example: `examples/hello-plugin/hello-plugin.cpp`.

docs online: https://1nfys.github.io/atlifml/docs/welcome (user guide, configuration reference, plugin developer guide)
