# Injects input into jimi through uinput, so that it goes through the kernel
# and libinput like real devices. Runs on the remote PC with WAYLAND_DISPLAY set
# by remote.sh; see SKILL.md for the actions.
import math
import re
import subprocess
import sys
import time

from evdev import AbsInfo, UInput, ecodes as e

KEY_ALIASES = {
    'super': e.KEY_LEFTMETA,
    'ctrl': e.KEY_LEFTCTRL,
    'alt': e.KEY_LEFTALT,
    'shift': e.KEY_LEFTSHIFT,
    'return': e.KEY_ENTER,
    'escape': e.KEY_ESC,
}
# The speed of the moves in pixels per second, like a quick hand
MOVE_SPEED = 1000
BUTTONS = {'left': e.BTN_LEFT, 'right': e.BTN_RIGHT, 'middle': e.BTN_MIDDLE}
# The keys of the characters on the US layout
CHARS = {
    ' ': 'space', '\n': 'enter', '\t': 'tab', '-': 'minus', '=': 'equal',
    '[': 'leftbrace', ']': 'rightbrace', '\\': 'backslash',
    ';': 'semicolon', "'": 'apostrophe', '`': 'grave', ',': 'comma',
    '.': 'dot', '/': 'slash',
}
SHIFTED_CHARS = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
    '*': '8', '(': '9', ')': '0', '_': 'minus', '+': 'equal',
    '{': 'leftbrace', '}': 'rightbrace', '|': 'backslash', ':': 'semicolon',
    '"': 'apostrophe', '~': 'grave', '<': 'comma', '>': 'dot', '?': 'slash',
}


def key_code(name):
    name = name.lower()
    return KEY_ALIASES.get(name) or e.ecodes['KEY_' + name.upper()]


# Returns the mode size of the output nearest to the origin of the layout,
# which jimi maps absolute pointers to. Transforms are not supported.
def output_size():
    output = subprocess.run(['wlr-randr'], capture_output=True, text=True,
                            check=True).stdout
    outputs = []
    for block in re.split(r'\n(?=\S)', output):
        if not re.search(r'^\s*Enabled: yes$', block, re.M):
            continue
        mode = re.search(r'(\d+)x(\d+) px.*current', block)
        position = re.search(r'Position: (-?\d+),(-?\d+)', block)
        scale = float(re.search(r'Scale: ([\d.]+)', block)[1])
        width, height = int(mode[1]), int(mode[2])
        x, y = int(position[1]), int(position[2])
        dx = max(x, -(x + width / scale), 0)
        dy = max(y, -(y + height / scale), 0)
        outputs.append((dx * dx + dy * dy, width, height))
    _, width, height = min(outputs)
    return width, height


class Devices:
    def __init__(self):
        self.width, self.height = output_size()
        self.keyboard = UInput({e.EV_KEY: range(e.KEY_ESC, e.KEY_MICMUTE + 1)},
                               name='jimi-agent keyboard')
        # A pixel of the output per unit
        self.pointer = UInput({
            e.EV_KEY: list(BUTTONS.values()),
            e.EV_ABS: [
                (e.ABS_X, AbsInfo(0, 0, self.width - 1, 0, 0, 0)),
                (e.ABS_Y, AbsInfo(0, 0, self.height - 1, 0, 0, 0)),
            ],
            e.EV_REL: [e.REL_WHEEL, e.REL_HWHEEL],
        }, name='jimi-agent pointer')
        self.pos = None
        # The events of the actions follow each other at this rate, in Hz.
        self.rate = 100
        # Until libinput adds the devices
        time.sleep(0.5)
        self.next_event = time.monotonic()

    def close(self):
        time.sleep(0.1)
        self.keyboard.close()
        self.pointer.close()

    # Waits for the time of the next event. After a stall, the rate starts
    # over instead of catching up in a burst.
    def wait(self):
        self.next_event = max(self.next_event + 1 / self.rate,
                              time.monotonic())
        time.sleep(max(0, self.next_event - time.monotonic()))

    def key(self, code, pressed):
        self.keyboard.write(e.EV_KEY, code, int(pressed))
        self.keyboard.syn()
        self.wait()

    def combo(self, combo):
        codes = [key_code(name) for name in combo.split('+')]
        for code in codes:
            self.key(code, True)
        for code in reversed(codes):
            self.key(code, False)

    def type(self, text):
        for char in text:
            shifted = char.isupper() or char in SHIFTED_CHARS
            name = SHIFTED_CHARS.get(char) or CHARS.get(char) or char
            if shifted:
                self.key(e.KEY_LEFTSHIFT, True)
            self.key(key_code(name), True)
            self.key(key_code(name), False)
            if shifted:
                self.key(e.KEY_LEFTSHIFT, False)

    # Moves at MOVE_SPEED in steps of the rate from the last position, so
    # that drags look like real ones.
    def move(self, x, y):
        x0, y0 = self.pos or (x, y)
        distance = math.hypot(x - x0, y - y0)
        steps = max(1, round(distance / MOVE_SPEED * self.rate))
        for i in range(1, steps + 1):
            self.pointer.write(e.EV_ABS, e.ABS_X,
                               round(x0 + (x - x0) * i / steps))
            self.pointer.write(e.EV_ABS, e.ABS_Y,
                               round(y0 + (y - y0) * i / steps))
            self.pointer.syn()
            self.wait()
        self.pos = (x, y)

    def button(self, name, pressed):
        self.pointer.write(e.EV_KEY, BUTTONS[name], int(pressed))
        self.pointer.syn()
        self.wait()

    # Positive clicks scroll down or right.
    def scroll(self, axis, clicks):
        self.pointer.write(e.EV_REL, axis, -clicks)
        self.pointer.syn()
        self.wait()


def run(devices, args):
    while args:
        action, args = args[0], args[1:]
        if action == 'move':
            devices.move(int(args[0]), int(args[1]))
            args = args[2:]
        elif action == 'click':
            devices.button(args[0], True)
            devices.button(args[0], False)
            args = args[1:]
        elif action in ('down', 'up'):
            pressed = action == 'down'
            if args[0] in BUTTONS:
                devices.button(args[0], pressed)
            else:
                devices.key(key_code(args[0]), pressed)
            args = args[1:]
        elif action == 'key':
            devices.combo(args[0])
            args = args[1:]
        elif action == 'type':
            devices.type(args[0])
            args = args[1:]
        elif action == 'scroll':
            devices.scroll(e.REL_WHEEL, int(args[0]))
            args = args[1:]
        elif action == 'hscroll':
            devices.scroll(e.REL_HWHEEL, -int(args[0]))
            args = args[1:]
        elif action == 'sleep':
            time.sleep(float(args[0]))
            args = args[1:]
        elif action == 'rate':
            devices.rate = float(args[0])
            args = args[1:]
        elif action == 'loop':
            body_end = args.index('end') if 'end' in args else len(args)
            end = time.monotonic() + float(args[0])
            while time.monotonic() < end:
                run(devices, args[1:body_end])
            args = args[body_end + 1:]
        else:
            sys.exit(f'Unknown action: {action}')


def main(args):
    devices = Devices()
    try:
        run(devices, args)
    finally:
        devices.close()


main(sys.argv[1:])
