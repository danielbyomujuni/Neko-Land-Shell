# nekoland-settings

Settings app for Nekoland, styled after macOS System Settings, written in C
with GTK4 + libadwaita. Colors come from the system GTK theme's named palette
(catppuccin via libadwaita-without-adwaita), so it follows theme/flavor
changes automatically; only the layout is hard-coded macOS.

Pages so far: **Sound** — layout follows the Settings.dc.html mockup: slim
pane header, 560px centered column, macOS-style form rows (right-aligned
label column). Contains a live spectrum visualizer (cava raw-ascii →
GtkDrawingArea), Output volume + Balance sliders (balance preserved via
per-channel `pactl set-sink-volume`), OUTPUT/INPUT device cards with
transport tags (USB/HDMI/…) and accent checkmarks (click to set default,
unplugged ports grayed), a live input level meter (parec on the default
source), and an **Advanced…** modal with per-device visibility switches.
Hidden devices go to `~/.config/nekoland/hidden-audio.conf` (one name per
line), which nekobar's quick settings also honors. `NEKOLAND_ADVANCED=1`
opens the modal on startup (dev hook).

## Build & run

```sh
make
./nekoland-settings
```

## Layout

- `src/main.c` — AdwApplication with the System Settings shell: undecorated
  window (no titlebar) with a single macOS-style red close dot floating over
  the sidebar, 215px sidebar with a search field and a category list, and a
  GtkStack content pane showing an empty state. Move/resize via Hyprland
  (Super+drag) since there is no titlebar to grab.
- `style.css` — the macOS dark look (loaded from the directory next to the
  binary). Includes an icon-tile palette (`.icon-blue`, `.icon-red`, …) for
  future sidebar categories.

## Adding a category

`add_category(id, title, glyph, color_class, page)` in main.c wires a sidebar
row (colored icon tile with a nerd-font glyph + label) to a page in the
content stack; see the "sound" category + `src/audio.c` for the pattern
(page module exposes `<name>_page_new()`).
