# nekobar

Topbar for Hyprland written in C (GTK3 + gtk-layer-shell), styled to match the
old waybar setup (Catppuccin Mocha, JetBrainsMono Nerd Font).

## Build & run

```sh
make
./nekobar
```

To use it as the session bar, replace `exec-once = waybar` in
`~/.config/hypr/hyprland.conf` with:

```
exec-once = ~/SoftwareDevelopment/Nekoland/shell/nekobar
```

## Layout

- `src/main.c` — GTK setup, one layer-shell bar per monitor, widgets and click handlers
- `src/hypr.c` — Hyprland IPC: requests over `.socket.sock`, live events over
  `.socket2.sock` (workspaces + focused window title), title rewrite rules
- `src/modules.c` — clock, memory, volume (wpctl), mpris (playerctl) timers
- `src/quickset.c` — quick settings panel (its own layer-shell window; GTK
  popovers clip on layer surfaces). Volume slider + mute via wpctl, output
  picker via `pactl --format=json list sinks` / `set-default-sink`; sinks with
  unplugged ports are grayed out (WirePlumber refuses to switch to them).
  Opens from left-click on the volume module, or SIGUSR1
  (`pkill -USR1 nekobar`) for a Hyprland keybind
- `src/tray.c` — StatusNotifierItem system tray over GDBus. Owns
  org.kde.StatusNotifierWatcher when possible (queues for the name if another
  bar holds it and inherits it when that bar exits). Icons resolve via
  IconName + IconThemePath with IconPixmap (ARGB32) fallback; right-click
  menus via libdbusmenu-gtk3, left-click = Activate
- `style.css` — ported 1:1 from `~/.config/waybar/style.css`; loaded from the
  directory next to the binary at startup

## Modules

Left: rofi launcher, per-monitor workspace dots (click to switch), mpris pill
(click play/pause, middle prev, right next), focused window title.
Center: clock.
Right: memory, system tray (left-click activate, right-click menu), volume
(left-click quick settings, right-click pavucontrol, scroll to adjust), color
picker, screenshot (left full / right area), wallpaper (waypaper), power menu.

Not implemented yet: tooltips, clock calendar popup. Battery/backlight omitted
(desktop machine).

The `qml-prototype/` folder holds the earlier Quickshell version; safe to
delete.
