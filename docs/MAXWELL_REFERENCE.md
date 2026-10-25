<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Offline Maxwell output checks

`tools/maxwell_reference.py` compares saved shader outputs with an independent
Python calculation of the Maxwell instructions that produced them. It uses
Envytools' GM107 disassembly. Neither the game nor the renderer needs to run.
Captured game code, constants, vertex data and results stay outside the repository.

Run its synthetic regressions with:

```sh
python3 tests/maxwell_reference_test.py
```

Run a saved capture with:

```sh
python3 tools/maxwell_reference.py /absolute/private/capture.json --report /absolute/private/report.json
```

The report distinguishes `PASS_POSITION_SLICE`, `FAIL_POSITION_SLICE` and
`UNSUPPORTED`, with exit codes 0, 1 and 2. A pass checks the requested output
dependencies, not every instruction or an entire rendered image. The report
always records `whole_shader_checked: false`, the number of instructions checked,
the component comparisons and any nonfinite floating-point inputs. Matching NaNs
are compared by class; differences in their payloads are counted separately.

## Arithmetic and vertex inputs

The execution reference handles unpredicated scalar attribute loads/stores,
full-register moves, FADD, FMUL and FFMA in straight-line output dependencies.
It rounds binary32 operations with integer arithmetic, including subnormals,
overflow, signed zero and one rounding for fused multiply-add. FTZ is applied
per instruction. Random FMA cases are also checked against the host's `fmaf`
when available. This keeps Python binary64 arithmetic out of the reference.

Vertex fetch supports the R/RG/RGB/RGBA families with 8-, 16- and 32-bit
components: applicable float, UNORM, SNORM, signed/unsigned integer and integer
to float conversions. Packed RGB10A2 variants are included. Unprovided components
expand to zero, with the fourth component one in the appropriate numeric type.
Signed normalized minima clamp to -1; half-floats preserve subnormals when widened.

The binding-relative byte offset, record stride and instance divisor are separate.
A zero stride reuses one record. A positive divisor selects the instance record
instead of the vertex record. Binding indices 0 through 15 and attribute offsets
0 through 16383 are checked. Missing bindings, missing constants, truncated ranges
and unknown formats are errors. Disabled attributes need their captured constant
values; the checker does not invent them.

This reference does not implement texture filtering, derivatives, atomics,
workgroup execution or general control flow. Required operations without an
implementation are refused. Branches, dynamic output stores and incomplete
programs also refuse a check, including branches after a requested output store
that might return to it. These refusals must never be counted as passes.

## Capture manifest

Paths are relative to the manifest. `code` is a raw Maxwell instruction body,
without its 80-byte graphics program header; alternatively `sass` names a saved
Envytools listing. The following example describes synthetic data, not game data:

```json
{
  "version": 1,
  "code": "program.body",
  "outputs": ["0x70", "0x74", "0x78", "0x7c"],
  "constant_buffers": {"3": "view.bin", "4": "model.bin"},
  "vertices": {
    "file": "positions.bin",
    "stride": 12,
    "count": 3,
    "attributes": [
      {"address": "0x80", "offset": 0, "format": "RGB32F", "components": 3}
    ]
  },
  "observed": {"file": "postvs.bin", "stride": 16}
}
```

`vertices.streams` can replace its `file` and `stride` with a dictionary keyed by
binding index. Each stream has a `file`, `stride`, optional binding `offset`, byte
`size` and `divisor`. Attributes select their binding with `stream` (default 0).
`first_vertex` and `instance` default to zero. Observed outputs must correspond
to these records in order; an indexed post-VS capture must be exported with its
actual vertex mapping. No implicit index remapping is performed.

## References and interpretation

[Envytools' Maxwell documentation](https://envytools.readthedocs.io/en/latest/hw/graph/maxwell/cuda/index.html)
and [NVIDIA's Maxwell/Pascal instruction reference](https://docs.nvidia.com/cuda/archive/12.9.0/cuda-binary-utilities/index.html#maxwell-and-pascal-instruction-set)
describe the instruction families used here. The independent checks exercise
their behavior, rather than trusting translated instruction names alone.
[Mesa documents separate encoding and hardware behavior tests](https://docs.mesa3d.org/drivers/nvk/external_hardware_docs.html).
That distinction matters here too: disassembling a shader or validating its
SPIR-V does not establish that its output matches Maxwell.

A correct calculation can still produce an exploded mesh from corrupt inputs.
A passing position comparison with huge or nonfinite input coordinates narrows
the investigation to data, bindings and their producers; it does not repair or
certify those inputs. Frozen captures help reproduce the arithmetic without
reopening the game, but cannot reconstruct missing writes from earlier frames.
