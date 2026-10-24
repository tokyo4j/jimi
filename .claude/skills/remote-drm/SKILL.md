---
name: remote-drm
description: Build, run, debug and profile jimi, or run another compositor such as labwc or sway for comparison, on the DRM backend of the remote test PC over SSH, with screenshots, injected input, logs, core dumps, KMS debug output, OBS screen capture, CPU usage and perf. Use this to run jimi, to see a change working on real hardware, to reproduce and debug a bug, to check how other compositors behave, or to analyze performance.
---

# Running jimi on the remote PC

jimi needs a GPU and a seat, so it runs on a dedicated PC with one monitor,
whose SSH destination is `$JIMI_REMOTE`, e.g. set in the `env` of
`.claude/settings.local.json`. If it is unset, ask the user. Everything goes
through `.claude/skills/remote-drm/remote.sh`, run from the repository root by
that relative path, which `.claude/settings.json` allows without prompts. On
the remote PC, the files live in `~/jimi-agent/`, and the compositor, jimi or
another one, runs as the transient user unit `compositor.service` on the logind
session of tty1. The setup of the remote PC, such as the dependencies and the
permissions, is the package of `archlinux/PKGBUILD` of this skill. The ScreenCast
portal also needs the jimi package itself, which the user installs on the
remote PC from a clone of the repository with `cd archlinux && makepkg -si`.

```
remote.sh build [--asan|--release]  Copy the working tree and compile it
remote.sh start [--asan|--release] [--validate] [CMD...]  (Re)start the compositor, print its log
remote.sh stop                     Stop the compositor and the clients started by run
remote.sh status                   systemctl status of the compositor and the clients
remote.sh log [N]                  Last N lines of the latest run of the compositor
remote.sh run CMD...               Start a client in the background
remote.sh client-log [N]           Last N lines of the clients started by run
remote.sh wl CMD...                Run a command in the foreground, e.g. wayland-info
remote.sh shot [OUTPUT]            Screenshot with grim, prints the local PNG path
remote.sh input ACTION...          Inject input through uinput
remote.sh obs                      Start OBS capturing the screen through the portal
remote.sh cpu [SECONDS]            CPU usage of the processes, averaged over 10 seconds
remote.sh perf-record [SECONDS]    Profile the whole system for 10 seconds
remote.sh perf-report [ARGS...]    perf report of the last perf-record
remote.sh crash [NAME]             Backtrace of the latest core dump, e.g. of jimi
remote.sh dmesg [N]                Last N lines of the kernel log
remote.sh drm-info                 KMS state: connectors, CRTCs, planes and their framebuffers
remote.sh drm-debug on|off         Log the KMS and atomic commit details in dmesg
remote.sh reboot                   Reboot the remote PC, e.g. after a GPU hang
remote.sh setup                    Install the archlinux/PKGBUILD of this skill; the user runs it for sudo
```

## Workflow

1. `remote.sh build`, then `remote.sh start`. `start` stops the previous
   compositor, and prints the startup log, then the sockets. It fails if the
   compositor exited.
2. Drive the compositor with `run`, `input` and `wl`, and look at it with
   `shot`: Read the printed PNG path to see the image.
3. Check `log` after each step. If jimi died, `crash jimi` shows the
   backtrace. ASan builds print their report in `log` instead.

`start` runs the build of jimi by default. With a command, it runs that
instead, where `jimi` stands for the build: e.g. `start jimi -C jimi.yaml`,
or `start labwc` to compare with labwc. labwc and sway are installed on the
remote PC. The other commands work with any compositor that supports the
protocols they use, such as `ext-image-copy-capture-v1` for `shot` and
`wlr-output-management-unstable-v1` for `input`.

`start` uploads `config/` and runs the compositor in it, so the configs there
are passed by relative paths. Without them, the compositors read the configs of
the user on the remote PC, which may start a panel and other clients. The ones in
`config/` keep only the defaults, so that the compositors compare equally:

- `start jimi -C jimi.yaml`
- `start labwc -C labwc`
- `start sway -c sway.conf`

- `--validate` enables the Vulkan validation layers; its errors appear in
  `log`. Use it for any renderer change.
- `--asan` builds, and runs `jimi`, with AddressSanitizer and UBSan in a
  separate build directory, with `lsan.supp`.
- `--release` builds, and runs `jimi`, optimized like the packages of the
  other compositors, with frame pointers for perf. Use it to measure
  performance.
- The screenshot shows what the compositor rendered, not what the display scans
  out. For problems with planes, the cursor or page flips, also check
  `drm-info` for what is actually committed, and `dmesg` with `drm-debug on`
  for why commits fail. Ask the user to look at the monitor when in doubt.
- The clients that the compositor spawns itself, from autostart or bindings,
  log into `log`.

## Input

`input` creates a virtual keyboard and an absolute pointer, runs the actions in
order, then removes them. Each call takes about half a second for libinput to
pick up the devices, so batch the actions into one call.

- `move X Y`: Moves to a pixel of `shot`, in steps at 1000 pixels per second
  from the last position within the call. This holds with the one output of
  the remote PC; with more, jimi maps the pointer to the output nearest to
  0,0 in the layout, and wlroots compositors to the whole layout.
- `click left|right|middle`
- `down KEY|BUTTON`, `up KEY|BUTTON`: e.g. drags with
  `move 100 100 down left move 400 300 up left`
- `key COMBO`: e.g. `super+return`, `ctrl+shift+t`, `alt+f4`. Keys are the
  evdev names without `KEY_`, plus `super`, `ctrl`, `alt`, `shift`, `return`
  and `escape`
- `type TEXT`: Types ASCII text, assuming the US layout
- `scroll N`, `hscroll N`: Wheel clicks, positive for down and right
- `sleep SECONDS`
- `rate HZ`: The rate of the events of the following actions, 100 by default:
  each move step, key and button press or release, and wheel click waits
  for its turn. Real mice report at 125 to 1000 Hz.
- `loop SECONDS ACTION... [end]`: Repeats the actions up to `end`, or to the
  end of the call, until the time has passed, e.g. for a steady load during
  `perf stat`:
  `rate 1000 move 200 200 loop 20 move 1700 900 move 200 200`, or a drag
  with `move 900 240 down left loop 20 move 1300 500 move 900 240 end up left`

## Performance

To compare the CPU usage of the compositors, repeat for each with the same
scenario:

1. `start --release jimi -C jimi.yaml`, or another compositor with its config
   in `config/`, where sway floats the windows like the others
2. Optionally `obs`, then wait about 5 seconds for its window
3. Start the clients that make the load, last so that they are on top: a
   covered window gets no frame callbacks and stalls.
   - `run weston-simple-damage --width=1600 --height=900 --use-damage-buffer`:
     a large shm buffer, with a small part damaged in each frame, like the
     widgets of Qt or GTK applications, such as the audio meters of OBS
   - `run stdbuf -oL weston-simple-vulkan`: a small dma-buf drawn 60 times per
     second. `stdbuf` makes it log its frame rate every 5 seconds instead of at
     exit: check `client-log` for about 60 fps.
   Check `shot` that they are visible.
4. Wait about 10 seconds for everything to settle, then `cpu`: the
   compositor, the clients, the portals, PipeWire and the kernel workers of
   the GPU driver, in percent of one CPU
5. `perf-record`, then `perf-report`: by default, the time of each process in
   each library, including the idle `swapper`. Then narrow down to a process
   in percent of its own time, e.g.
   `perf-report --comms jimi --percentage relative --children --sort sym -g none --percent-limit 5`
   for the functions including their callees, or with `-g caller` for the call
   chains. The percentages in the call chains are of all the samples, even
   with `--percentage relative`.

To compare two builds of jimi, count the instructions instead: the CPU time,
and the cycles as well, of the same load can vary a lot from moment to moment,
e.g. with the clock speed or the core the CPU runs it on, while the
instructions over 15 seconds vary by a few percent:

```
remote.sh wl sh -c 'perf stat -e instructions,context-switches -p $(pidof jimi) -- sleep 15'
```

The context switches count how often the event loop wakes up. strace and
`perf trace` are not available. sway cannot be attached to, so compare it with
`cpu` only.

For the cost of input, start a looping `input` call in the background that
outlasts the window by a few seconds, wait about 4 seconds for its devices,
then run `perf stat`, and divide by the events. Each wheel click makes two
libinput events, a legacy axis event and a wheel event, and the uinput
pointer is absolute, so it skips the acceleration of relative mice. Python
may fall short of high rates such as 1000 Hz.

`obs` starts OBS with a fresh config from `config/obs/`, whose only source
captures the screen through the ScreenCast portal and PipeWire. It replaces the
portal backend, xdg-desktop-portal-wlr, with an instance that captures the
output without asking. The preview shows itself, so the screen changes in
every frame OBS captures, at 30 frames per second. The capture works when
`shot` shows the OBS window recursively in its preview; a black preview means
it failed, and `client-log` and `log` tell why.

perf records the whole system, since the work moves between the compositor,
the portal, PipeWire, OBS and the kernel, and since sway, with its file
capabilities, cannot be attached to. The process names in `--comms` are the
first 15 characters of the thread names, e.g. `jimi`, `labwc`, `sway`, and
`libobs: graphic` for the graphics thread of OBS. The libraries of the
packages, such as Mesa, are stripped, so their functions are addresses.

The ScreenCast portal for jimi is chosen by
`/usr/share/xdg-desktop-portal/jimi-portals.conf` from the jimi package on the
remote PC, not by the build. If the portal frontend lacks ScreenCast with jimi,
check that it is installed, and ask the user to install it if not.

## Recovery

If the compositor hangs, `stop` kills it after 5 seconds. If SSH stops
responding or the display stays broken after `stop`, run `reboot`, wait about a
minute, then go on. If the PC does not come back, ask the user.
