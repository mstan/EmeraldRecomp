#!/usr/bin/env python3
"""Touch-input scenario gates for EmeraldRecomp.

Launches the game headless with the debug TCP server, loads a savestate, and
drives it only through the engine's scripted touch commands (the same path a
finger takes: TouchHub -> gesture recognizer -> Emerald touch policy ->
synthesized keys). Each scenario asserts on guest state and on the always-on
touch rings, then reports coverage honesty from the exit banner.

    python tools/touch_scenarios.py --exe build-touch/EmeraldRecomp.exe \
        --bios ../gbarecomp/bios/gba_bios.bin --rom variants/emerald/roms/emerald_usa.gba \
        --state variants/emerald/roms/fixtures/route101_walk.state --output out/touch

Scenarios are listed by --list; --only selects a subset.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

CB2_OVERWORLD = 0x08085E5C
CB2_BAG = 0x081AAD5C
GMAIN = 0x030022C0
PLAYER_AVATAR = 0x02037590
OBJECT_EVENTS = 0x02037350
START_ACTIONS = 0x02037610
NUM_START_ACTIONS = 0x0203760F
MENU_ACTION_BAG = 2


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.file = self.socket.makefile("rb")

    def call(self, cmd, **kwargs):
        self.socket.sendall((json.dumps(dict(cmd=cmd, **kwargs)) + "\n").encode())
        response = json.loads(self.file.readline())
        if not response.get("ok", False):
            raise RuntimeError(f"{cmd}: {response}")
        return response

    def close(self):
        try:
            self.socket.sendall(b'{"cmd":"quit"}\n')
        except OSError:
            pass
        self.file.close()
        self.socket.close()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Game:
    def __init__(self, args, name, state=None):
        self.root = args.output / name
        self.root.mkdir(parents=True, exist_ok=True)
        exe = self.root / args.exe.name
        shutil.copy2(args.exe, exe)
        for dll in args.exe.parent.glob("*.dll"):
            shutil.copy2(dll, self.root / dll.name)
        catalog = Path(__file__).resolve().parents[1] / "mods" / "preloaded"
        shutil.copytree(catalog, self.root / "mods", dirs_exist_ok=True)
        aspect = args.aspect
        (self.root / "mods" / "state.toml").write_text(
            'format_version = 1\n[[package]]\nid = "pokemon-emerald.enhancement.widescreen"\n'
            'version = "0.2.0"\n[[feature]]\npackage_id = "pokemon-emerald.enhancement.widescreen"\n'
            f'id = "widescreen"\nenabled = {"true" if aspect != "native" else "false"}\n'
            f'[feature.values]\naspect = "{aspect if aspect != "native" else "fit"}"\n')
        env = {k: v for k, v in os.environ.items() if not k.startswith("GBARECOMP_")}
        env.update(RECOMP_RTC_EPOCH="1789261200")
        if not args.bridge:
            env["GBARECOMP_STRICT_STATIC"] = "1"   # a dispatch miss aborts the run
        if args.toolchain:
            env["PATH"] = str(args.toolchain) + os.pathsep + env["PATH"]
        port = free_port()
        self.log = (self.root / "run.log").open("w")
        self.process = subprocess.Popen(
            [str(exe), "--bios", str(args.bios), "--rom", str(args.rom),
             "--save-path", str(self.root / "test.sav"), "--no-window", "--tcp", str(port)],
            cwd=self.root, env=env, stdout=self.log, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        deadline = time.monotonic() + 30
        while True:
            if self.process.poll() is not None:
                raise RuntimeError(f"{name}: exited early ({self.process.returncode})")
            try:
                self.c = Client(port)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.args_state = state or args.state
        self.c.call("savestate_load", path=str(self.args_state))
        self.frames(2)

    # ── primitives ───────────────────────────────────────────────────────
    def frames(self, n):
        return self.c.call("run_frames", n=n)

    def mem(self, region, addr, size):
        return bytes.fromhex(self.c.call("read_" + region, addr=hex(addr), len=size)["data"])

    def u8(self, addr):
        return self.mem("ewram" if addr < 0x03000000 else "iwram", addr, 1)[0]

    def u32(self, addr):
        return struct.unpack("<I", self.mem("ewram" if addr < 0x03000000 else "iwram", addr, 4))[0]

    def cb2(self):
        return self.u32(GMAIN + 4) & ~1

    def player(self):
        obj = self.u8(PLAYER_AVATAR + 5)
        x, y = struct.unpack("<hh", self.mem("ewram", OBJECT_EVENTS + obj * 0x24 + 0x10, 4))
        return x, y

    def status(self):
        return self.c.call("emerald_touch_status")

    def view(self, nx, ny):
        off = self.status()["view_offset"]
        return nx + off[0], ny + off[1]

    def tap(self, nx, ny, settle=30):
        x, y = self.view(nx, ny)
        self.c.call("touch_tap", x=x, y=y, hold_frames=3)
        self.frames(settle)

    def double_tap(self, nx, ny, gap=6, settle=40):
        x, y = self.view(nx, ny)
        self.c.call("touch_tap", x=x, y=y, hold_frames=2)
        self.frames(gap)
        self.c.call("touch_tap", x=x, y=y, hold_frames=2)
        self.frames(settle)

    def long_press(self, nx, ny, settle=20):
        x, y = self.view(nx, ny)
        self.c.call("touch_long_press", x=x, y=y, hold_frames=34)
        self.frames(34 + settle)

    def drag(self, points, per=2, settle=10):
        pts = [list(self.view(x, y)) for x, y in points]
        self.c.call("touch_drag", points=pts, frames_per_point=per)
        self.frames(len(pts) * per + settle)

    def wait_until(self, predicate, limit=600, step=10):
        for _ in range(0, limit, step):
            if predicate():
                return True
            self.frames(step)
        return predicate()

    def actions(self):
        return self.c.call("emerald_touch_actions", limit=64)["actions"]

    def shot(self, name):
        s = self.c.call("screenshot")
        w, h, data = s["w"], s.get("h", 160), bytes.fromhex(s["data"])
        raw = b"".join(b"\0" + data[y * w * 3:(y + 1) * w * 3] for y in range(h))
        def chunk(tag, payload):
            return (struct.pack(">I", len(payload)) + tag + payload +
                    struct.pack(">I", zlib.crc32(tag + payload)))
        path = self.root / f"{name}.png"
        path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                         chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                         chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        return path

    def close(self):
        coverage = "UNKNOWN"
        try:
            m = self.c.call("misses")
            strict = not (m.get("distinct_misses") or m.get("interpreted_insns") or
                          m.get("healed_native"))
            coverage = m.get("coverage", "UNKNOWN") if strict else f"NOT_STATIC {json.dumps(m)}"
        except Exception as e:
            coverage = f"UNKNOWN ({e})"
        try:
            self.c.close()
            self.process.wait(timeout=30)
        except Exception:
            self.process.kill()
        self.log.close()
        return coverage


# Player tile (at rest) covers native pixels x 112..127, y 72..87.
def tile_center(dx, dy):
    return 120 + 16 * dx, 80 + 16 * dy


def scenario_long_press_start(g):
    assert g.status()["field_free"], "precondition: free overworld"
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), "start menu did not open"
    items = g.status()["menu_items"]
    assert items, "start menu items not reported"
    x, y, w, h = items[0]
    g.double_tap(x - 60, y + h // 2)   # double tap off the menu: B closes it
    assert g.wait_until(lambda: not g.status()["menu"], 60), "double tap did not close"
    assert "double-tap:B" in [a["decision"] for a in g.actions()]
    return {"start_items": len(items)}


def scenario_start_menu_bag(g):
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), "start menu did not open"
    count = g.u8(NUM_START_ACTIONS)
    actions = list(g.mem("ewram", START_ACTIONS, count))
    index = actions.index(MENU_ACTION_BAG)
    x, y, w, h = g.status()["menu_items"][index]
    g.tap(x + w // 2, y + h // 2, settle=10)
    assert g.wait_until(lambda: g.cb2() == CB2_BAG, 240), f"bag did not open (cb2={g.cb2():08X})"
    g.frames(60)
    rows = g.status()["list_rows"]
    # Back out with a two-finger tap (B) and wait for the field again.
    for _ in range(4):
        if g.cb2() == CB2_OVERWORLD:
            break
        g.c.call("touch_two_finger_tap", x=100, y=80)
        g.frames(60)
    assert g.wait_until(lambda: g.cb2() == CB2_OVERWORLD, 300), "did not return to the field"
    return {"start_index": index, "bag_rows": len(rows)}


def scenario_tap_walk(g):
    start = g.player()
    target_offsets = [(3, 0), (-3, 0), (0, 2), (0, -2), (2, 2), (-2, -2)]
    for dx, dy in target_offsets:
        before = g.player()
        g.tap(*tile_center(dx, dy), settle=5)
        g.wait_until(lambda: not g.status()["macro"] and not g.c.call("emerald_touch_field")["active"],
                     240, 5)
        after = g.player()
        field = g.c.call("emerald_touch_field")
        if after == (before[0] + dx, before[1] + dy):
            return {"from": start, "reached": after, "offset": [dx, dy],
                    "mismatches": field["mismatches"]}
    raise AssertionError(f"no tapped tile reached; last field={field}")


def scenario_draw_path(g):
    # L-shaped strokes drawn finely (4 px samples); the first one whose end
    # is reachable must move the player along it with no model mismatch.
    attempts = []
    for sx, sy, ex, ey in [(1, 0, 3, -2), (-1, 0, -3, -2), (1, 0, 3, 2), (-1, 0, -3, 2),
                           (0, -1, 2, -3), (0, 1, 2, 3)]:
        before = g.player()
        pts = [tile_center(0, 0)]
        ax, ay = tile_center(ex * (sx != 0), ey * (sy != 0) if sy else 0)
        corner = tile_center(ex if sx else 0, ey if sy else 0)
        end = tile_center(ex, ey)
        for (x0, y0), (x1, y1) in [(pts[0], corner), (corner, end)]:
            n = max(abs(x1 - x0), abs(y1 - y0)) // 4
            for i in range(1, n + 1):
                pts.append((x0 + (x1 - x0) * i // n, y0 + (y1 - y0) * i // n))
        g.drag(pts, per=1)
        g.wait_until(lambda: not g.c.call("emerald_touch_field")["active"], 300, 5)
        after = g.player()
        field = g.c.call("emerald_touch_field")
        moved = abs(after[0] - before[0]) + abs(after[1] - before[1])
        attempts.append({"end": [ex, ey], "from": before, "to": after, "moved": moved})
        if moved >= 2:
            assert field["mismatches"] == 0, f"model mismatches: {field}"
            return {"attempts": attempts, "steps": field["steps"]}
    raise AssertionError(f"no stroke moved the player: {attempts}")


CB2_PARTY = 0x081B01B0
MENU_ACTION_POKEMON = 1


def scenario_party_menu(g):
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), "start menu did not open"
    count = g.u8(NUM_START_ACTIONS)
    actions = list(g.mem("ewram", START_ACTIONS, count))
    if MENU_ACTION_POKEMON not in actions:
        return {"skipped": "no party yet"}
    x, y, w, h = g.status()["menu_items"][actions.index(MENU_ACTION_POKEMON)]
    g.tap(x + w // 2, y + h // 2, settle=10)
    assert g.wait_until(lambda: g.cb2() == CB2_PARTY, 240), f"party did not open ({g.cb2():08X})"
    g.frames(40)
    g.tap(48, 50, settle=30)                      # first slot (big box)
    assert g.wait_until(lambda: g.status()["menu"], 90), "slot tap did not open the action menu"
    items = g.status()["menu_items"]
    g.double_tap(20, 20)                          # double tap off the popup: B
    assert g.wait_until(lambda: not g.status()["menu"], 90), "popup did not close"
    for _ in range(4):
        if g.cb2() == CB2_OVERWORLD:
            break
        g.c.call("touch_two_finger_tap", x=100, y=80)
        g.frames(60)
    assert g.wait_until(lambda: g.cb2() == CB2_OVERWORLD, 300), "did not return to the field"
    return {"popup_items": len(items)}


MENU_ACTION_SAVE = 5


def open_save_prompt(g):
    """Start menu -> SAVE -> the "Would you like to save the game?" Yes/No."""
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), "start menu did not open"
    count = g.u8(NUM_START_ACTIONS)
    actions = list(g.mem("ewram", START_ACTIONS, count))
    start_items = g.status()["menu_items"]
    x, y, w, h = start_items[actions.index(MENU_ACTION_SAVE)]
    g.tap(x + w // 2, y + h // 2, settle=10)
    prompt = lambda: (lambda s: s["menu"] and not s["start_menu"] and s["dialogue_box"]
                      and len(s["menu_items"]) == 2)(g.status())
    assert g.wait_until(prompt, 240, 5), f"save Yes/No prompt did not appear ({g.status()})"
    s = g.status()
    assert s["menu_cursor"] == 0, f"caret should start on YES ({s['menu_cursor']})"
    s["start_items"] = start_items
    return s


def scenario_save_prompt_tap_no(g):
    # Tapping a choice confirms it, even when the caret is elsewhere.
    s = open_save_prompt(g)
    x, y, w, h = s["menu_items"][1]
    g.tap(x + w // 2, y + h // 2, settle=10)
    assert g.wait_until(lambda: g.status()["start_menu"], 120, 5), \
        f"NO did not return to the Start menu ({g.status()})"
    decisions = [a["decision"] for a in g.actions()]
    assert "menu-item" in decisions, decisions
    return {"after": "start menu"}


SAVE_COUNTER = 0x03006200   # gSaveCounter: incremented by every completed save


def scenario_save_prompt_tap_outside(g):
    # Tapping off a prompt confirms the highlighted choice. Answer every YES
    # prompt (save? / overwrite?) that way and advance the text by taps: the
    # game must complete a save, never fall back to the Start menu (= NO).
    open_save_prompt(g)
    before = g.u32(SAVE_COUNTER)
    prompts = 0
    for _ in range(60):
        s = g.status()
        if g.u32(SAVE_COUNTER) != before and s["field_free"]:
            break
        assert not s["start_menu"], "a prompt was answered NO (back at the Start menu)"
        if s["menu"] and s["dialogue_box"]:
            g.tap(60, 40, settle=20)              # the field above the prompt
            prompts += 1
        elif s["text_waiting"]:
            g.tap(120, 140, settle=20)
        else:
            g.frames(20)
    decisions = [a["decision"] for a in g.actions()]
    assert "tap-outside-prompt:A" in decisions, decisions
    assert g.u32(SAVE_COUNTER) != before, f"YES did not complete a save ({decisions[-6:]})"
    return {"prompts_confirmed": prompts, "save_counter": [before, g.u32(SAVE_COUNTER)]}


def scenario_queued_tap(g):
    # A tap while a macro runs is queued and executed, never dropped: tap NO,
    # then tap where EXIT will be on the Start menu before the NO macro ends.
    s = open_save_prompt(g)
    x, y, w, h = s["menu_items"][1]
    vx, vy = g.view(x + w // 2, y + h // 2)
    count = g.u8(NUM_START_ACTIONS)
    ex, ey, ew, eh = s["start_items"][count - 1]  # EXIT is the last Start action
    g.c.call("touch_tap", x=vx, y=vy, hold_frames=2)
    g.frames(3)
    ox, oy = g.view(ex + ew // 2, ey + eh // 2)
    g.c.call("touch_tap", x=ox, y=oy, hold_frames=2)
    g.frames(4)
    decisions = [a["decision"] for a in g.actions()]
    assert "queued:busy" in decisions, f"second tap was not queued: {decisions}"
    assert g.wait_until(lambda: not g.status()["menu"] and g.status()["field_free"], 240, 5), \
        f"queued tap never closed the Start menu ({g.status()}, {decisions})"
    return {"decisions": decisions[-4:]}


SCRIPT_CONTEXT = 0x03000E40         # sGlobalScriptContext: mode +1, nativePtr +4
WAIT_FOR_A_OR_B_PRESS = 0x0809AC98


def nearby_npc(g, max_dx=6, max_dy=3):
    """Offset (dx, dy) of the nearest active non-player object event in view."""
    me = g.u8(PLAYER_AVATAR + 5)
    px, py = g.player()
    best = None
    for i in range(16):
        if i == me:
            continue
        raw = g.mem("ewram", OBJECT_EVENTS + i * 0x24, 0x24)
        if not raw[0] & 1:                            # active
            continue
        x, y = struct.unpack_from("<hh", raw, 0x10)
        dx, dy = x - px, y - py
        if (dx or dy) and abs(dx) <= max_dx and abs(dy) <= max_dy:
            if best is None or abs(dx) + abs(dy) < abs(best[0]) + abs(best[1]):
                best = (dx, dy)
    return best


def scenario_stale_script_wait(g):
    # Talking to an NPC ends with waitbuttonpress; StopScript leaves the
    # context's nativePtr at WaitForAorBPress. The free overworld must still
    # take a long press (Start) and taps must walk, not press A.
    npc = nearby_npc(g)
    assert npc, "no NPC in view of the fixture"
    g.tap(*tile_center(*npc), settle=5)
    talked = g.wait_until(lambda: g.status()["text_waiting"] or g.status()["text_printing"],
                          600, 5)
    assert talked, f"tapping the NPC at {npc} started no dialogue"
    for _ in range(40):
        s = g.status()
        if s["field_free"] and not s["text_waiting"] and not s["text_printing"]:
            break
        g.tap(120, 140, settle=20)
    assert g.wait_until(lambda: g.status()["field_free"], 300), "dialogue never ended"
    _, mode, _, _, native = struct.unpack("<BBBBI", g.mem("iwram", SCRIPT_CONTEXT, 8))
    stale = mode == 0 and (native & ~1) == WAIT_FOR_A_OR_B_PRESS
    assert not g.status()["script_wait"], "stopped script still reported as waiting"
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), \
        f"long press after dialogue did not open Start ({g.actions()[-3:]})"
    return {"npc": npc, "stale_native_ptr": stale}


def scenario_start_menu_slide(g):
    # Slide a finger down the Start menu and lift on BAG: the caret follows
    # the finger and the release confirms (a sloppy tap becomes a slide).
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), "start menu did not open"
    count = g.u8(NUM_START_ACTIONS)
    actions = list(g.mem("ewram", START_ACTIONS, count))
    items = g.status()["menu_items"]
    x, y, w, h = items[0]
    bx, by, bw, bh = items[actions.index(MENU_ACTION_BAG)]
    cx = x + w // 2
    pts = [(cx, y + h // 2 + k) for k in range(0, (by + bh // 2) - (y + h // 2) + 1, 3)]
    g.drag(pts, per=2, settle=10)
    decisions = [a["decision"] for a in g.actions()]
    assert "menu-slide-select" in decisions, decisions
    assert g.wait_until(lambda: g.cb2() == CB2_BAG, 240), f"slide did not open the bag ({decisions})"
    return {"hovered": decisions.count("menu-hover")}


def scenario_menu_tap_anywhere_confirms(g):
    # A single tap off the Start menu confirms the highlighted item (A) once
    # the double-tap window passes.
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), "start menu did not open"
    count = g.u8(NUM_START_ACTIONS)
    actions = list(g.mem("ewram", START_ACTIONS, count))
    cursor = g.status()["menu_cursor"]
    g.tap(40, 120, settle=40)
    decisions = [a["decision"] for a in g.actions()]
    assert "tap-outside-menu:A" in decisions, decisions
    assert not g.status()["start_menu"], "tap off the menu did not confirm"
    return {"confirmed_action": actions[cursor]}


def scenario_menu_remote_swipe(g):
    # A vertical drag anywhere off the menu moves its cursor (finger down =
    # cursor down); the menu stays open.
    g.long_press(*tile_center(0, -3))
    assert g.wait_until(lambda: g.status()["start_menu"], 60), "start menu did not open"
    before = g.status()["menu_cursor"]
    pts = [(40, 40 + k) for k in range(0, 52, 4)]      # 48 native px down: two steps
    g.drag(pts, per=2, settle=20)
    s = g.status()
    decisions = [a["decision"] for a in g.actions()]
    assert decisions.count("remote-down") == 2, decisions
    assert s["start_menu"], "remote swipe closed the menu"
    count = g.u8(NUM_START_ACTIONS)
    assert s["menu_cursor"] == (before + 2) % count, (before, s["menu_cursor"])
    return {"cursor": [before, s["menu_cursor"]]}


def scenario_battle_tap_anywhere(g):
    # Battle action menu (fixture): a tap on the scene confirms the
    # highlighted FIGHT; a double tap in the move menu backs out to actions.
    control = lambda: g.status()["battle_control"]
    assert g.wait_until(lambda: control() == "action", 300), f"not at the action menu ({control()})"
    assert g.u8(0x020244AC) == 0, "FIGHT should be highlighted in the fixture"
    g.tap(60, 40, settle=40)                          # battle scene background
    assert g.wait_until(lambda: control() == "move", 120), f"tap did not confirm FIGHT ({control()})"
    g.double_tap(60, 40)
    assert g.wait_until(lambda: control() == "action", 120), f"double tap did not back out ({control()})"
    decisions = [a["decision"] for a in g.actions()]
    assert "tap-outside-menu:A" in decisions and "double-tap:B" in decisions, decisions
    return {"decisions": decisions[-4:]}


BATTLE_MAIN_CB2 = 0x08038420


def encounter(g, limit_taps=60):
    """Pace the tall grass left of the Route 101 start by taps until a wild
    battle starts. Returns True once BattleMainCB2 runs."""
    targets = [tile_center(-5, -3), tile_center(-5, -1), tile_center(-6, -3), tile_center(-6, -1)]
    for i in range(limit_taps):
        if g.cb2() == BATTLE_MAIN_CB2:
            return True
        if g.cb2() != CB2_OVERWORLD:
            # Battle transition / intro in progress.
            return g.wait_until(lambda: g.cb2() == BATTLE_MAIN_CB2, 900, 10)
        if g.status()["field_free"]:
            g.tap(*targets[i % len(targets)], settle=5)
        g.wait_until(lambda: g.cb2() == BATTLE_MAIN_CB2 or
                     not g.c.call("emerald_touch_field")["active"], 200, 5)
    return g.cb2() == BATTLE_MAIN_CB2


def advance_text_until(g, predicate, limit=1500):
    """Tap to advance battle text until `predicate` holds."""
    for _ in range(0, limit, 20):
        if predicate():
            return True
        s = g.status()
        if s["text_waiting"] or s["text_printing"]:
            g.tap(120, 60, settle=5)
        g.frames(15)
    return predicate()


def scenario_wild_battle(g):
    assert encounter(g), "no wild encounter within the tap budget"
    fixture = Path(g.args_state).parent / "wild_battle.state"
    g.frames(5)
    control = lambda: g.status()["battle_control"]
    assert advance_text_until(g, lambda: control() == "action"), "never reached the action menu"
    g.c.call("savestate_save", path=str(fixture))
    g.shot("action_menu")
    # FIGHT via the in-image menu (native coords): top-left quadrant.
    g.tap(152, 128, settle=20)
    assert g.wait_until(lambda: control() == "move", 120), f"FIGHT did not open moves ({control()})"
    g.shot("move_menu")
    # First move (top-left move window).
    g.tap(40, 128, settle=10)
    assert advance_text_until(g, lambda: control() == "action" or g.cb2() == CB2_OVERWORLD, 3000), \
        "turn never completed"
    if g.cb2() == CB2_OVERWORLD:
        return {"result": "won in one hit", "fixture": str(fixture)}
    # RUN (bottom-right quadrant) and return to the field.
    g.tap(208, 144, settle=20)
    assert advance_text_until(g, lambda: g.cb2() == CB2_OVERWORLD, 3000), "did not return to the field"
    return {"result": "fought one turn then ran", "fixture": str(fixture)}


# Scenarios that need another fixture (resolved next to --state).
SCENARIO_FIXTURES = {
    "stale_script_wait": "door_menu.state",   # Littleroot: NPCs in view
    "battle_tap_anywhere": "wild_battle.state",  # saved at the action menu
}

SCENARIOS = {
    "wild_battle": scenario_wild_battle,
    "party_menu": scenario_party_menu,
    "long_press_start": scenario_long_press_start,
    "start_menu_bag": scenario_start_menu_bag,
    "tap_walk": scenario_tap_walk,
    "draw_path": scenario_draw_path,
    "save_prompt_tap_no": scenario_save_prompt_tap_no,
    "save_prompt_tap_outside": scenario_save_prompt_tap_outside,
    "queued_tap": scenario_queued_tap,
    "stale_script_wait": scenario_stale_script_wait,
    "start_menu_slide": scenario_start_menu_slide,
    "menu_tap_anywhere_confirms": scenario_menu_tap_anywhere_confirms,
    "menu_remote_swipe": scenario_menu_remote_swipe,
    "battle_tap_anywhere": scenario_battle_tap_anywhere,
}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("exe", "bios", "rom", "state", "output"):
        p.add_argument("--" + name, type=lambda s: Path(s).resolve(), required=name != "output",
                       default=None)
    p.add_argument("--toolchain", type=Path)
    p.add_argument("--aspect", default="native", choices=["native", "fit", "16:9", "21:9", "32:9"])
    p.add_argument("--only", nargs="*")
    p.add_argument("--bridge", action="store_true",
                   help="diagnostic: let the interpreter bridge dispatch misses (reported as "
                        "NOT_STATIC with the missed PCs) instead of aborting under strict static")
    p.add_argument("--list", action="store_true")
    args = p.parse_args()
    if args.list:
        print("\n".join(SCENARIOS))
        return 0
    args.output = args.output or Path("touch_scenarios_out").resolve()
    results, failed = {}, 0
    for name, fn in SCENARIOS.items():
        if args.only and name not in args.only:
            continue
        fixture = SCENARIO_FIXTURES.get(name)
        state = args.state.parent / fixture if fixture else None
        if state is not None and not state.exists():
            raise SystemExit(f"{name}: fixture {state} missing")
        game = Game(args, name, state)
        try:
            detail = fn(game)
            results[name] = {"ok": True, **(detail or {})}
        except Exception as e:  # report every scenario
            failed += 1
            try:
                acts = game.actions()[-12:]
            except Exception:
                acts = []
            results[name] = {"ok": False, "error": str(e), "recent_actions": acts}
        finally:
            results[name]["coverage"] = game.close()
    print(json.dumps(results, indent=1))
    (args.output / "touch_scenarios.json").write_text(json.dumps(results, indent=1))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
