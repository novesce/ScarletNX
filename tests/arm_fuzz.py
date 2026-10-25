#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Random-encoding differential of the A32/Thumb LLVM backend against Unicorn 2.1.4.

The catalogues of arm_test.py, arm_vector_test.py and thumb_test.py pick their encodings by hand. This draws
them at random from each encoding class, keeps the ones llvm-mc disassembles for the Cortex-A57 (Unicorn has
no A57 model: its "max" CPU takes v8.2 encodings too, and llvm-mc drops them and the UNPREDICTABLE ones it flags),
and runs each from random state: GPRs as addresses or edge values, NZCV/Q/GE, FPSCR's RMode/FZ/DN/AHP and
cumulative bits, D registers full of NaNs (quiet and signalling, with payloads), infinities, zeros and
denormals. Exclusive pairs (LDREX* then STREX* of the same size) start from a cleared monitor in both.

    python3 tests/arm_fuzz.py --arm-emitter build/tests/snx-test-arm-emit \\
        --thumb-emitter build/tests/snx-test-thumb-emit --runtime .../libsnx_arm_rt.a --mc llvm-mc

Disagreements are grouped by mnemonic: wrong state (both executed), missing (ours undefined, Unicorn ran it),
extra (ours ran, Unicorn raised), and different faults. Unicorn is QEMU's translator, not the hardware: a
disagreement is checked against the Arm ARM before either side is called wrong. Many missing, extra and fault
ones are UNPREDICTABLE encodings llvm-objdump still decodes (RdLo == RdHi, writeback to the loaded register, the PC
where it isn't allowed, register lists past d31, should-be-zero bits set), where either answer is the Arm ARM's,
and QEMU 5 checks no alignment for LDRD, LDM, VLDM or VLDn, which the A57 faults on: the run fails only on wrong
state, and reports the rest. No game assets.
"""

import argparse
import collections
import ctypes as C
import pathlib
import random
import re
import subprocess
import sys
import tempfile

from arm_test import CPU, PC, SIZE, unicorn_dies

EXITS = ('ok', 'undefined', 'thumb16-unsupported', 'thumb32-unsupported', 'svc', 'bkpt', 'data-abort',
         'alignment', 'bad-entry', 'endian', 'unsupported', 'prefetch-abort', 'code-changed', 'it-entry')
UNDEF = {1, 2, 3, 10}

# (isa, [(mask, value)]): the bits under mask are value, the others random. A32 words take cond AL three times
# in four.
CLASSES = {
    'a32-dp': ('arm', [(0x0C000000, 0x00000000)]),
    'a32-mul': ('arm', [(0x0F0000F0, 0x00000090), (0x0F8000F0, 0x01000000), (0x0F900090, 0x01000080)]),
    'a32-ldst': ('arm', [(0x0E000000, 0x04000000), (0x0E000010, 0x06000000), (0x0E000090, 0x00000090)]),
    'a32-media': ('arm', [(0x0E000010, 0x06000010), (0x0FF000F0, 0x07000010), (0x0F8000F0, 0x07800050)]),
    'a32-ldm': ('arm', [(0x0E000000, 0x08000000)]),
    'a32-misc': ('arm', [(0x0F900000, 0x01000000), (0x0FB00000, 0x03200000), (0xFF000000, 0xF5000000),
                         (0xFE000000, 0xF6000000)]),
    'a32-vfp': ('arm', [(0x0F000E10, 0x0E000A00), (0x0F000E10, 0x0E000A10), (0x0E000E00, 0x0C000A00),
                        (0xFF000E10, 0xFE000A00)]),
    'a32-neon': ('arm', [(0xFE000000, 0xF2000000)]),
    'a32-neon-fp': ('arm', [(0xFE800F10, 0xF2000D00), (0xFE800F10, 0xF2000F00), (0xFE800E10, 0xF2800900),
                            (0xFFB30600, 0xF3B30400), (0xFFB30E00, 0xF3B30600), (0xFFB30F00, 0xF3B30500),
                            (0xFE800E90, 0xF2800E10), (0xFFB00000, 0xF3B00000)]),
    'a32-neon-ls': ('arm', [(0xFF100000, 0xF4000000)]),
    't16': ('thumb16', [(0x0000, 0x0000)]),
    't32': ('thumb32', [(0x00000000, 0x00000000)]),
    't32-vfp': ('thumb32', [(0xEF000E10, 0xEE000A00), (0xEF000E10, 0xEE000A10), (0xEE000E00, 0xEC000A00),
                            (0xFF000E10, 0xFE000A00)]),
    't32-neon': ('thumb32', [(0xEF000000, 0xEF000000), (0xFF100000, 0xF9000000)]),
}
EXCLUSIVE = ('a32-exclusive', 't32-exclusive', 'a32-monitor', 't-it')

F32 = (0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001,
       0x7F800001, 0xFFA00000, 0x00000001, 0x807FFFFF, 0x00800000, 0x7F7FFFFF, 0x4F000000, 0xCF000000,
       0x3F000000, 0x3FC00000, 0x40490FDB, 0x4B800000, 0x34000000, 0x2F800000)
F64 = (0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
       0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000123, 0x7FF0000000000001, 0xFFF4000000000000,
       0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0x41E0000000000000,
       0xC1E0000000000000, 0x3FE0000000000000, 0x3FF8000000000000, 0x43E0000000000000, 0x47EFFFFFE0000000,
       0x36A0000000000000, 0x3810000000000000, 0x41DFFFFFFFC00000)
F16 = (0x0000, 0x8000, 0x3C00, 0x7C00, 0xFC00, 0x7E00, 0x7C01, 0xFE01, 0x0001, 0x03FF, 0x0400, 0x7BFF)
INT = (0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678)


def draw(rng, mask, value, bits):
    return (rng.getrandbits(bits) & ~mask | value) & ((1 << bits) - 1)


def generate(rng, name, count):
    """Encodings of one class: A32 words, a Thumb16 halfword, or a Thumb32 word as hw1 << 16 | hw2."""
    isa, templates = CLASSES[name]
    out = []
    while len(out) < count:
        mask, value = rng.choice(templates)
        if isa == 'arm':
            word = draw(rng, mask, value, 32)
            if word >> 28 != 15 and rng.random() < 0.75:
                word = word & 0x0FFFFFFF | 0xE0000000
            out.append([word])
        elif isa == 'thumb16':
            half = rng.getrandbits(16)
            if half >> 11 < 0b11101:
                out.append([half])
        else:
            word = draw(rng, mask, value, 32)
            if word >> 27 >= 0b11101:
                out.append([word >> 16, word & 0xFFFF])
    return out


def exclusive_pairs(rng, isa, count):
    """LDREX*/LDAEX* then STREX*/STLEX* of the same size on the same base, as A32 words or Thumb halfwords."""
    out = []
    while len(out) < count:
        size = rng.choice(('word', 'double', 'byte', 'half'))
        acquire = rng.randrange(2)
        n = rng.choice((1, 2, 3, 12))
        t, t2, s, u, u2 = rng.sample([r for r in range(13) if r != n], 5)
        if isa == 'arm':
            # cccc 0001 1ss1 nnnn tttt 11a0 1001 1111, the store's status register in tttt and its Rt in the low bits
            t, u = t & ~1, u & ~1
            if s in (u, u + 1):
                continue
            ss = {'word': 0, 'double': 1, 'byte': 2, 'half': 3}[size]
            kind = 0xE90 if acquire else 0xF90
            out.append([0xE1900000 | ss << 21 | n << 16 | t << 12 | kind | 0xF,
                        0xE1800000 | ss << 21 | n << 16 | s << 12 | kind | u])
            continue
        if s in (u, u2):
            continue
        if size == 'word' and not acquire:  # T1, with an offset of imm8 words
            imm = rng.choice((0, 0, 1, 2, rng.randrange(256)))
            out.append([0xE850 | n, t << 12 | 0x0F00 | imm, 0xE840 | n, u << 12 | s << 8 | imm])
            continue
        # E8Dn tttt TTTT 01ss 1111 (11ss for acquire), TTTT the doubleword's second register or 1111
        op = {'byte': 0, 'half': 1, 'word': 2, 'double': 3}[size] | (0xC if acquire else 0x4)
        second, second_u = (t2, u2) if size == 'double' else (15, 15)
        out.append([0xE8D0 | n, t << 12 | second << 8 | op << 4 | 0xF, 0xE8C0 | n, u << 12 | second_u << 8 | op << 4 | s])
    return out


def monitor_sequences(rng, count):
    """A32 LDREX*, something that may change what the monitor holds, then STREX* on the same base."""
    out = []
    while len(out) < count:
        n, n2 = rng.sample((1, 2, 3, 12), 2)
        ss = rng.randrange(4)  # word, doubleword, byte, halfword
        store_ss = ss if rng.random() < 0.8 else rng.choice([x for x in range(4) if x != ss])
        load = 0xE1900F9F | ss << 21 | n << 16 | 4 << 12
        store = 0xE1800F90 | store_ss << 21 | n << 16 | 8 << 12 | 6
        middle = rng.choice((
            [],
            [0xF57FF01F],  # CLREX
            [0xE5800000 | n << 16 | 4 << 12],  # STR r4, [n]: the value it loaded
            [0xE5800000 | n << 16 | 10 << 12],  # STR r10, [n]: another
            [0xE5C00000 | n << 16 | 10 << 12],  # STRB r10, [n]
            [0xE1900F9F | n2 << 16 | 11 << 12],  # LDREX r11, [n2]
            [0xE1D00F9F | n << 16 | 11 << 12],  # LDREXB r11, [n]
            [0xF57FF05B],  # DMB ISH
            [0xE2800001 | 10 << 16 | 10 << 12],  # ADD r10, r10, #1
        ))
        out.append([load] + middle + [store])
    return out


def implementation_defined(name, item):
    """A monitor sequence whose STREX may pass or fail: a size other than the last LDREX's (CONSTRAINED
    UNPREDICTABLE), or an ordinary store between them, which the backend lets clear the monitor (writemem in llvm.c)."""
    if name != 'a32-monitor':
        return False
    last = [w for w in item[:-1] if w & 0x0F900FFF == 0x01900F9F][-1]
    return (last >> 21 & 3) != (item[-1] >> 21 & 3) or any(w & 0x0E100000 == 0x04000000 for w in item[1:-1])


SLOT = 32


def disassemble(tools, isa, items):
    """Each item's text, or None for an encoding the Cortex-A57 doesn't have. llvm-mc's disassembler resynchronizes
    a byte at a time after an invalid Thumb32 encoding, so the items are assembled as .inst directives into slots of
    their own and disassembled by llvm-objdump, which steps over an unknown one by its size."""
    mc, objdump = tools
    triple = 'armv8a-linux-gnueabi' if isa == 'arm' else 'thumbv8a-linux-gnueabi'
    source = ['.syntax unified', '.arm' if isa == 'arm' else '.thumb']
    for item in items:
        source.append(f'.balign {SLOT}')
        i = 0
        while i < len(item):
            if isa == 'arm':
                source.append(f'.inst 0x{item[i]:08x}')
                i += 1
            elif item[i] >> 11 >= 0b11101:
                source.append(f'.inst.w 0x{item[i]:04x}{item[i + 1]:04x}')
                i += 2
            else:
                source.append(f'.inst.n 0x{item[i]:04x}')
                i += 1
    with tempfile.TemporaryDirectory(prefix='snx-arm-fuzz-mc-') as path:
        asm, obj = pathlib.Path(path) / 'items.s', pathlib.Path(path) / 'items.o'
        asm.write_text('\n'.join(source) + '\n')
        subprocess.run([mc, f'-triple={triple}', '-mcpu=cortex-a57', '-filetype=obj', str(asm), '-o', str(obj)],
                       check=True)
        listing = subprocess.run([objdump, '-d', '--mcpu=cortex-a57', '--no-show-raw-insn', str(obj)],
                                 check=True, text=True, capture_output=True).stdout
    found = collections.defaultdict(list)
    for line in listing.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s*(.*)$', line)
        if m:
            address = int(m.group(1), 16)
            found[address // SLOT].append((address % SLOT, m.group(2).strip().split('@')[0].strip()))
    out = []
    width = 4 if isa == 'arm' else 2
    for index, item in enumerate(items):
        texts = [t for offset, t in found[index] if offset < width * len(item)]
        out.append(None if not texts or any(t == '<unknown>' for t in texts) else '; '.join(texts))
    return out


def soft_fail(mc, isa, item):
    """llvm-mc's warning for an encoding it decodes but the Arm ARM calls UNPREDICTABLE (SoftFail)."""
    data = b''.join(w.to_bytes(4 if isa == 'arm' else 2, 'little') for w in item)
    triple = 'armv8a-linux-gnueabi' if isa == 'arm' else 'thumbv8a-linux-gnueabi'
    proc = subprocess.run([mc, '--disassemble', f'-triple={triple}', '-mcpu=cortex-a57'],
                          input=' '.join(f'0x{b:02x}' for b in data) + '\n', text=True, capture_output=True)
    return 'potentially undefined' in proc.stderr


CONDITIONS = '(eq|ne|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?'
BRANCHY = re.compile(r'^(b|bl|blx|bx|cbn?z|tb[bh]|svc|bkpt|udf|it[te]*|hvc|smc|setend|eret|sev|wfe|wfi|yield)'
                     + CONDITIONS + r'(\.[nw])?$')


def it_blocks(rng, tools, count):
    """An IT instruction and as many random Thumb instructions as its mask covers, none of them branches."""
    out = []
    while len(out) < count:
        firstcond = rng.randrange(14)
        mask = rng.randrange(1, 16)
        n = 5 - (mask & -mask).bit_length()
        body = generate(rng, 't16', n * 3) + generate(rng, 't32', n * 3)
        rng.shuffle(body)
        texts = disassemble(tools, 'thumb', body)
        picked = [b for b, t in zip(body, texts) if t and not BRANCHY.match(t.split()[0]) and 'pc' not in t]
        if len(picked) >= n:
            out.append([0xBF00 | firstcond << 4 | mask] + [h for b in picked[:n] for h in b])
    return out


def special(rng):
    kind = rng.randrange(6)
    if kind == 0:
        return rng.choice(F32) | rng.choice(F32) << 32
    if kind == 1:
        return rng.choice(F64)
    if kind == 2:
        return sum(rng.choice(F16) << 16 * i for i in range(4))
    if kind == 3:
        return rng.choice(INT) | rng.choice(INT) << 32
    if kind == 4:
        return rng.choice((0x01, 0x7F, 0x80, 0xFF, 0x08, 0x10, 0x1F, 0x20, 0xE0)) * 0x0101010101010101
    return rng.getrandbits(64)


def initial(rng, isa, item):
    thumb = isa != 'arm'
    cpu = CPU()
    mem = (C.c_uint8 * SIZE).from_buffer_copy(rng.randbytes(SIZE))
    cpu.mem = mem
    cpu.mem_size = SIZE
    cpu.cpsr = rng.getrandbits(4) << 28 | rng.getrandbits(1) << 27 | rng.getrandbits(4) << 16 | thumb << 5 | 0x10
    cpu.fpscr = rng.getrandbits(4) << 28 | rng.getrandbits(1) << 27 | rng.getrandbits(3) << 24
    cpu.fpscr |= rng.getrandbits(2) << 22 | rng.choice((0, 0, 0x9F, rng.getrandbits(8) & 0x9F))
    addresses = rng.random() < 0.6
    for i in range(15):
        if addresses and rng.random() < 0.85:
            cpu.r[i] = 0x2000 + rng.randrange(0xC000) & ~rng.choice((0, 3, 7, 15))
        else:
            cpu.r[i] = rng.choice(INT) if rng.random() < 0.4 else rng.getrandbits(32)
    cpu.r[15] = PC
    for i in range(32):
        cpu.d[i] = special(rng)
    width = 2 if thumb else 4
    mem[PC - 4 : PC] = bytes((0xBF, 0xF3, 0x2F, 0x8F)) if thumb else (0xF57FF01F).to_bytes(4, 'little')  # CLREX
    for i, w in enumerate(item):
        mem[PC + width * i : PC + width * (i + 1)] = w.to_bytes(width, 'little')
    return cpu, mem


class Unicorn:
    def __init__(self, thumb):
        import unicorn as U
        from unicorn import arm_const as A

        self.U, self.A, self.thumb = U, A, thumb
        self.uc = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_THUMB if thumb else U.UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
        self.uc.mem_map(0, SIZE)
        self.regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)]
        self.regs += [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]

    def run(self, cpu, mem, count, end):
        """Run the CLREX before the code, then count instructions or up to end (Unicorn doesn't count an IT).
        Returns Unicorn's error or None."""
        A, uc = self.A, self.uc
        uc.mem_write(0, bytes(mem))
        uc.ctl_remove_cache(0, SIZE)
        uc.reg_write(A.UC_ARM_REG_CPSR, cpu.cpsr)
        uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        uc.reg_write(A.UC_ARM_REG_FPSCR, cpu.fpscr)
        for r, value in zip(self.regs, cpu.r):
            uc.reg_write(r, value)
        uc.reg_write(A.UC_ARM_REG_PC, (PC - 4) | self.thumb)
        for i, value in enumerate(cpu.d):
            uc.reg_write(A.UC_ARM_REG_D0 + i, value)
        try:
            uc.emu_start((PC - 4) | self.thumb, end, count=count + 1)
        except self.U.UcError as exc:
            return str(exc)
        return None

    def state(self):
        A, uc = self.A, self.uc
        values = [uc.reg_read(r) for r in self.regs] + [uc.reg_read(A.UC_ARM_REG_CPSR)]
        values += [uc.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)] + [uc.reg_read(A.UC_ARM_REG_FPSCR)]
        return values


FIELDS = [f'r{i}' for i in range(16)] + ['cpsr'] + [f'd{i}' for i in range(32)] + ['fpscr']


def fault_kind(error):
    if error is None or 'FETCH_UNMAPPED' in error:
        return 'ok'  # an indirect branch completed, to a random target
    if 'UNMAPPED' in error or 'PROT' in error:
        return 'abort'
    return 'exception'


def agree(ours, theirs):
    if ours == 0:
        return theirs == 'ok'
    if ours in UNDEF or ours in (4, 5):
        return theirs == 'exception'
    if ours in (6, 7):
        return theirs in ('abort', 'exception')
    return False


def key_of(name, text):
    mnemonics = [t.split()[0] for t in text.split('; ')]
    if name == 't-it':
        return 'it-block'
    if name.endswith('exclusive') or name == 'a32-monitor':
        return '/'.join(mnemonics)
    return mnemonics[0]


def run_one(fn, uni, isa, item, text, rng):
    """One state: (kind, detail), kind pass, endian, wrong, missing, extra, fault, pc-undefined or alignment."""
    cpu, mem = initial(rng, isa, item)
    before = list(cpu.r) + [cpu.cpsr] + list(cpu.d) + [cpu.fpscr]
    error = uni.run(cpu, mem, len(text.split('; ')), PC + (2 if isa != 'arm' else 4) * len(item))
    theirs = fault_kind(error)
    fn(C.byref(cpu))
    ours = cpu.exit
    if ours == 9:
        return 'endian', ''
    if not agree(ours, theirs):
        kind = 'missing' if ours in UNDEF and theirs == 'ok' else 'extra' if ours == 0 else 'fault'
        if kind == 'missing' and re.search(r'\bpc\b', text):
            kind = 'pc-undefined'  # most uses of the PC an A57 doesn't define: CONSTRAINED UNPREDICTABLE
        elif kind == 'fault' and ours == 7 and theirs == 'ok':
            kind = 'alignment'  # QEMU 5, Unicorn's base, doesn't check LDRD/LDM/VLDM alignment
        return kind, f'ours={EXITS[ours]} syndrome={cpu.syndrome:x} unicorn={error}'
    if ours != 0 or theirs != 'ok':
        return 'pass', ''
    actual = list(cpu.r) + [cpu.cpsr] + list(cpu.d) + [cpu.fpscr]
    expected = uni.state()
    diffs = [f'{FIELDS[i]}: {b:x} -> {a:x} (unicorn {e:x})'
             for i, (b, a, e) in enumerate(zip(before, actual, expected)) if a != e]
    if diffs:
        return 'wrong', f'cpsr0={before[16]:08x} fpscr0={before[49]:08x} ' + ', '.join(diffs[:6])
    if error is None:
        theirs_mem = bytes(uni.uc.mem_read(0, SIZE))
        if bytes(mem) != theirs_mem:
            at = next(i for i in range(SIZE) if mem[i] != theirs_mem[i])
            return 'wrong', f'memory at {at:x}: {bytes(mem[at:at + 8]).hex()} (unicorn {theirs_mem[at:at + 8].hex()})'
    return 'pass', ''


def build(emitter, cc, runtime, directory, name, items, isa, opt):
    source = directory / f'{name}.txt'
    width = 8 if isa == 'arm' else 4
    source.write_text(''.join(f'~{PC:x} ' + ' '.join(f'{w:0{width}x}' for w in item) + '\n' for item in items))
    obj, dll = directory / f'{name}{opt}.o', directory / f'{name}{opt}.so'
    subprocess.run([emitter, str(source), str(obj), opt], check=True)
    subprocess.run([cc, '-shared', str(obj), runtime, '-o', str(dll)], check=True)
    lib = C.CDLL(str(dll))

    def fn_for(index):
        fn = getattr(lib, f'block_{index}', None)
        if fn is not None:
            fn.argtypes = [C.POINTER(CPU)]
            fn.restype = None
        return fn

    return fn_for


def items_of(args, name):
    rng = random.Random(f'{args.seed}:{name}')
    if name == 'a32-exclusive':
        return 'arm', exclusive_pairs(rng, 'arm', args.count)
    if name == 't32-exclusive':
        return 'thumb', exclusive_pairs(rng, 'thumb', args.count)
    if name == 'a32-monitor':
        return 'arm', monitor_sequences(rng, args.count)
    if name == 't-it':
        return 'thumb', it_blocks(rng, (args.mc, args.objdump), args.count)
    isa = 'arm' if CLASSES[name][0] == 'arm' else 'thumb'
    return isa, generate(rng, name, args.count)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--arm-emitter', required=True)
    p.add_argument('--thumb-emitter', required=True)
    p.add_argument('--runtime', required=True, help='libsnx_arm_rt.a')
    p.add_argument('--mc', default='llvm-mc')
    p.add_argument('--objdump', default='llvm-objdump')
    p.add_argument('--cc', default='clang')
    p.add_argument('--class', dest='classes', action='append', choices=sorted(CLASSES) + list(EXCLUSIVE))
    p.add_argument('--count', type=int, default=2000, help='encodings drawn per class')
    p.add_argument('--states', type=int, default=6, help='random states per encoding')
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--opt', choices=('0', '2'), default='2')
    p.add_argument('--examples', type=int, default=3)
    p.add_argument('--only', metavar='ISA:HEX[,HEX...]',
                   help='one encoding (arm:e1a00001, thumb:4408 or thumb:e850,0f00), every state printed')
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-arm-fuzz-') as path:
        directory = pathlib.Path(path)
        if args.only:
            isa, words = args.only.split(':')
            item = [int(w, 16) for w in words.split(',')]
            text = disassemble((args.mc, args.objdump), isa, [item])[0] or '(refused by llvm-mc)'
            emitter = args.arm_emitter if isa == 'arm' else args.thumb_emitter
            fn = build(emitter, args.cc, args.runtime, directory, 'only', [item], isa, args.opt)(0)
            uni = Unicorn(isa != 'arm')
            print(text)
            for state in range(args.states):
                rng = random.Random(f'{args.seed}:only:{state}')
                cpu, _ = initial(random.Random(f'{args.seed}:only:{state}'), isa, item)
                kind, detail = run_one(fn, uni, isa, item, text, rng)
                print(f'  {state}: {kind} {detail}')
                if kind != 'pass':
                    print('     r=' + ' '.join(f'{v:x}' for v in cpu.r) + f' cpsr={cpu.cpsr:x} fpscr={cpu.fpscr:x}')
                    print('     d=' + ' '.join(f'{v:x}' for v in cpu.d))
            return 0
        wrong = 0
        for name in args.classes or list(CLASSES) + list(EXCLUSIVE):
            isa, items = items_of(args, name)
            texts = disassemble((args.mc, args.objdump), isa, items)
            kept = {tuple(i): (i, t) for i, t in zip(items, texts) if t}
            items, texts = [k[0] for k in kept.values()], [k[1] for k in kept.values()]
            emitter = args.arm_emitter if isa == 'arm' else args.thumb_emitter
            fn_for = build(emitter, args.cc, args.runtime, directory, name, items, isa, args.opt)
            uni = Unicorn(isa != 'arm')
            report = collections.defaultdict(lambda: collections.defaultdict(list))
            runs = 0
            for index, (item, text) in enumerate(zip(items, texts)):
                entry = report[key_of(name, text)]
                fn = fn_for(index)
                if fn is None:
                    entry['refused'].append(text)
                    continue
                if unicorn_dies(isa, item):  # left to another host, as arm_test.py says
                    entry['unicorn-dies'].append(f'{text}  [{isa}:{",".join(f"{w:x}" for w in item)}]')
                    continue
                for state in range(args.states):
                    rng = random.Random(f'{args.seed}:{name}:{index}:{state}')
                    kind, detail = run_one(fn, uni, isa, item, text, rng)
                    runs += 1
                    entry[kind].append(f'{text}  [{isa}:{",".join(f"{w:x}" for w in item)}] {detail}')
                    if kind != 'pass':
                        if kind in ('wrong', 'missing', 'extra', 'fault') and implementation_defined(name, item):
                            entry[kind].pop()
                            entry['impl-defined'].append(f'{text}  [{isa}:{",".join(f"{w:x}" for w in item)}]')
                        elif kind in ('wrong', 'missing', 'extra', 'fault') and soft_fail(args.mc, isa, item):
                            entry[kind].pop()
                            entry['unpredictable'].append(f'{text}  [{isa}:{",".join(f"{w:x}" for w in item)}]')
                        break
            kinds = ('wrong', 'missing', 'extra', 'fault', 'refused', 'pc-undefined', 'alignment', 'unpredictable',
                     'impl-defined', 'unicorn-dies')
            bad = {k: v for k, v in report.items() if any(v[x] for x in kinds)}
            print(f'== {name}: {len(items)} encodings, {runs} runs, {len(bad)} mnemonics disagree', flush=True)
            for mnemonic in sorted(bad, key=lambda m: -sum(len(bad[m][x]) for x in kinds)):
                entry = bad[mnemonic]
                summary = ', '.join(f'{x} {len(entry[x])}' for x in kinds + ('endian',) if entry[x])
                print(f'  {mnemonic}: {summary} (of {sum(len(v) for v in entry.values())})')
                for x in kinds:
                    for line in entry[x][: args.examples]:
                        print(f'    {x}: {line}')
            wrong += sum(len(v['wrong']) for v in bad.values())
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
