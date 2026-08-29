# nekoland-settings

Settings app for Nekoland, styled after macOS System Settings, written in C
with GTK4 + libadwaita. Colors come from the system GTK theme's named palette
(catppuccin via libadwaita-without-adwaita), so it follows theme/flavor
changes automatically; only the layout is hard-coded macOS. Currently a blank
skeleton — no settings pages yet.

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

## Adding a category later

`add_category(id, title, icon_name, color_class, page)` in main.c wires a
sidebar row (colored rounded-square icon + label) to a page in the content
stack. Selection switching is already handled; the function is unused until
the first real settings page exists.
