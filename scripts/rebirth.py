#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebirth at the Doll (bbport.ini rebirth, game version 01.09): a new choice in the Doll's menu
puts the character back to its origin's attributes and level for REBIRTH_INSIGHT Insight, then
opens the level-up screen, where the levels it had are bought again for free. No Blood Echoes
come back: levels cost nothing only up to the level the character had (src/runtime_rebirth.c
keeps that target per character).

Three parts:
- The Doll's talk scripts (script/talk/m21_00_00_00.talkesdbnd.dcx: every list with Channel
  Blood Echoes, t210304's and others): "Rebirth" in the list, a Yes/No question, then the rebirth (event flag REQUEST, which the patch below turns into
  a call to the port), then the level-up screen on success or a message when Insight is short.
- The game's event flag setter: setting REQUEST calls the port ("bb:b") and sets RESULT to its
  answer instead. Every other flag is set as before.
- The level-up screen's pricing: raising or lowering an attribute asks the port ("bb:f" with the
  level) whether that level is free, and adds or takes off 0 Blood Echoes if so.
`code_writes` gives the eboot patches; `main` writes the talk script and texts as a mod layer
(the menu texts on top of --source, Quit to Desktop's layer, which edits the same archive).
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esd  # noqa: E402
import quit_menu  # noqa: E402

REBIRTH_INSIGHT = 5          # src/runtime_rebirth.c REBIRTH_INSIGHT
# Unused flags in the Doll's range (721000xx): the request, and the port's answer: open the
# level-up screen (reborn now, or levels still to regain), or nothing to do.
REQUEST, RESULT, NOTHING = 72100090, 72100091, 72100092
OPTION_TEXT, QUESTION_TEXT, SHORT_TEXT, NOTHING_TEXT = 15000170, 10010790, 10010791, 10010792  # free in FMG 30
TALK_FMG = 30
DOLL_SCRIPTS = 'm21_00_00_00.talkesdbnd.dcx'   # the Hunter's Dream: the Doll is t210304 and more
LEVEL_UP_TEXT = 15000100     # Channel Blood Echoes
OPTION = 4                   # the talk list's result for Rebirth (1-3 are the Doll's own)

# Image offsets in the 01.09 eboot.
BROWSER = quit_menu.BROWSER
SET_FLAG = 0x13CFCC0         # SetEventFlag(EventFlagMan*, flag, value)
SET_FLAG_RESUME = 0x13CFCC8  # after the instructions the jump replaces
RAISE = 0x169CA2D            # level-up screen, an attribute raised: price -> rax
LOWER = 0x169CA93            # an attribute lowered: price taken off
CAVE = 0x50D9900             # past Quit to Desktop's code in the code segment's last page
ORIGINAL = {
    SET_FLAG: '4189d0 8b4f1c 31d2',
    RAISE: 'c4e1fa2cc0 8b8bf8010000',
    LOWER: 'c4e1fa2cc0 2983f4010000',
}

TEXTS = {
    'engus': ('Rebirth',
              f'Be reborn for {REBIRTH_INSIGHT} Insight? Your attributes and level return to those of '
              f'your origin. Your levels can then be regained without Blood Echoes, now or later '
              f'with Channel Blood Echoes.',
              f'Rebirth requires {REBIRTH_INSIGHT} Insight.',
              'Your attributes are already those of your origin.'),
}


def code_writes():
    cave = quit_menu.Code(CAVE)
    # SetEventFlag: REQUEST goes to the port; RESULT is set to its answer.
    hook = cave.here
    cave.raw('81fe' + struct.pack('<I', REQUEST).hex())  # cmp esi, REQUEST
    cave.raw('740d')                                      # je rebirth
    original = cave.here
    cave.raw('4189d0 8b4f1c 31d2'.replace(' ', ''))       # the replaced instructions
    cave.jmp(SET_FLAG_RESUME)
    if cave.here - original != 13:
        raise ValueError('flag hook layout')
    # The port answers in eax: bit 0 opens the level-up screen (RESULT), bit 1 says there is
    # nothing to reset (NOTHING). Both flags are set through the original function.
    cave.raw('57')                                        # rebirth: push rdi (aligned)
    cave.browser(0)
    cave.raw('5f')                                        # pop rdi
    cave.raw('57 50 4883ec08'.replace(' ', ''))           # push rdi; push rax; sub rsp, 8
    cave.raw('89c2 83e201'.replace(' ', ''))              # mov edx, eax; and edx, 1
    cave.raw('be' + struct.pack('<I', RESULT).hex())      # mov esi, RESULT
    cave.call(original)
    cave.raw('4883c408 58 5f'.replace(' ', ''))           # add rsp, 8; pop rax; pop rdi
    cave.raw('89c2 d1ea 83e201'.replace(' ', ''))         # mov edx, eax; shr edx, 1; and edx, 1
    cave.raw('be' + struct.pack('<I', NOTHING).hex())     # mov esi, NOTHING
    cave.jmp(original)
    # Pricing: rax = the price; 0 when the port says the level is free. r13d is the price's
    # level + 0x51 when raising (the screen's level + 0x51), and the screen's level + 0x50 when
    # lowering: either way the level being bought or given back is r13d - 0x51.
    def price(level_fix, tail):
        at = cave.here
        cave.raw('c4e1fa2cc0')  # vcvttss2si rax, xmm0
        cave.raw('50')          # push rax (aligned for the call)
        cave.raw('4489ee')      # mov esi, r13d
        cave.raw(level_fix)
        # The browser call with esi kept (quit_menu.Code.browser clears it).
        cave.messages.append((len(cave.data), 1))
        cave.raw('488d3d00000000')  # lea rdi, [rip + "bb:f"]
        cave.call(BROWSER)
        cave.raw('89c1')        # mov ecx, eax
        cave.raw('58')          # pop rax
        cave.raw('85c9 7402 31c0'.replace(' ', ''))  # test ecx, ecx; jz +2; xor eax, eax
        cave.raw(tail)
        cave.raw('c3')
        return at
    raise_price = price('83ee51', '8b8bf8010000')      # sub esi, 0x51 | mov ecx, [rbx+0x1f8]
    lower_price = price('83ee51', '2983f4010000')      # sub esi, 0x51 | sub [rbx+0x1f4], eax
    strings = cave.here
    for name in 'bf':
        cave.data += f'bb:{name}'.encode() + b'\0'
    for at, message in cave.messages:
        cave.data[at + 3:at + 7] = quit_menu.rel32(CAVE + at, 7, strings + 5 * message)
    if cave.here > quit_menu.CAVE_END:
        raise ValueError('rebirth code does not fit')

    flag = quit_menu.Code(SET_FLAG)
    flag.jmp(hook)
    flag.raw('909090')
    writes = [(cave.offset, bytes(cave.data)), (flag.offset, bytes(flag.data))]
    for site, target in ((RAISE, raise_price), (LOWER, lower_price)):
        call = quit_menu.Code(site)
        call.call(target)
        call.raw('90' * 6)
        writes.append((call.offset, bytes(call.data)))
    return writes


def check_original(image_bytes):
    for offset, expected in ORIGINAL.items():
        expected = bytes.fromhex(expected.replace(' ', ''))
        if image_bytes(offset, offset + len(expected)) != expected:
            raise ValueError(f'Rebirth: unexpected code at {offset + quit_menu.EBOOT_BASE:#x}')


# ---- The Doll's talk script -------------------------------------------------------------------

def call_equals(function, args, value):
    """Evaluator: f<function>(args) == value."""
    code = esd.push_int(function) + b''.join(esd.push_int(a) for a in args)
    return code + bytes([0x84 + len(args), 0xA6]) + esd.push_int(value) + b'\x95\xa1'


def command(bank, id, *args):
    return esd.Command(bank, id, [esd.int_arg(a) for a in args])


def doll_menus(script):
    """(group, menu state, choice state) of every talk list with Channel Blood Echoes: the state
    adding the list, and the one reading the player's choice into reg0 (f23) after it."""
    found = []
    for _gid, group in script.groups:
        for menu in group:
            if not any((c.bank, c.id) == (1, 19) and esd.disassemble(c.args[1]) == str(LEVEL_UP_TEXT)
                       for c in menu.entry):
                continue
            # The choice: a few unconditional steps on from the list.
            seen, frontier, choice = set(), [menu], None
            for _depth in range(4):
                following = []
                for st in frontier:
                    for c in st.conditions:
                        if c.target is not None and id(c.target) not in seen:
                            seen.add(id(c.target))
                            following.append(c.target)
                for st in following:
                    if any('= f23()' in esd.disassemble(c.evaluator) for c in st.conditions):
                        choice = st
                        break
                if choice or not following:
                    break
                frontier = following
            if choice:
                found.append((group, menu, choice))
    return found


def doll_script(data):
    """A Hunter's Dream talk script with Rebirth in each of the Doll's lists (data unchanged when
    it has none, or has it already)."""
    script = esd.read(data)
    menus = doll_menus(script)
    if not menus or any(esd.disassemble(c.args[1]) == str(OPTION_TEXT)
                        for _g, menu, _c in menus for c in menu.entry if (c.bank, c.id) == (1, 19)):
        return data
    for group, menu, choice in menus:
        used = {esd.disassemble(c.args[0]) for c in menu.entry if (c.bank, c.id) == (1, 19)}
        if str(OPTION) in used:
            raise ValueError(f'talk list entry {OPTION} already used')
        next_id = max(st.id for st in group) + 1
        ask, reborn, level_up, short, nothing = (esd.State(next_id + i) for i in range(5))
        # Rebirth right after Channel Blood Echoes.
        at = next(i for i, c in enumerate(menu.entry)
                  if (c.bank, c.id) == (1, 19) and esd.disassemble(c.args[1]) == str(LEVEL_UP_TEXT))
        menu.entry.insert(at + 1, command(1, 19, OPTION, OPTION_TEXT, -1))
        # reg0 == OPTION, right after the condition that reads the choice into reg0.
        read = next(i for i, c in enumerate(choice.conditions) if '= f23()' in esd.disassemble(c.evaluator))
        choice.conditions.insert(read + 1, esd.Condition(ask, b'\xaf' + esd.push_int(OPTION) + b'\x95\xa1'))
        # Yes/No (as m21_01's "Return to the Hunter's Dream?"): f22 is the button, f21 the dialog.
        ask.entry = [command(1, 17, 2, QUESTION_TEXT, 3, 4, 2)]
        ask.conditions = [esd.Condition(reborn, call_equals(22, [], 1)), esd.Condition(menu, call_equals(21, [], 0))]
        # The rebirth: the port answers through RESULT.
        reborn.entry = [command(1, 11, REQUEST, 1)]
        reborn.conditions = [esd.Condition(level_up, call_equals(15, [RESULT], 1)),
                             esd.Condition(nothing, call_equals(15, [NOTHING], 1)),
                             esd.Condition(short, b'\x41\xa1')]
        # The level-up screen, as Channel Blood Echoes opens it (c1_31, open while f25(23) != 0).
        level_up.entry = [command(1, 11, RESULT, 0), esd.Command(1, 31, [])]
        level_up.conditions = [esd.Condition(menu, call_equals(25, [23], 0))]
        short.entry = [command(1, 17, 1, SHORT_TEXT, 1, 0, 1)]
        short.conditions = [esd.Condition(menu, call_equals(21, [], 0))]
        nothing.entry = [command(1, 11, NOTHING, 0), command(1, 17, 1, NOTHING_TEXT, 1, 0, 1)]
        nothing.conditions = [esd.Condition(menu, call_equals(21, [], 0))]
        group.extend([ask, reborn, level_up, short, nothing])
    return esd.write(script)


def write_layer(game, out, source=None):
    dvd = Path(game) / 'dvdroot_ps4'
    talk = dvd / 'script' / 'talk' / DOLL_SCRIPTS
    archive = talk.read_bytes()
    bnd = quit_menu.unpack_dcx(archive)
    edited = {}
    for _e, o, n, file_id in quit_menu.bnd4_files(bnd):
        script = doll_script(bnd[o:o + n])
        if script != bnd[o:o + n]:
            edited[file_id] = script
    if not edited:
        raise ValueError("no Doll menu found in the Hunter's Dream scripts")
    target = Path(out) / 'dvdroot_ps4' / 'script' / 'talk' / DOLL_SCRIPTS
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(quit_menu.pack_dcx(archive, quit_menu.rebuild_bnd4(bnd, edited)))
    print(f'Rebirth: Doll menus in {len(edited)} talk scripts')
    written = 0
    for folder in sorted(p for p in (dvd / 'msg').iterdir() if p.is_dir()):
        name = folder.name
        layered = Path(source) / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx' if source else None
        path = layered if layered and layered.is_file() else folder / 'menu.msgbnd.dcx'
        if not path.is_file():
            continue
        option, question, short, nothing = TEXTS.get(name.casefold(), TEXTS['engus'])
        archive = path.read_bytes()
        bnd = quit_menu.unpack_dcx(archive)
        texts = {file_id: bnd[o:o + n] for _e, o, n, file_id in quit_menu.bnd4_files(bnd)}
        strings = quit_menu.read_fmg(texts[TALK_FMG])
        strings.update({OPTION_TEXT: option, QUESTION_TEXT: question, SHORT_TEXT: short, NOTHING_TEXT: nothing})
        replaced = quit_menu.rebuild_bnd4(bnd, {TALK_FMG: quit_menu.write_fmg(texts[TALK_FMG], strings)})
        target = Path(out) / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(quit_menu.pack_dcx(archive, replaced))
        written += 1
    print(f'Rebirth: the Doll\'s menu and texts for {written} languages')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='game folder (with dvdroot_ps4)')
    p.add_argument('--out', type=Path, required=True, help='mod layer folder to write')
    p.add_argument('--source', type=Path, help='a layer whose menu texts to build on')
    a = p.parse_args()
    write_layer(a.game, a.out, a.source)


if __name__ == '__main__':
    main()
