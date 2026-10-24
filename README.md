# Jimi

Jimi is a minimalistic, standalone, stacking Wayland compositor.

It focuses on simplicity. It does not depend on a compositor library, and it
supports only the DRM backend and a Vulkan 1.3 renderer.

Jimi is experimental. Breaking changes, for example to the configuration,
the command line options and supported protocols, may be made at any time
without keeping compatibility.

## Features

- Server-side decorations with a configurable titlebar, buttons and shadows
- Window snapping to the edges and corners of outputs
- Workspaces, and sticky windows shown on all of them
- A window switcher with thumbnails
- Xwayland, started on demand
- HDR, adaptive sync and fractional scaling
- A night light and a magnifier
- Layer shell, session lock, screen capture, input methods, tablets and more

## Building

Jimi needs a GPU with a Vulkan 1.3 driver. The dependencies are listed in
`meson.build`.

```sh
meson setup build
meson compile -C build
```

On Arch Linux, `archlinux/PKGBUILD` builds the commit checked out:

```sh
cd archlinux && makepkg -si
```

## Running

Run `jimi` from a TTY, or choose the Jimi session in a display manager. It
needs a seat from seatd or systemd-logind.

```
jimi [-C config] [-c command] | -r
```

- `-C config` loads the config file instead of
  `$XDG_CONFIG_HOME/jimi/config.yaml`.
- `-c command` runs the command after startup.
- `-r` tells the running jimi to reload its config.

## Configuration

`config.yaml` is an example configuration, which describes every option. Its
values are the defaults, and the options without defaults are commented out.
Copy it to `~/.config/jimi/config.yaml` and edit it. Without the file, the
defaults are used.

## Credits

The code of these compositors was read throughout the development of Jimi to
decide its design, and many parts of Jimi follow how they solved the same
problems:

- [KWin](https://invent.kde.org/plasma/kwin)
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)
- [Sway](https://github.com/swaywm/sway)
- [labwc](https://github.com/labwc/labwc)
- [Wayfire](https://github.com/WayfireWM/wayfire)

Jimi is mostly coded by [Claude Code](https://www.claude.com/product/claude-code)
and [Codex](https://openai.com/codex/).

## Contributing

Feature requests are welcome in
[GitHub Discussions](https://github.com/tokyo4j/jimi/discussions), and bug
reports in [GitHub Issues](https://github.com/tokyo4j/jimi/issues). However,
pull requests, especially large ones, are unlikely to be merged, so please open
a discussion or an issue first. Also note that I'm busy, and may not be able to
reply to them actively.

## License

Jimi is licensed under the [MIT license](LICENSE).
