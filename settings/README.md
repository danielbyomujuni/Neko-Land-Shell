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

**RGB** — lighting control for connected devices, built to the mockup's RGB
pane: a master "Lighting" card (on/off, effect, colour dots + hex, brightness,
speed — staged until "Apply to All Devices"), expandable per-device rows
(caret + live colour square + "type · mode" subtitle + on/off switch,
revealing effect/colour/brightness controls), and a PROFILES card —
snapshots of the full state saved to `~/.config/nekoland/rgb-profiles.ini`;
click a card to apply, thumbnails are gradients of the profile's colours.
Brightness is implemented by scaling the emitted colour; speed feeds
providers with a speed control (NZXT). Devices come from pluggable backends declared in
`src/rgb.h` (`RgbProvider`: list / set_color / set_mode, optional
set_color_all) and registered in `providers[]` in `src/rgb.c` — adding a
device family means implementing those hooks and appending one table entry.

Native providers (no external tools, instant): ENE DRAM (SMBus), NZXT hubs +
Kraken fans (hidraw), Gigabyte Fusion2 Blackwell GPU (raw i2c), ASRock
Polychrome USB (hidraw), Razer extended-matrix keyboards (feature reports),
Logitech HID++ 0x8070 mice — every RGB device in this machine is covered
natively; the OpenRGB CLI fallback only activates for hardware none of them
claim. The OpenRGB *package* is still wanted for its udev rules (device
ACLs on hidraw/i2c).
- `src/rgb_ene.c` — ENE (Aura) DRAM over SMBus (/dev/i2c-*): pointer-write
  register protocol, probes 0x71/0x73/0x67 on SMBus adapters only, validates
  via the device-name register, reads back the current mode. Colours are RBG.
- `src/rgb_hue2.c` — NZXT Hue 2 family over hidraw: legacy 64-byte effect
  packets (verified against the RGB & Fan Controller 2024, PID 0x2022); model
  table maps PIDs to channel counts.

Device node access comes from OpenRGB's udev rules (i2c/hidraw ACLs) — keep
that package or ship equivalent rules.

`src/rgb_openrgb.c` is an *optional* CLI fallback for hardware without native
code yet (GPU, mice, keyboards, motherboard); used only if `openrgb` is in
PATH, and it skips devices native providers claimed. Invocations are
serialized through flock + timeout because concurrent openrgb processes
deadlock on the hardware. `NEKOLAND_PAGE=<id>` opens the app on a category.

## Adding a category

`add_category(id, title, glyph, color_class, page)` in main.c wires a sidebar
row (colored icon tile with a nerd-font glyph + label) to a page in the
content stack; see the "sound" category + `src/audio.c` for the pattern
(page module exposes `<name>_page_new()`).
