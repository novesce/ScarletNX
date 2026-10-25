#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check a straight-line Maxwell output slice against saved GPU outputs.

Envytools supplies the instruction disassembly. Integer arithmetic below rounds
binary32 operations exactly once, independently of the runtime's translator and
the host's floating-point mode. This is a deliberately partial reference: a
passing position slice is not a passing shader, draw, or game. See --help.
"""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import struct
import subprocess


class Unsupported(ValueError):
    """An operation or required input has no reference implementation."""


SIGN = 0x80000000
INF = 0x7F800000
NAN = 0x7FC00000


def is_nan(bits):
    return bits & 0x7FFFFFFF > INF


def flush(bits):
    return bits & SIGN if bits & INF == 0 else bits


def finite_parts(bits):
    exponent = (bits >> 23) & 255
    mantissa = bits & 0x7FFFFF
    if exponent:
        mantissa |= 1 << 23
    if bits & SIGN:
        mantissa = -mantissa
    return mantissa, exponent - 150 if exponent else -149


def round_binary32(integer, exponent, negative_zero=False):
    """Round integer * 2**exponent to binary32, nearest with ties to even."""
    if not integer:
        return SIGN if negative_zero else 0
    sign = SIGN if integer < 0 else 0
    integer = abs(integer)
    shift = max(integer.bit_length() - 24, -149 - exponent)
    if shift > 0:
        quotient, remainder = divmod(integer, 1 << shift)
        half = 1 << (shift - 1)
        integer = quotient + (remainder > half or (remainder == half and quotient & 1))
    else:
        integer <<= -shift
    exponent += shift
    if not integer:
        return sign
    high = integer.bit_length() - 1
    actual_exponent = high + exponent
    if actual_exponent > 127:
        return sign | INF
    if actual_exponent < -126:
        return sign | integer
    # Carry after rounding can make the significand 25 bits wide.
    mantissa = integer >> (high - 23) if high >= 23 else integer << (23 - high)
    return sign | ((actual_exponent + 127) << 23) | (mantissa & 0x7FFFFF)


def add_exact(a, b):
    if is_nan(a) or is_nan(b):
        return NAN
    if a & 0x7FFFFFFF == INF or b & 0x7FFFFFFF == INF:
        return NAN if a ^ b == SIGN else a if a & 0x7FFFFFFF == INF else b
    ma, ea = finite_parts(a)
    mb, eb = finite_parts(b)
    exponent = min(ea, eb)
    return round_binary32((ma << (ea - exponent)) + (mb << (eb - exponent)), exponent,
                          bool(a & b & SIGN))


def mul_exact(a, b):
    if is_nan(a) or is_nan(b):
        return NAN
    sign = (a ^ b) & SIGN
    aa, ab = a & 0x7FFFFFFF, b & 0x7FFFFFFF
    if aa == INF or ab == INF:
        return NAN if not aa or not ab else sign | INF
    ma, ea = finite_parts(a)
    mb, eb = finite_parts(b)
    return round_binary32(ma * mb, ea + eb, bool(sign))


def fma_exact(a, b, c):
    if is_nan(a) or is_nan(b) or is_nan(c):
        return NAN
    aa, ab, ac = a & 0x7FFFFFFF, b & 0x7FFFFFFF, c & 0x7FFFFFFF
    product_sign = (a ^ b) & SIGN
    if aa == INF or ab == INF:
        if not aa or not ab or (ac == INF and product_sign != c & SIGN):
            return NAN
        return product_sign | INF
    if ac == INF:
        return c
    ma, ea = finite_parts(a)
    mb, eb = finite_parts(b)
    mc, ec = finite_parts(c)
    exponent = min(ea + eb, ec)
    value = (ma * mb << (ea + eb - exponent)) + (mc << (ec - exponent))
    return round_binary32(value, exponent, bool(product_sign & c & SIGN))


def float_operation(operation, operands, ftz=False):
    if ftz:
        operands = [flush(v) for v in operands]
    functions = {"fadd": add_exact, "fmul": mul_exact, "ffma": fma_exact}
    result = functions[operation](*operands)
    return flush(result) if ftz else result


def normalized_binary32(numerator, denominator):
    """Correctly round a signed normalized component without host floats."""
    if not numerator:
        return 0
    sign = SIGN if numerator < 0 else 0
    numerator = abs(numerator)
    exponent = numerator.bit_length() - denominator.bit_length()
    if (numerator << max(0, -exponent)) < (denominator << max(0, exponent)):
        exponent -= 1
    shift = 23 - max(exponent, -126)
    quotient, remainder = divmod(numerator << max(shift, 0), denominator << max(-shift, 0))
    divisor = denominator << max(-shift, 0)
    quotient += 2 * remainder > divisor or (2 * remainder == divisor and quotient & 1)
    return sign | round_binary32(quotient, -shift)


def vertex_format(name):
    """Independent vertex-format descriptions; no SDK headers are loaded."""
    name = name.removeprefix("NVN_FORMAT_")
    packed = re.fullmatch(r"RGB10A2(SN|UI|I|_UI2F|_I2F)?", name)
    if packed:
        return [10, 10, 10, 2], packed[1] or "UNORM"
    scalar = re.fullmatch(r"(RGBA|RGB|RG|R)(8|16|32)(F|SN|UI|I|_UI2F|_I2F)?", name)
    if not scalar:
        raise Unsupported(f"vertex format {name} has no reference")
    width, kind = int(scalar[2]), scalar[3] or "UNORM"
    if (kind == "F" and width == 8) or (width == 32 and kind in {"UNORM", "SN"}):
        raise Unsupported(f"invalid vertex format {name}")
    return [width] * len(scalar[1]), kind


def decode_vertex_attribute(data, offset, name):
    """Return the four register values after vertex fetch and conversion."""
    widths, kind = vertex_format(name)
    size = sum(widths) // 8
    if offset < 0 or offset + size > len(data):
        raise Unsupported("vertex attribute exceeds its captured binding range")
    packed = int.from_bytes(data[offset:offset + size], "little")
    values = []
    for width in widths:
        value = packed & ((1 << width) - 1)
        packed >>= width
        signed = value - (1 << width) if value >> (width - 1) else value
        if kind == "F":
            if width == 16:
                sign, exponent, fraction = (value & 0x8000) << 16, (value >> 10) & 31, value & 1023
                if exponent == 31:
                    value = sign | INF | fraction << 13
                elif exponent:
                    value = sign | (exponent + 112) << 23 | fraction << 13
                else:
                    value = sign | round_binary32(fraction, -24)
        elif kind == "UNORM":
            value = normalized_binary32(value, (1 << width) - 1)
        elif kind == "SN":
            value = normalized_binary32(max(signed, -((1 << (width - 1)) - 1)), (1 << (width - 1)) - 1)
        elif kind == "I":
            value = signed & 0xFFFFFFFF
        elif kind == "_I2F":
            value = round_binary32(signed, 0)
        elif kind == "_UI2F":
            value = round_binary32(value, 0)
        values.append(value)
    one = 1 if kind in {"I", "UI"} else 0x3F800000
    return (values + [0, 0, 0, one][len(values):])[:4]


def vertex_inputs(descriptions, streams, vertex, instance=0):
    """Apply binding-relative offsets, zero strides, and instance divisors."""
    attributes = {}
    for desc in descriptions:
        address = int(desc["address"], 0)
        binding = desc.get("stream", 0)
        relative = desc["offset"]
        if not 0 <= binding < 16 or not 0 <= relative <= 16383:
            raise Unsupported("vertex binding index or relative offset is outside the API limits")
        if not desc.get("enabled", True):
            if "constant_values" not in desc:
                raise Unsupported("disabled attribute requires captured constant values")
            values = desc["constant_values"]
        else:
            if binding not in streams:
                raise Unsupported(f"vertex binding {binding} not captured")
            stream, data = streams[binding]
            stride, divisor = stream["stride"], stream.get("divisor", 0)
            if stride < 0 or divisor < 0:
                raise Unsupported("negative vertex stride or divisor")
            record = instance // divisor if divisor else vertex
            if record < 0:
                raise Unsupported("negative vertex record")
            offset = stream.get("offset", 0) + record * stride + relative
            fmt = desc.get("format", {1: "R32F", 2: "RG32F", 3: "RGB32F", 4: "RGBA32F"}.get(desc.get("components")))
            if fmt is None:
                raise Unsupported("attribute needs a format or a component count from 1 to 4")
            values = decode_vertex_attribute(data, offset, fmt)
        components = desc.get("components", 4)
        if not 1 <= components <= 4 or len(values) < components:
            raise Unsupported("invalid attribute component count")
        for component, value in enumerate(values[:components]):
            key = address + 4 * component
            if key in attributes:
                raise Unsupported("attribute destinations overlap")
            attributes[key] = value
    return attributes


@dataclass(frozen=True)
class Instruction:
    pc: int
    operation: str
    arguments: tuple
    predicate: str = ""


def parse_disassembly(text):
    instructions = []
    terminated = False
    for line in re.sub(r"\x1b\[[0-9;]*m", "", text).splitlines():
        match = re.match(r"^([0-9a-fA-F]+):\s+[0-9a-fA-F]{8}\s+[0-9a-fA-F]{8}\s+(.*)$", line)
        if not match:
            continue
        pc, assembly = int(match[1], 16), match[2]
        tokens = re.findall(r"c\d+\[[^]]+\]|a\[[^]]+\]|\S+", assembly)
        if not tokens:
            continue
        predicate = ""
        if tokens[0] == "not" or re.fullmatch(r"\$p\d+", tokens[0]):
            predicate = tokens.pop(0)
            if predicate == "not":
                predicate += " " + tokens.pop(0)
        operation = tokens.pop(0)
        if operation == "sched":
            continue
        instruction = Instruction(pc, operation, tuple(tokens), predicate)
        instructions.append(instruction)
        if operation == "exit" and not predicate:
            terminated = True
            break
    if not instructions:
        raise Unsupported("no Envytools instructions found")
    if not terminated:
        raise Unsupported("no unconditional EXIT; the captured program may be incomplete")
    return instructions


def register(token):
    match = re.fullmatch(r"\$r(\d+)", token)
    return int(match[1]) if match else None


def attribute(token):
    match = re.fullmatch(r"a\[(0x[0-9a-fA-F]+|\d+)\]", token)
    return int(match[1], 0) if match else None


def destinations(instruction):
    op, args = instruction.operation, instruction.arguments
    if op in {"st", "depbar", "nop", "exit"}:
        return set()
    scalar = {"mov", "fmul", "fadd", "ffma", "imnmx", "iadd", "iadd32i", "shl", "shr", "xmad", "lop"}
    regs = [register(a) for a in args if register(a) is not None]
    if op in scalar and regs:
        return {regs[0]}
    if op == "ld" and regs:
        if args[0] not in {"b32", "b64", "b128"}:
            raise Unsupported(f"0x{instruction.pc:x}: unknown load width")
        return set(range(regs[0], regs[0] + int(args[0][1:]) // 32))
    if op == "tld" and regs:
        # TLD's component mask selects consecutive destination registers.
        try:
            mask = int(args[-1], 0)
        except ValueError as error:
            raise Unsupported(f"0x{instruction.pc:x}: unknown TLD mask") from error
        if not 0 < mask <= 15:
            raise Unsupported(f"0x{instruction.pc:x}: unsupported TLD mask")
        return set(range(regs[0], regs[0] + mask.bit_count()))
    raise Unsupported(f"0x{instruction.pc:x}: output analysis does not support {op}")


def output_slice(instructions, outputs):
    stores = []
    for i, inst in enumerate(instructions):
        if inst.operation == "st" and len(inst.arguments) >= 3 and attribute(inst.arguments[1]) in outputs:
            stores.append(i)
    if not stores:
        raise Unsupported("the requested attribute outputs are never stored")
    end = max(stores)
    flow = {"bra", "brx", "jmp", "cal", "ret", "ssy", "sync", "pbk", "brk", "pcnt", "cont", "pexit"}
    # A branch after the last store could return to that store. A dynamic
    # attribute address could overwrite it without a statically named target.
    for inst in instructions:
        if inst.operation in {"???", "C", "L"}:
            raise Unsupported(f"0x{inst.pc:x}: unknown instruction")
        if inst.operation in flow or (inst.operation == "exit" and inst.predicate):
            raise Unsupported(f"0x{inst.pc:x}: control flow needs a path-aware reference")
        if inst.operation == "st" and (len(inst.arguments) < 3 or attribute(inst.arguments[1]) is None):
            raise Unsupported(f"0x{inst.pc:x}: dynamic or unknown output store")
    needed_outputs, needed_regs, selected = set(outputs), set(), []
    for inst in reversed(instructions[:end + 1]):
        op, args = inst.operation, inst.arguments
        if op == "st":
            if len(args) < 3 or args[0] != "b32" or attribute(args[1]) is None:
                raise Unsupported(f"0x{inst.pc:x}: unsupported attribute store")
            addr = attribute(args[1])
            if addr not in needed_outputs:
                continue
            if args[3:] != ("0x0",):
                raise Unsupported(f"0x{inst.pc:x}: indexed attribute store")
            needed_outputs.remove(addr)
            reg = register(args[2])
            if reg is not None:
                needed_regs.add(reg)
            selected.append(inst)
        else:
            defs = destinations(inst)
            if not defs & needed_regs:
                continue
            if op not in {"mov", "ld", "fadd", "fmul", "ffma"}:
                raise Unsupported(f"0x{inst.pc:x}: required operation {op} has no execution reference")
            needed_regs -= defs
            for arg in args:
                reg = register(arg)
                if reg is not None and reg not in defs:
                    needed_regs.add(reg)
            # A destination can also occur as a source (e.g. FFMA R0,R0,...).
            first = next((i for i, arg in enumerate(args) if register(arg) is not None), len(args))
            for arg in args[first + 1:]:
                reg = register(arg)
                if reg is not None:
                    needed_regs.add(reg)
            selected.append(inst)
        if inst.predicate:
            raise Unsupported(f"0x{inst.pc:x}: predicated output dependency")
        if not needed_outputs and not needed_regs:
            break
    if needed_outputs or needed_regs:
        raise Unsupported(f"missing inputs: outputs {sorted(needed_outputs)}, registers {sorted(needed_regs)}")
    return list(reversed(selected))


def word(data, offset, label):
    if offset < 0 or offset + 4 > len(data):
        raise Unsupported(f"{label}: word at 0x{offset:x} is outside its captured data")
    return struct.unpack_from("<I", data, offset)[0]


def execute(instructions, attributes, buffers):
    regs, outputs = {}, {}

    def operand(token):
        reg = register(token)
        if reg is not None:
            if reg not in regs:
                raise Unsupported(f"uninitialized register R{reg}")
            return regs[reg]
        cb = re.fullmatch(r"c(\d+)\[(0x[0-9a-fA-F]+|\d+)\]", token)
        if cb:
            bank, offset = int(cb[1]), int(cb[2], 0)
            if bank not in buffers:
                raise Unsupported(f"constant buffer {bank} not captured")
            return word(buffers[bank], offset, f"c{bank}")
        try:
            return int(token, 0) & 0xFFFFFFFF
        except ValueError as error:
            raise Unsupported(f"operand {token} has no reference") from error

    for inst in instructions:
        op, args = inst.operation, list(inst.arguments)
        if op == "st":
            outputs[attribute(args[1])] = operand(args[2])
        elif op == "ld":
            if len(args) != 4 or args[0] != "b32" or args[3] != "0x0":
                raise Unsupported(f"0x{inst.pc:x}: load must be an unindexed b32 attribute")
            addr = attribute(args[2])
            if addr not in attributes:
                raise Unsupported(f"attribute 0x{addr:x} not captured" if addr is not None else "indexed attribute load")
            regs[register(args[1])] = attributes[addr]
        elif op == "mov":
            if len(args) != 3 or args[2] != "0xf":
                raise Unsupported(f"0x{inst.pc:x}: partial MOV mask")
            regs[register(args[0])] = operand(args[1])
        else:
            ftz = args[0] == "ftz"
            if ftz:
                args.pop(0)
            dest = register(args.pop(0))
            if dest is None:
                raise Unsupported(f"0x{inst.pc:x}: unsupported float modifier")
            values = []
            while args:
                modifier = args.pop(0)
                if modifier in {"neg", "abs"}:
                    if not args:
                        raise Unsupported("missing modified operand")
                    value = operand(args.pop(0))
                    value = value ^ SIGN if modifier == "neg" else value & ~SIGN
                else:
                    value = operand(modifier)
                values.append(value)
            arity = 3 if op == "ffma" else 2
            if len(values) != arity:
                raise Unsupported(f"0x{inst.pc:x}: wrong {op} operand count")
            regs[dest] = float_operation(op, values, ftz)
    return outputs


def check_capture(manifest, base, envytools="envydis"):
    if manifest.get("version") != 1:
        raise Unsupported("unsupported capture manifest version")
    if "sass" in manifest:
        text = (base / manifest["sass"]).read_text()
    else:
        code = base / manifest["code"]
        result = subprocess.run([envytools, "-i", "-m", "gm107", str(code)], check=True, capture_output=True, text=True)
        text = result.stdout
    instructions = parse_disassembly(text)
    outputs = [int(v, 0) if isinstance(v, str) else v for v in manifest["outputs"]]
    selected = output_slice(instructions, outputs)
    buffers = {int(bank): (base / file).read_bytes() for bank, file in manifest["constant_buffers"].items()}
    vertices = manifest["vertices"]
    stream_specs = vertices.get("streams", {"0": vertices})
    streams = {}
    for binding, stream in stream_specs.items():
        data = (base / stream["file"]).read_bytes()
        size = stream.get("size", len(data))
        if size < 0 or size > len(data):
            raise Unsupported("captured vertex binding size is invalid")
        streams[int(binding)] = (stream, data[:size])
    observed = manifest["observed"]
    gpu = (base / observed["file"]).read_bytes()
    count = vertices["count"]
    if not isinstance(count, int) or count <= 0:
        raise Unsupported("the capture must contain at least one vertex")
    if not outputs or len(set(outputs)) != len(outputs):
        raise Unsupported("outputs must be nonempty and unique")
    if observed["stride"] < 4 * len(outputs):
        raise Unsupported("invalid vertex or observed-output stride")
    mismatches, nan_payloads = [], 0
    mismatch_count = 0
    abnormal_inputs = set()
    float_inputs = set()
    for desc in vertices["attributes"]:
        kind = vertex_format(desc["format"])[1] if "format" in desc else "F"
        if kind not in {"I", "UI"}:
            address = int(desc["address"], 0)
            float_inputs.update(range(address, address + desc.get("components", 4) * 4, 4))
    for vertex in range(count):
        attrs = vertex_inputs(vertices["attributes"], streams, vertex + vertices.get("first_vertex", 0),
                              vertices.get("instance", 0))
        if any(attrs[address] & INF == INF for address in float_inputs):
            abnormal_inputs.add(vertex)
        calculated = execute(selected, attrs, buffers)
        for component, addr in enumerate(outputs):
            actual = word(gpu, observed.get("offset", 0) + vertex * observed["stride"] + component * 4, "GPU output")
            expected = calculated[addr]
            if actual == expected:
                continue
            if is_nan(actual) and is_nan(expected):
                nan_payloads += 1
                continue
            mismatch_count += 1
            if len(mismatches) < 32:
                mismatches.append({"vertex": vertex, "attribute": hex(addr),
                                   "reference": f"{expected:08x}", "gpu": f"{actual:08x}"})
    return {"status": "FAIL_POSITION_SLICE" if mismatches else "PASS_POSITION_SLICE",
            "vertices": count, "components_checked": count * len(outputs),
            "instructions_in_program": len(instructions), "instructions_executed_per_vertex": len(selected),
            "whole_shader_checked": False, "nan_payload_differences": nan_payloads,
            "mismatched_components": mismatch_count,
            "vertices_with_nonfinite_inputs": len(abnormal_inputs), "first_mismatches": mismatches,
            "slice": [{"pc": hex(i.pc), "operation": i.operation, "arguments": i.arguments} for i in selected]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="local JSON containing captured vertex, constant-buffer and GPU-output files")
    parser.add_argument("--envydis", default="envydis")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        result = check_capture(json.loads(args.manifest.read_text()), args.manifest.parent, args.envydis)
    except (Unsupported, OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        result = {"status": "UNSUPPORTED", "whole_shader_checked": False, "reason": str(error)}
    text = json.dumps(result, indent=2)
    if args.report:
        args.report.write_text(text + "\n")
    print(text)
    return 0 if result["status"] == "PASS_POSITION_SLICE" else 2 if result["status"] == "UNSUPPORTED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
