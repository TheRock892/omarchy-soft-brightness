# omarchy-soft-brightness

"Virtual" brightness for an external monitor when real DDC/CI control doesn't work.

Nothing new on the hardware side: it draws a semi-transparent black rectangle, full-screen, **only on the chosen monitor**, click-through and keyboard-transparent (the same trick some GNOME extensions used when DDC wasn't available). Works on any Wayland compositor that implements `wlr-layer-shell` (Hyprland, and therefore Omarchy, included).

## Installation

Install the bar plugin (clones this repo into `~/.config/omarchy/plugins/`), then build the daemon/CLI it depends on:

```
omarchy plugin add https://github.com/TheRock892/omarchy-soft-brightness --enable
sh ~/.config/omarchy/plugins/io.github.therock892.soft-brightness/install.sh
```

The second command builds and installs `omarchy-soft-brightnessd` and `omarchy-soft-brightness` into `~/.local/bin`. No root needed, no system service: it's a plain user Wayland client.

Build dependencies: `gcc`, `pkg-config`, `wayland-scanner`, `wayland-client`/`wayland-protocols` development headers.

To use only the command-line tool (without the bar plugin), clone the repo anywhere else and run `sh install.sh` from there.

## Removal

```
omarchy plugin remove io.github.therock892.soft-brightness --yes
rm -f ~/.local/bin/omarchy-soft-brightness ~/.local/bin/omarchy-soft-brightnessd
rm -rf ~/.local/state/omarchy-soft-brightness
```

## Usage

```
omarchy-soft-brightness status        # current level (0-100, 100 = full brightness, 0 = darkest)
omarchy-soft-brightness 50%           # set brightness to 50%
omarchy-soft-brightness +10%          # increase by 10 points
omarchy-soft-brightness 10%-          # decrease by 10 points
omarchy-soft-brightness off           # no dimming (equivalent to 100%)
omarchy-soft-brightness --monitor HDMI-A-1 50%   # force a specific monitor
```

The percentage represents the desired brightness, not how much to darken: 100% = no effect (full brightness). The minimum selectable value is **10%** (the screen never goes fully black, enforced in both the daemon and the script).

## Omarchy bar plugin

Also installed as an Omarchy bar plugin (`~/.config/omarchy/plugins/io.github.therock892.soft-brightness/`): bar icon with a slider popup (10-100%), mouse wheel to adjust by 5 points, auto-refresh while the popup is open. It's just a front end over the `omarchy-soft-brightness` script — it never talks to the daemon's socket directly.

Useful commands if you modify it:
```
omarchy-shell shell rescanPlugins   # reload after editing Panel.qml
omarchy plugin enable io.github.therock892.soft-brightness
omarchy plugin disable io.github.therock892.soft-brightness
```

Without `--monitor`, it auto-detects the first connected external monitor (anything not `eDP`/`LVDS`/`DSI`) via `hyprctl monitors`.

The daemon (`omarchy-soft-brightnessd`) starts itself on the first command and keeps running until the monitor is unplugged or it receives `QUIT`. The chosen level is persisted in `~/.local/state/omarchy-soft-brightness/<monitor>.level` and restored automatically the next time the daemon starts.

## Binding it to the Fn keys

To use the same brightness keys (Fn+F5/F6) for the external monitor too when it's connected, just add a Hyprland bind that calls this script instead of (or in addition to) `omarchy-brightness-display`, pointing explicitly at the external monitor.

## Known limitations

- It's a visual trick (darkens with a black overlay), it doesn't change the real backlight: at high dimming levels contrast and perceived image quality drop, as with any software gamma/overlay technique (the same trade-off the equivalent GNOME extension had).
- If the monitor is unplugged and replugged, the existing daemon exits (the protocol's `closed` event) and the command just needs to be called again: it comes back up on its own.
- One daemon per monitor: concurrent commands on the same monitor are serialized by the control socket, no issue for normal use (keys, scripts).

## License

MIT, see `LICENSE`.
