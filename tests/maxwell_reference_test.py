#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The offline reference must catch wrong results and refuse incomplete checks."""

import ctypes
import ctypes.util
import importlib.util
from pathlib import Path
import random
import struct
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("maxwell_reference", Path(__file__).resolve().parents[1] / "tools/maxwell_reference.py")
reference = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reference
spec.loader.exec_module(reference)


class Binary32(unittest.TestCase):
    def test_rounding_and_special_values(self):
        self.assertEqual(reference.add_exact(0x3F800000, 0x33800000), 0x3F800000)  # ties to even
        self.assertEqual(reference.add_exact(0x3F800001, 0x33800000), 0x3F800002)
        self.assertEqual(reference.mul_exact(0x7F7FFFFF, 0x40000000), reference.INF)
        self.assertEqual(reference.mul_exact(1, 0x3F000000), 0)
        self.assertEqual(reference.mul_exact(3, 0x3F000000), 2)
        self.assertEqual(reference.mul_exact(0x80000001, 0x3F000000), reference.SIGN)
        self.assertEqual(reference.add_exact(reference.SIGN, reference.SIGN), reference.SIGN)
        self.assertEqual(reference.add_exact(0x3F800000, 0xBF800000), 0)
        self.assertTrue(reference.is_nan(reference.mul_exact(reference.INF, 0)))
        self.assertTrue(reference.is_nan(reference.fma_exact(reference.INF, 0, reference.INF)))
        self.assertTrue(reference.is_nan(reference.fma_exact(reference.INF, 0x3F800000, 0xFF800000)))

    def test_fma_rounds_only_once(self):
        a, b, c = 0x3F800001, 0x3F7FFFFE, 0xBF800000
        self.assertEqual(reference.fma_exact(a, b, c), 0xA8800000)
        self.assertEqual(reference.add_exact(reference.mul_exact(a, b), c), 0)

    def test_ftz_is_per_operation(self):
        self.assertEqual(reference.float_operation("fadd", [1, 1]), 2)
        self.assertEqual(reference.float_operation("fadd", [1, 1], True), 0)
        self.assertEqual(reference.float_operation("fmul", [0x80800000, 0x3F000000], True), reference.SIGN)
        self.assertEqual(reference.float_operation("ffma", [1, 0x3F800000, 0x3F800000], True), 0x3F800000)

    def test_random_against_libm_fmaf(self):
        library = ctypes.util.find_library("m")
        if not library:
            self.skipTest("libm unavailable; deterministic arithmetic cases still run")
        function = ctypes.CDLL(library).fmaf
        function.argtypes = [ctypes.c_float] * 3
        function.restype = ctypes.c_float
        rng = random.Random(0x50454143)
        for _ in range(4000):
            inputs = [rng.getrandbits(32) for _ in range(3)]
            values = [struct.unpack("<f", struct.pack("<I", bits))[0] for bits in inputs]
            expected = struct.unpack("<I", struct.pack("<f", function(*values)))[0]
            actual = reference.fma_exact(*inputs)
            if reference.is_nan(expected):
                self.assertTrue(reference.is_nan(actual))
            else:
                self.assertEqual(actual, expected, [hex(v) for v in inputs])


class Captures(unittest.TestCase):
    # Synthetic Envytools text; no game instructions, constants, or assets.
    assembly = """00000000: fc0007e0 001f8000 sched (st 0x0)
00000008: 0807ff00 efd87f80 ld b32 $r0 a[0x80] 0x0
00000010: 00070000 4c58100c fadd ftz $r0 $r0 c3[0x0]
00000018: 0707ff00 eff07f80 st b32 a[0x70] $r0 0x0
00000020: fc0007e0 001f8000 sched (st 0x0)
00000028: 0007000f e3000000 exit
"""

    def test_reused_destination_remains_a_dependency(self):
        code = reference.parse_disassembly(self.assembly)
        sliced = reference.output_slice(code, [0x70])
        self.assertEqual(len(sliced), 3)
        output = reference.execute(sliced, {0x80: 0x40000000}, {3: struct.pack("<I", 0x3F800000)})
        self.assertEqual(output[0x70], 0x40400000)

    def test_missing_input_is_not_zero(self):
        sliced = reference.output_slice(reference.parse_disassembly(self.assembly), [0x70])
        with self.assertRaises(reference.Unsupported):
            reference.execute(sliced, {}, {3: bytes(4)})
        with self.assertRaises(reference.Unsupported):
            reference.execute(sliced, {0x80: 0}, {})

    def test_predicates_and_control_flow_are_not_silent_passes(self):
        for changed in [self.assembly.replace("ld b32", "$p0 ld b32"),
                        self.assembly.replace("fadd ftz", "bra"),
                        self.assembly.replace("fadd ftz", "mufu rcp")]:
            with self.assertRaises(reference.Unsupported):
                reference.output_slice(reference.parse_disassembly(changed), [0x70])

    def test_incomplete_outputs_are_rejected(self):
        with self.assertRaises(reference.Unsupported):
            reference.output_slice(reference.parse_disassembly(self.assembly), [0x70, 0x74])

    def test_wrong_gpu_result_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / "program.sass").write_text(self.assembly)
            (folder / "cb.bin").write_bytes(struct.pack("<I", 0x3F800000))
            (folder / "vertices.bin").write_bytes(struct.pack("<2I", 0x40000000, 0x40400000))
            (folder / "output.bin").write_bytes(struct.pack("<2I", 0x40400000, 0x40800000))
            manifest = {"version": 1, "sass": "program.sass", "outputs": ["0x70"],
                        "constant_buffers": {"3": "cb.bin"},
                        "vertices": {"file": "vertices.bin", "stride": 4, "count": 2,
                                     "attributes": [{"address": "0x80", "offset": 0, "components": 1}]},
                        "observed": {"file": "output.bin", "stride": 4}}
            result = reference.check_capture(manifest, folder)
            self.assertEqual(result["status"], "PASS_POSITION_SLICE")
            self.assertFalse(result["whole_shader_checked"])
            # A one-bit arithmetic error must fail the same frozen capture.
            (folder / "output.bin").write_bytes(struct.pack("<2I", 0x40400001, 0x40800000))
            result = reference.check_capture(manifest, folder)
            self.assertEqual(result["status"], "FAIL_POSITION_SLICE")
            self.assertEqual(result["mismatched_components"], 1)
            manifest["vertices"]["count"] = 0
            with self.assertRaises(reference.Unsupported):
                reference.check_capture(manifest, folder)


class VertexContracts(unittest.TestCase):
    def test_float_bits_half_values_and_defaults(self):
        self.assertEqual(reference.decode_vertex_attribute(struct.pack("<3I", 0x3F800000, 0x80000000, 0x7FC12345),
                                                           0, "NVN_FORMAT_RGB32F"),
                         [0x3F800000, 0x80000000, 0x7FC12345, 0x3F800000])
        self.assertEqual(reference.decode_vertex_attribute(struct.pack("<4H", 0x3C00, 0xBC00, 1, 0x8000),
                                                           0, "RGBA16F"),
                         [0x3F800000, 0xBF800000, 0x33800000, 0x80000000])
        for half in range(65536):
            expected = struct.unpack("<I", struct.pack("<f", struct.unpack("<e", struct.pack("<H", half))[0]))[0]
            actual = reference.decode_vertex_attribute(struct.pack("<H", half), 0, "R16F")[0]
            if reference.is_nan(expected):
                self.assertTrue(reference.is_nan(actual))
            else:
                self.assertEqual(actual, expected)

    def test_normalized_endpoints_and_rounding(self):
        self.assertEqual(reference.decode_vertex_attribute(bytes([0, 127, 128, 255]), 0, "RGBA8SN"),
                         [0, 0x3F800000, 0xBF800000, 0xBC010204])
        for value in range(65536):
            expected = struct.unpack("<I", struct.pack("<f", value / 65535))[0]
            self.assertEqual(reference.normalized_binary32(value, 65535), expected)
        for value in range(-128, 128):
            expected = struct.unpack("<I", struct.pack("<f", max(-1.0, value / 127)))[0]
            actual = reference.decode_vertex_attribute(bytes([value & 255]), 0, "R8SN")[0]
            self.assertEqual(actual, expected)

    def test_integer_and_cast_to_float_are_different(self):
        data = struct.pack("<4I", 0xFFFFFFFF, 0x80000000, 0x01000001, 0x7FFFFFFF)
        self.assertEqual(reference.decode_vertex_attribute(data, 0, "RGBA32I"),
                         [0xFFFFFFFF, 0x80000000, 0x01000001, 0x7FFFFFFF])
        self.assertEqual(reference.decode_vertex_attribute(data, 0, "RGBA32_I2F"),
                         [0xBF800000, 0xCF000000, 0x4B800000, 0x4F000000])
        self.assertEqual(reference.decode_vertex_attribute(bytes([255]), 0, "R8UI"), [255, 0, 0, 1])

    def test_packed_components(self):
        data = struct.pack("<I", 1023 | 512 << 10 | 3 << 30)
        values = reference.decode_vertex_attribute(data, 0, "RGB10A2")
        self.assertEqual(values, [0x3F800000, reference.normalized_binary32(512, 1023), 0, 0x3F800000])
        data = struct.pack("<I", 512 | 511 << 10 | 1023 << 20 | 2 << 30)
        self.assertEqual(reference.decode_vertex_attribute(data, 0, "RGB10A2I"),
                         [0xFFFFFE00, 511, 0xFFFFFFFF, 0xFFFFFFFE])
        values = reference.decode_vertex_attribute(data, 0, "RGB10A2SN")
        self.assertEqual(values[0], 0xBF800000)
        self.assertEqual(values[1], 0x3F800000)
        self.assertEqual(values[3], 0xBF800000)

    def test_zero_stride_offsets_and_instance_divisor(self):
        data = b"pad!" + struct.pack("<4I", 0x3F800000, 0x40000000, 0x40400000, 0x40800000)
        desc = [{"address": "0x80", "offset": 4, "format": "RG32F"}]
        streams = {0: ({"stride": 0}, data)}
        self.assertEqual(reference.vertex_inputs(desc, streams, 99),
                         {0x80: 0x3F800000, 0x84: 0x40000000, 0x88: 0, 0x8C: 0x3F800000})
        streams[0] = ({"stride": 8, "divisor": 2}, data)
        self.assertEqual(reference.vertex_inputs(desc, streams, 99, 3)[0x80], 0x40400000)
        streams[0] = ({"stride": 8, "offset": 4}, data)
        desc[0]["offset"] = 0
        self.assertEqual(reference.vertex_inputs(desc, streams, 1)[0x80], 0x40400000)

    def test_missing_disabled_and_out_of_range_inputs(self):
        desc = [{"address": "0x80", "offset": 0, "format": "RGB32F"}]
        for streams in [{}, {0: ({"stride": 12}, bytes(8))}, {0: ({"stride": -1}, bytes(16))}]:
            with self.assertRaises(reference.Unsupported):
                reference.vertex_inputs(desc, streams, 0)
        desc[0]["enabled"] = False
        with self.assertRaises(reference.Unsupported):
            reference.vertex_inputs(desc, {}, 0)
        desc[0]["constant_values"] = [1, 2, 3, 4]
        self.assertEqual(reference.vertex_inputs(desc, {}, 0)[0x80], 1)
        desc[0]["stream"] = 16
        with self.assertRaises(reference.Unsupported):
            reference.vertex_inputs(desc, {}, 0)
        with self.assertRaises(reference.Unsupported):
            reference.decode_vertex_attribute(bytes(8), 0, "RGBA8_SRGB")


if __name__ == "__main__":
    unittest.main()
