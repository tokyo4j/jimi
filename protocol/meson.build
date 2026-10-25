wayland_scanner = find_program('wayland-scanner')
protocol_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')

protocols = {
	'xdg-shell': protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	'linux-dmabuf-v1': protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
	'linux-drm-syncobj-v1': protocol_dir / 'staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml',
	'viewporter': protocol_dir / 'stable/viewporter/viewporter.xml',
	'presentation-time': protocol_dir / 'stable/presentation-time/presentation-time.xml',
	'tearing-control-v1': protocol_dir / 'staging/tearing-control/tearing-control-v1.xml',
	'color-management-v1': protocol_dir / 'staging/color-management/color-management-v1.xml',
	'fractional-scale-v1': protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
	'xdg-toplevel-icon-v1': protocol_dir / 'staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml',
	'xdg-activation-v1': protocol_dir / 'staging/xdg-activation/xdg-activation-v1.xml',
	'xdg-dialog-v1': protocol_dir / 'staging/xdg-dialog/xdg-dialog-v1.xml',
	'xdg-output-unstable-v1': protocol_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
	'xdg-decoration-unstable-v1': protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
	'kde-server-decoration': 'server-decoration.xml',
	'wlr-layer-shell-unstable-v1': 'wlr-layer-shell-unstable-v1.xml',
	'ext-foreign-toplevel-list-v1': protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
	'wlr-foreign-toplevel-management-unstable-v1': 'wlr-foreign-toplevel-management-unstable-v1.xml',
	'xfce-foreign-toplevel-management-private-v1': 'xfce-foreign-toplevel-management-private-v1.xml',
	'ext-workspace-v1': protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
	'wlr-output-management-unstable-v1': 'wlr-output-management-unstable-v1.xml',
	'xwayland-shell-v1': protocol_dir / 'staging/xwayland-shell/xwayland-shell-v1.xml',
	'ext-image-capture-source-v1': protocol_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
	'ext-image-copy-capture-v1': protocol_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
	'input-method-unstable-v1': protocol_dir / 'unstable/input-method/input-method-unstable-v1.xml',
	'text-input-unstable-v3': protocol_dir / 'unstable/text-input/text-input-unstable-v3.xml',
	'pointer-gestures-unstable-v1': protocol_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml',
	'relative-pointer-unstable-v1': protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
	'pointer-constraints-unstable-v1': protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
	'cursor-shape-v1': protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
	'primary-selection-unstable-v1': protocol_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
	'ext-data-control-v1': protocol_dir / 'staging/ext-data-control/ext-data-control-v1.xml',
	'tablet-v2': protocol_dir / 'stable/tablet/tablet-v2.xml',
	'ext-session-lock-v1': protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
	'ext-idle-notify-v1': protocol_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
	'idle-inhibit-unstable-v1': protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
	'virtual-keyboard-unstable-v1': 'virtual-keyboard-unstable-v1.xml',
	'wlr-virtual-pointer-unstable-v1': 'wlr-virtual-pointer-unstable-v1.xml',
}

foreach name, path : protocols
	protocol_sources += custom_target(
		name.underscorify() + '_c',
		input: path,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
	protocol_sources += custom_target(
		name.underscorify() + '_server_h',
		input: path,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach
