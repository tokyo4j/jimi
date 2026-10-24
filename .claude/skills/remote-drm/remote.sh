#!/bin/bash
# Builds jimi on a remote PC over SSH, and runs and inspects it, or another
# compositor, on the DRM backend there.
# See SKILL.md for the commands.
set -euo pipefail

host=${JIMI_REMOTE:?"Set JIMI_REMOTE to the SSH destination of the remote PC"}
skill_dir=$(dirname "$(realpath "$0")")
repo=$(git -C "$skill_dir" rev-parse --show-toplevel)
# Screenshots are copied back here.
out=$repo/.cache/remote-drm

# Reuses one connection for the many short commands.
ssh_() {
	ssh -o ControlMaster=auto -o ControlPath="$HOME/.ssh/cm-%C" \
		-o ControlPersist=10m "$host" "$@"
}

# Runs the bash script on stdin remotely with the arguments, after the shared
# definitions.
remote() {
	cat "$skill_dir/prelude.sh" - | ssh_ "bash -s -- ${*@Q}"
}

usage() {
	sed -n '/^remote.sh /p' "$skill_dir/SKILL.md" >&2
	exit 1
}

# The build directory of the build option
build_dir() {
	case ${1:-} in
	--asan) echo build-asan ;;
	--release) echo build-release ;;
	*) echo build ;;
	esac
}

cmd=${1:-}
[ -n "$cmd" ] || usage
shift

case $cmd in
build)
	# Mirrors the working tree, including uncommitted changes. tar keeps the
	# mtimes, so ninja only rebuilds what changed.
	git -C "$repo" ls-files -co --exclude-standard -z \
		| tar -C "$repo" --null -T - -cf - \
		| ssh_ 'rm -rf jimi-agent/src && mkdir -p jimi-agent/src && tar -xf - -C jimi-agent/src'
	remote "$(build_dir "${1:-}")" <<'EOF'
build=$dir/$1
if [ ! -d "$build" ]; then
	case $1 in
	build-asan) options=(-Db_sanitize=address,undefined) ;;
	# Optimized like the packages of Arch Linux, with the frame pointers
	# perf walks the stacks with
	build-release) options=(-Dbuildtype=debugoptimized
		"-Dc_args=-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer") ;;
	*) options=() ;;
	esac
	meson setup "${options[@]}" "$build" "$dir/src"
fi
meson compile -C "$build"
EOF
	;;
start)
	build= validate=
	while [ $# -gt 0 ]; do
		case $1 in
		--asan|--release) build=$1 ;;
		--validate) validate=1 ;;
		-*) usage ;;
		*) break ;;
		esac
		shift
	done
	tar -C "$skill_dir/config" -cf - . \
		| ssh_ 'rm -rf jimi-agent/config && mkdir -p jimi-agent/config && tar -xf - -C jimi-agent/config'
	remote "$(build_dir "$build")" "$validate" "${@:-jimi}" <<'EOF'
build=$1 validate=$2
shift 2
if [ "$1" = jimi ]; then
	set -- "$dir/$build/jimi" "${@:2}"
fi
systemctl --user stop compositor.service 'client-*' 2>/dev/null || true
# Like after a new login, the user manager has none of the variables that
# compositors export to it, and the portals are activated again with the ones
# of the next compositor. They are stopped after the clients, which may
# activate them while exiting, and the failures without a compositor are reset.
# A crashed compositor would otherwise leave e.g. its socket, in which the next
# one would try to nest.
systemctl --user stop 'xdg-desktop-portal*'
systemctl --user reset-failed 'xdg-desktop-portal*'
systemctl --user unset-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP \
	XDG_SESSION_TYPE XCURSOR_SIZE XCURSOR_THEME LABWC_PID SWAYSOCK
# The compositor takes the devices of the session on the VT, as if started
# from it.
session=$(loginctl show-user "$USER" -p Display --value)
env=(-E LIBSEAT_BACKEND=logind -E XDG_SESSION_ID="$session")
[ -n "$validate" ] && env+=(-E VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation)
[ "$build" = build-asan ] && env+=(-E LSAN_OPTIONS=suppressions=$dir/src/lsan.supp)
systemd-run --user --quiet --unit=compositor --collect -p TimeoutStopSec=5 \
	--working-directory="$dir/config" "${env[@]}" "$@"
for _ in $(seq 100); do
	systemctl --user -q is-active compositor.service || break
	(compositor_env 2>/dev/null) && break
	sleep 0.1
done
sleep 0.5
journalctl --user -u compositor -I -o cat --no-pager
if ! systemctl --user -q is-active compositor.service; then
	echo "The compositor exited" >&2
	exit 1
fi
compositor_env
echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY DISPLAY=${DISPLAY:-}"
EOF
	;;
stop)
	remote <<'EOF'
systemctl --user stop compositor.service 'client-*'
EOF
	;;
status)
	remote <<'EOF'
systemctl --user status compositor.service 'client-*' --no-pager || true
EOF
	;;
log)
	remote "${1:-200}" <<'EOF'
journalctl --user -u compositor -I -o cat --no-pager -n "$1"
EOF
	;;
client-log)
	remote "${1:-200}" <<'EOF'
journalctl --user -u 'client-*' -o cat --no-pager -n "$1"
EOF
	;;
run)
	[ $# -gt 0 ] || usage
	remote "$@" <<'EOF'
compositor_env
run_client "$@"
EOF
	;;
wl)
	[ $# -gt 0 ] || usage
	remote "$@" <<'EOF'
compositor_env
"$@"
EOF
	;;
shot)
	mkdir -p "$out"
	file=$out/shot-$(date +%H%M%S).png
	remote "$@" > "$file" <<'EOF'
compositor_env
if [ $# -gt 0 ]; then
	grim -o "$1" -
else
	grim -
fi
EOF
	echo "$file"
	;;
input)
	[ $# -gt 0 ] || usage
	{
		echo "compositor_env"
		echo "python3 - \"\$@\" <<'PYTHON'"
		cat "$skill_dir/uinput.py"
		echo "PYTHON"
	} | remote "$@"
	;;
obs)
	remote <<'EOF'
compositor_env
# Replaces the portal activated with its output chooser, before the portal
# frontend, activated by OBS, looks for it.
run_client /usr/lib/xdg-desktop-portal-wlr --replace -c "$dir/config/xdpw.conf"
gdbus wait --session --timeout 5 org.freedesktop.impl.portal.desktop.wlr
# With the config uploaded by start, which captures the screen
run_client -E XDG_CONFIG_HOME="$dir/config/obs" obs --disable-shutdown-check
EOF
	;;
cpu)
	remote "${1:-10}" <<'EOF'
# The second report of top averages over the interval. The command lines tell
# apart the processes with truncated names, such as the portals.
top -b -c -d "$1" -n 2 -o %CPU -w 160 \
	| awk '/^top -/ {n++} n == 2 && lines++ < 22'
EOF
	;;
perf-record)
	remote "${1:-10}" <<'EOF'
# The whole system, since the work is split among the compositor, the portal,
# PipeWire, the clients and the kernel, and sway cannot be attached to with its
# file capabilities.
perf record -a -g -F 999 -o "$dir/perf.data" -- sleep "$1" 2>&1 | tail -n 2
EOF
	;;
perf-report)
	# The time of each process in each library, with the idle swapper
	[ $# -gt 0 ] || set -- --no-children --sort comm,dso -g none \
		--percent-limit 0.1
	remote "$@" <<'EOF'
perf report -i "$dir/perf.data" --stdio "$@" 2> /dev/null
EOF
	;;
crash)
	remote "$@" <<'EOF'
coredumpctl info -1 --no-pager "$@"
EOF
	;;
dmesg)
	remote "${1:-100}" <<'EOF'
dmesg | tail -n "$1"
EOF
	;;
drm-info)
	remote <<'EOF'
drm_info
EOF
	;;
drm-debug)
	# DRIVER, KMS, PRIME and ATOMIC: enough to explain failed atomic
	# commits.
	case ${1:-} in
	on) mask=0x1e ;;
	off) mask=0 ;;
	*) usage ;;
	esac
	remote "$mask" <<'EOF'
echo "$1" | sudo -n tee /sys/module/drm/parameters/debug > /dev/null
EOF
	;;
reboot)
	remote <<'EOF'
sudo -n systemctl reboot
EOF
	;;
setup)
	# Builds and installs the package on the remote PC. sudo asks for the
	# password, so the user runs this in a terminal.
	pkg_dir=$(tar -C "$skill_dir/archlinux" -cf - . \
		| ssh_ 'dir=$(mktemp -d) && tar -xf - -C "$dir" && echo "$dir"')
	ssh -t "$host" "cd $pkg_dir && makepkg -si"
	;;
*)
	usage
	;;
esac
