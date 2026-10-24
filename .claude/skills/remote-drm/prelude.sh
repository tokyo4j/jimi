# Sourced by the remote scripts of remote.sh. The compositor runs in the
# transient unit compositor.service of the user manager, so that it survives
# the SSH connection and logs to the journal.
set -euo pipefail

dir=$HOME/jimi-agent

# Exports WAYLAND_DISPLAY and, with Xwayland, DISPLAY of the sockets listening
# on the remote PC, which only runs the compositor. They are not matched by the
# processes, since ss cannot see the sockets of the processes with file
# capabilities, such as sway with CAP_SYS_NICE.
compositor_env() {
	if ! systemctl --user -q is-active compositor.service; then
		echo "The compositor is not running" >&2
		return 1
	fi
	local sockets x11
	sockets=$(ss -xlH | awk '{print $5}')
	WAYLAND_DISPLAY=$(sed -n "s|^$XDG_RUNTIME_DIR/\(wayland-[0-9]*\)$|\1|p" <<< "$sockets")
	if [ -z "$WAYLAND_DISPLAY" ]; then
		echo "The compositor has no Wayland socket" >&2
		return 1
	fi
	if [ "$(wc -l <<< "$WAYLAND_DISPLAY")" -gt 1 ]; then
		echo "Multiple Wayland sockets:" $WAYLAND_DISPLAY >&2
		return 1
	fi
	export WAYLAND_DISPLAY
	x11=$(sed -n 's|^@\?/tmp/.X11-unix/X\([0-9]*\)$|\1|p' <<< "$sockets" | sort -u)
	if [ -n "$x11" ]; then
		export DISPLAY=:$x11
	fi
}

# Starts a client of the compositor, with the extra variables of the leading
# -E options, as a transient unit, and prints its name. Takes the variables of
# compositor_env.
run_client() {
	local env=(-E WAYLAND_DISPLAY="$WAYLAND_DISPLAY" -E XDG_SESSION_TYPE=wayland)
	if [ -n "${DISPLAY:-}" ]; then
		env+=(-E DISPLAY="$DISPLAY")
	fi
	while [ "$1" = -E ]; do
		env+=(-E "$2")
		shift 2
	done
	local unit
	unit=client-$(basename "$1")-$$
	unit=${unit//[^a-zA-Z0-9:_.-]/_}
	systemd-run --user --quiet --unit="$unit" --collect "${env[@]}" "$@"
	echo "$unit"
}
