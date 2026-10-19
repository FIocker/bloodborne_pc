#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""FromSoftware ESD state machines (talk scripts, *.talkesdbnd.dcx), Bloodborne's 64-bit
("fsSL") layout: read into objects, edited, written back in the game's own order.

A file holds state groups; a state has entry, exit and while commands and conditions; a
condition has an evaluator (bytecode), commands run when it passes, a target state and
sub-conditions. A command is (bank, id, [argument bytecode]). Offsets in the file are relative
to the data start (0x6C) and are 64-bit. write(read(data)) == data for the game's files (checked
by tests/test_rebirth.py); edits keep that layout, which is how the game's loader expects it.

Expression bytecode (enough to build arguments and evaluators):
  0x00-0x7F  push integer value-0x40          0x82 push int32      0x80 push float32
  0x84-0x8A  call function (popped id) with 0..6 args                0xA1 end
  0x8C-0x95  + - * / <= >= < > == != on the stack ...
"""
import struct

HEADER = 0x6C


class Command:
    def __init__(self, bank, id, args):
        self.bank, self.id, self.args = bank, id, list(args)  # args: [bytes]

    def __repr__(self):
        return f'Command({self.bank}, {self.id}, {[a.hex() for a in self.args]})'


class Condition:
    def __init__(self, target, evaluator, commands=(), subconditions=()):
        self.target = target  # State or None
        self.evaluator = evaluator  # bytes
        self.commands = list(commands)
        self.subconditions = list(subconditions)


class State:
    def __init__(self, id):
        self.id = id
        self.conditions, self.entry, self.exit, self.during = [], [], [], []


class Esd:
    pass


def _q(data, at):
    return struct.unpack_from('<q', data, at)[0]


def read(data):
    if data[:4] != b'fsSL':
        raise ValueError('not a 64-bit ESD (fsSL)')
    h = struct.unpack_from('<27i', data, 0)
    esd = Esd()
    esd.header = data[:HEADER]
    base = HEADER
    esd.data_header = data[base:base + 0x18]  # 1, four unknown values, 0
    table, group_count = _q(data, base + 0x18), _q(data, base + 0x20)
    name_offset, name_length = _q(data, base + 0x28), _q(data, base + 0x30)
    esd.name = data[base + name_offset:base + name_offset + name_length * 2]
    states_by_offset, conditions_by_offset = {}, {}

    def state_at(offset):
        if offset not in states_by_offset:
            states_by_offset[offset] = State(_q(data, base + offset))
        return states_by_offset[offset]

    def commands(offset, count):
        out = []
        for i in range(count):
            at = base + offset + i * 0x18
            bank, id = struct.unpack_from('<ii', data, at)
            args_offset, args_count = _q(data, at + 8), _q(data, at + 16)
            args = []
            for k in range(args_count):
                a = base + args_offset + k * 0x10
                expr, length = _q(data, a), _q(data, a + 8)
                args.append(bytes(data[base + expr:base + expr + length]))
            out.append(Command(bank, id, args))
        return out

    def condition_list(offset, count):
        out = []
        for i in range(count):
            out.append(condition_at(_q(data, base + offset + i * 8)))
        return out

    def condition_at(offset):
        if offset in conditions_by_offset:
            return conditions_by_offset[offset]
        at = base + offset
        target = _q(data, at)
        c = Condition(None, b'')
        conditions_by_offset[offset] = c
        c.target = state_at(target) if target != -1 else None
        c.commands = commands(_q(data, at + 8), _q(data, at + 16))
        c.subconditions = condition_list(_q(data, at + 24), _q(data, at + 32))
        e, n = _q(data, at + 40), _q(data, at + 48)
        c.evaluator = bytes(data[base + e:base + e + n])
        return c

    esd.groups = []
    for g in range(group_count):
        at = base + table + g * 0x20
        group_id, offset, count = _q(data, at), _q(data, at + 8), _q(data, at + 16)
        states = []
        for s in range(count):
            so = offset + s * 0x48
            st = state_at(so)
            a = base + so
            st.conditions = condition_list(_q(data, a + 8), _q(data, a + 16))
            st.entry = commands(_q(data, a + 24), _q(data, a + 32))
            st.exit = commands(_q(data, a + 40), _q(data, a + 48))
            st.during = commands(_q(data, a + 56), _q(data, a + 64))
            states.append(st)
        esd.groups.append((group_id, states))
    esd.counts = h[10:19]
    return esd


def write(esd):
    """The file, in the game's order: groups, states, conditions, commands, arguments, condition
    lists, bytecode, name."""
    states = [s for _g, ss in esd.groups for s in ss]
    state_offset = {}
    copies = []  # (offset, state): after each group, a copy of its first state, as the game writes
    at = 0x48 + len(esd.groups) * 0x20
    for _g, ss in esd.groups:
        for s in ss:
            state_offset[id(s)] = at
            at += 0x48
        if ss:
            copies.append((at, ss[0]))
            at += 0x48
    # Conditions in the order the game's files list them: the states' own, then sub-conditions
    # as they are found (each once).
    conditions, seen = [], set()

    def add(c):
        if id(c) not in seen:
            seen.add(id(c))
            conditions.append(c)
    for s in states:
        for c in s.conditions:
            add(c)
    i = 0
    while i < len(conditions):
        for sub in conditions[i].subconditions:
            add(sub)
        i += 1
    condition_offset = {}
    for c in conditions:
        condition_offset[id(c)] = at
        at += 0x38
    # Commands: per state entry, exit, while; then per condition.
    command_lists = []
    for s in states:
        command_lists += [s.entry, s.exit, s.during]
    for c in conditions:
        command_lists.append(c.commands)
    list_offset = {}
    for cl in command_lists:
        list_offset[id(cl)] = at if cl else -1
        at += 0x18 * len(cl)
    arg_offset = {}
    for cl in command_lists:
        for cmd in cl:
            arg_offset[id(cmd)] = at if cmd.args else -1
            at += 0x10 * len(cmd.args)
    # Condition lists: each group's states', then an unused copy of its first state's (the copied
    # state points at the original list), then the sub-conditions'.
    cond_lists, list_copies = [], []
    condition_offsets_start = at
    cond_list_offset = {}
    for _g, ss in esd.groups:
        for st in ss:
            cond_lists.append(st.conditions)
            cond_list_offset[id(st.conditions)] = at if st.conditions else -1
            at += 8 * len(st.conditions)
        if ss and ss[0].conditions:
            list_copies.append((at, ss[0].conditions))
            at += 8 * len(ss[0].conditions)
    for c in conditions:
        cond_lists.append(c.subconditions)
        cond_list_offset[id(c.subconditions)] = at if c.subconditions else -1
        at += 8 * len(c.subconditions)
    condition_offsets_count = (at - condition_offsets_start) // 8
    # Bytecode, in the game's order: the states' own conditions' evaluators; then state by state
    # the evaluators of sub-conditions not written yet and the state's command arguments; then
    # the arguments of the commands run when a condition passes.
    blob = bytearray()
    expr_offset = {}
    bytecode_start = at

    def evaluator(c):
        if id(c) not in expr_offset:
            expr_offset[id(c)] = bytecode_start + len(blob)
            blob.extend(c.evaluator)

    def arguments(commands):
        for cmd in commands:
            for i, a in enumerate(cmd.args):
                expr_offset[(id(cmd), i)] = bytecode_start + len(blob)
                blob.extend(a)

    def subconditions(c):
        for sub in c.subconditions:
            evaluator(sub)
            subconditions(sub)
    for st in states:
        for c in st.conditions:
            evaluator(c)
    for st in states:
        for c in st.conditions:
            subconditions(c)
        arguments(st.entry)
        arguments(st.exit)
        arguments(st.during)
    for c in conditions:
        evaluator(c)  # unreachable from a state: none in the game's files
        arguments(c.commands)
    at = bytecode_end = bytecode_start + len(blob)
    at += at % 2  # the name is UTF-16: an even offset
    name_offset = at
    at += len(esd.name)
    data_size = at

    out = bytearray(HEADER + data_size)
    out[:HEADER] = esd.header
    arg_count = sum(len(cmd.args) for cl in command_lists for cmd in cl)
    struct.pack_into('<i', out, 0x14, data_size)
    struct.pack_into('<9i', out, 0x28, len(esd.groups), 0x48, len(states), 0x38, len(conditions),
                     0x18, sum(len(cl) for cl in command_lists), 0x10, arg_count)
    struct.pack_into('<i', out, 0x30, len(states) + len(copies))
    # The header has the bytecode's end where the data header has the (aligned) name.
    struct.pack_into('<8i', out, 0x4C, condition_offsets_start, condition_offsets_count,
                     bytecode_end, len(esd.name) // 2, data_size, 0, data_size, 0)
    base = HEADER
    out[base:base + 0x18] = esd.data_header
    struct.pack_into('<6q', out, base + 0x18, 0x48, len(esd.groups), name_offset, len(esd.name) // 2, -1, -1)
    at = base + 0x48
    first = 0x48 + len(esd.groups) * 0x20
    for group_id, ss in esd.groups:
        o = state_offset[id(ss[0])] if ss else first
        struct.pack_into('<4q', out, at, group_id, o, len(ss), o)
        at += 0x20

    def lst(l, table):
        return table[id(l)], len(l)

    for a, s in [(state_offset[id(s)], s) for s in states] + copies:
        struct.pack_into('<9q', out, base + a, s.id, *lst(s.conditions, cond_list_offset), *lst(s.entry, list_offset),
                         *lst(s.exit, list_offset), *lst(s.during, list_offset))
    for c in conditions:
        a = base + condition_offset[id(c)]
        target = state_offset[id(c.target)] if c.target is not None else -1
        struct.pack_into('<7q', out, a, target, *lst(c.commands, list_offset), *lst(c.subconditions, cond_list_offset),
                         expr_offset[id(c)], len(c.evaluator))
    for cl in command_lists:
        for k, cmd in enumerate(cl):
            a = base + list_offset[id(cl)] + k * 0x18
            struct.pack_into('<iiqq', out, a, cmd.bank, cmd.id, arg_offset[id(cmd)], len(cmd.args))
            for i, arg in enumerate(cmd.args):
                struct.pack_into('<qq', out, base + arg_offset[id(cmd)] + i * 0x10, expr_offset[(id(cmd), i)], len(arg))
    for cl in cond_lists:
        for k, c in enumerate(cl):
            struct.pack_into('<q', out, base + cond_list_offset[id(cl)] + k * 8, condition_offset[id(c)])
    for a, cl in list_copies:
        for k, c in enumerate(cl):
            struct.pack_into('<q', out, base + a + k * 8, condition_offset[id(c)])
    out[base + bytecode_start:base + bytecode_start + len(blob)] = blob
    out[base + name_offset:base + name_offset + len(esd.name)] = esd.name
    return bytes(out)


# ---- Bytecode helpers --------------------------------------------------------------------------

def push_int(value):
    if -64 <= value <= 63:
        return bytes([value + 0x40])
    return b'\x82' + struct.pack('<i', value)


def int_arg(value):
    """A command argument: the integer and the end mark."""
    return push_int(value) + b'\xa1'


def disassemble(code):
    """Readable bytecode: numbers, f<id>(args) calls, operators."""
    stack, i = [], 0
    ops = {0x8C: '+', 0x8D: 'neg', 0x8E: '-', 0x8F: '*', 0x90: '/', 0x91: '<=', 0x92: '>=', 0x93: '<',
           0x94: '>', 0x95: '==', 0x96: '!=', 0x98: '&&', 0x99: '||', 0x9A: '!'}
    while i < len(code):
        b = code[i]
        i += 1
        if b <= 0x7F:
            stack.append(str(b - 0x40))
        elif b == 0x80:
            stack.append(f'{struct.unpack_from("<f", code, i)[0]:g}'); i += 4
        elif b == 0x81:
            stack.append(f'{struct.unpack_from("<d", code, i)[0]:g}'); i += 8
        elif b == 0x82:
            stack.append(str(struct.unpack_from('<i', code, i)[0])); i += 4
        elif 0x84 <= b <= 0x8A:
            n = b - 0x84
            args = stack[len(stack) - n:] if n else []
            del stack[len(stack) - n:]
            fn = stack.pop() if stack else '?'
            stack.append(f'f{fn}({", ".join(args)})')
        elif b in ops:
            if ops[b] in ('neg', '!'):
                stack.append(f'{ops[b]}({stack.pop()})')
            else:
                r, l = stack.pop(), stack.pop()
                stack.append(f'({l} {ops[b]} {r})')
        elif b == 0xA1:
            break
        elif b == 0xA5:  # UTF-16 string literal
            end = code.index(b'\0\0', i)
            end += (end - i) % 2
            stack.append(repr(code[i:end].decode('utf-16-le', errors='replace'))); i = end + 2
        elif b == 0xA6:
            pass  # follows a call whose value is used next
        elif 0xA7 <= b <= 0xAE:
            stack.append(f'reg{b - 0xA7} = {stack.pop()}')
        elif 0xAF <= b <= 0xB6:
            stack.append(f'reg{b - 0xAF}')
        elif b == 0xB7:
            stack.append(f'abort_if_false({stack.pop()})')
        elif b == 0xB9:
            stack.append('call_result')
        else:
            stack.append(f'<{b:#x}>')
    return ' ; '.join(stack)
