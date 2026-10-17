#!/usr/bin/env python3
"""m7drive.py: drive the M7 Snow 512Ke (Xvfb :77 in the talkipm7 namespace) one step at a time.

Uses the emulators' canonical XTEST library (~/emulators/scripts/snow_automation.py). Mac coordinates.
    m7drive.py shot OUT.png
    m7drive.py click X Y | dclick X Y | sclick SX SY (screen coordinates)
    m7drive.py type TEXT | key return | back N | cmd q
    m7drive.py menu MENU_X ITEM_Y          (press on the menu title, drag to the item, release)
    m7drive.py menushot MENU_X OUT.png     (hold the menu open and screenshot it)
"""
import os
import sys
import time

sys.path.insert(0, os.path.expanduser("~/emulators/scripts"))
from snow_automation import SnowAutomation  # noqa: E402

DISPLAY = ":77"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "shot":
        os.system(f"DISPLAY={DISPLAY} import -window root {args[0]}")
        return 0
    # Xvfb :77 has no window manager, so X focus stays PointerRoot and winit never sees FocusIn: keys are
    # dropped. Give Snow's window the input focus explicitly.
    os.system(f"DISPLAY={DISPLAY} xdotool search --name Snow windowfocus --sync >/dev/null 2>&1")
    s = SnowAutomation(display_name=DISPLAY, auto_calibrate=True)
    s.ensure_focus()
    if cmd == "click":
        s.click_at_mac(int(args[0]), int(args[1]))
    elif cmd == "sclick":                   # X screen coordinates: calibration puts Mac x=0 at screen 62, but
        s.click_at(int(args[0]), int(args[1]))  # the Mac screen starts near 10, so the left edge is unreachable
    elif cmd == "dclick":
        s.double_click_at_mac(int(args[0]), int(args[1]))
    elif cmd == "type":
        s.type_text(" ".join(args))
    elif cmd == "key" and args[0] == "return":
        s.press_return()
    elif cmd == "key":                      # any X keysym name, e.g. KP_Enter, Tab
        s.key_sym_press(args[0])
    elif cmd == "back":
        for _ in range(int(args[0])):
            s.key_sym_press("BackSpace")
            time.sleep(0.15)  # faster than this and the emulated keyboard drops keys
    elif cmd == "cmd":
        s.cmd_key(args[0])
    elif cmd == "menu":
        s.menu_select(int(args[0]), int(args[1]))
    elif cmd == "menushot":                 # hold menu MENU_X open, screenshot to OUT, release on the title
        from Xlib import X
        from Xlib.ext import xtest
        s.move_to_mac(int(args[0]), 8)
        time.sleep(0.3)
        xtest.fake_input(s.d, X.ButtonPress, 1, X.CurrentTime)
        s.d.sync()
        time.sleep(1.0)
        os.system(f"DISPLAY={DISPLAY} import -window root {args[1]}")
        xtest.fake_input(s.d, X.ButtonRelease, 1, X.CurrentTime)
        s.d.sync()
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
