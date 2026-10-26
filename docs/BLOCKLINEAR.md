# Block linear surfaces and texture conversions on the GPU

Guest memory holds most surfaces in the GPU's block linear layout. ScarletNX moves them between that layout and the
linear rows Vulkan's buffer copies take on the GPU, and converts the formats the host can't sample as they are there
too. The CPU only copies guest memory into staging, and staging into guest memory, as whole ranges. It never takes a
GOB apart, never converts a texel, and never reads GPU memory to do either.

## Where it is used

| Path | Direction | Files |
| --- | --- | --- |
| Textures (every format, every level, layer and slice) | deswizzle, then convert when needed | `vk/texture.c` |
| ASTC textures | deswizzle, then `astc.comp` | `vk/texture.c` (`astc_rows`), `vk/astc.c` |
| Render targets reloaded after the CPU wrote their memory | deswizzle | `vk/surface.c` (`rt_reload_bl`) |
| Render targets written back to guest memory | swizzle into the readback buffer | `vk/surface.c` (`flush_record`) |
| Frames vi presents from memory (drawn on the CPU) | deswizzle, then convert to RGBA8 | `vk/renderer.c` (`cpu_frame`) |

A write back is the one place bytes come back from the GPU, and it already did before: the guest's CPU reads that
memory. The swizzle now happens before the copy, so the host copies the readback into guest memory a GOB or more at a
time (`gpu/texture/blocklinear.c`'s `bl_land`, which leaves the layout's padding as the guest had it) instead of
scattering 16 byte pieces.

## The shaders

`gpu/texture/blocklinear.comp` (body `blocklinear.glsl`, host `vk/blocklinear.c`) moves 16 byte runs. A workgroup is
two GOBs side by side, 64 invocations, and lane `k` of a GOB is its run `k` in memory order:

| Bits of `k` | Meaning | Bytes in the GOB (gpu.c's `bl_x`, `bl_yz`) |
| --- | --- | --- |
| 0 | row & 1 | `(y & 1) << 4` |
| 1 | run & 1 | `(x & 16) << 1` |
| 2, 3 | (row & 7) >> 1 | `((y & 7) >> 1) << 6` |
| 4 | run >> 1 | `(x & 32) << 3` |

so 32 lanes read or write a GOB's 512 bytes in order, and four neighboring lanes write a row's 64 bytes on the
linear side. Everything else in the offset (the block's column, row of blocks, slab of `2^bd` slices, and the GOB's
place in the block) is the same for the GOB's lanes. The host passes the shifts and strides, and a bias that places
the layout's first byte anywhere in the bound range (it wraps modulo 2^32, as the shader adds it). A move's rows may
start inside a GOB: the row relative to the first moved wraps past `rows` for the rows above it, so one unsigned
compare drops those and the ones past the end.

`gpu/texture/texconv.comp` (body `texconv.glsl`, host `vk/texconv.c`) converts linear rows: 2 byte RGBA texels, RGB32F
(A becomes 1.0), packed depth and stencil (into a depth aspect and a stencil aspect, the float for
`D32_SFLOAT_S8_UINT` when D24S8 isn't supported), ETC2 and EAC blocks (`tests/support/etc2.c` is the reference), BC1
to BC7 blocks where the device lacks `textureCompressionBC` or the `native_bc` setting is 0 (`tests/support/bcn.c` is the
reference; RGBA8 out, 16 bit channels for BC4 and BC5, RGBA16F for BC6H), and vi's pixel formats. An invocation owns whole output words (a pair of 2 byte texels, a texel, four depth texels whose
stencil bytes make one word, or a block), and output rows are padded to them, so no two invocations write one word.
The copies into the image take the padding through `bufferRowLength`.

All of it is integer arithmetic, with the CPU's results to the bit. The 24 bit depth as a float is built from its
bits: `z / (2^24 - 1)` in binary is `z`'s bits repeated after the point, so from `z`'s top bit `p` the significand is
`z << (23 - p)`, followed by the zeros above `p` in the next copy and then that copy's bit `p` (a 1) with more ones
after it. That remainder is more than half an ulp and never a tie, so the nearest float is one ulp up. A GPU's
`OpFDiv` needn't be correctly rounded, so dividing would not give the CPU's bits. The test checks all 2^24 values.

### One shader each, not one per direction or format

Both directions of the block linear move are the same instructions: the offsets on both sides are computed either way,
and the direction selects which side is read. A push constant picks the direction, so a uniform select per 16 bytes
is all it costs. A second shader, or a specialization constant, would be a second pipeline to compile at startup (on
lavapipe each pipeline is an LLVM compile) and to keep in the pipeline cache. The conversions follow the same rule:
a dispatch converts one level of one format, so their branches on the mode never diverge, and one pipeline replaces
twenty. Both pipelines are made on first use, from SPIR-V the build compiles (`cmake/glsl.cmake`), and kept for the
device's lifetime like the backend's other internal pipelines.

## Batches and barriers

`vk_bl_barrier(cb, 1)` goes before a group of moves and conversions. It waits for the transfers and dispatches that
used either side's buffers (the scratch buffer is reused). `vk_bl_barrier(cb, 0)` goes after them, before the
transfers, dispatches and host reads of what they wrote. A texture's levels share one pair: `texture.c` places every
strip's linear rows and converted rows in the scratch buffer one after another. It records the moves, a barrier, the
conversions and a barrier, then copies everything into the image. It flushes the batch when the scratch buffer or its
lists are full. A strip reserves room for both its deswizzled and its converted rows before either is recorded, so the
two never land in different batches or buffers.

## Strips

A level bigger than the band (64 MiB, or the allocation cap when that is lower) goes in strips:

- the whole level when it fits;
- whole rows of blocks of `2^bd` slices, which are contiguous in memory;
- when one row of blocks doesn't fit (a very wide texture, or a small allocation cap), `2^k` GOB rows of a slice:
  each block's piece of them is contiguous in it, so they are staged one block after another, which is the layout of a
  surface of one row of blocks `2^k` GOBs high. The shader moves that like any surface.

Pitch linear levels are staged as whole rows, slices after another, when the guest's pitch suits the copy and the
conversion. Otherwise each row is copied into rows that do.

## Tests

- `blocklinear_glsl` and `texconv_glsl` run the bodies on the CPU (`tests/glsl/glsl.hpp`). The first checks moves
  both ways against `bl_offset` byte by byte, with random shapes, blocks, biases and moves starting inside a GOB, and
  that nothing outside the moved runs is written. The second checks every 2 byte texel and vi pixel, every 24 bit
  depth, RGB32F, depth and stencil rows, and random ETC2, EAC and BCn blocks of every kind against `etc2.c` and
  `bcn.c` (`tests/support/`). `bcn_decode` checks `bcn.c` itself on blocks built by hand.
- `tex_upload_rows` runs `texture.c` with a CPU model of the dispatches, which checks the barriers' order, and checks
  every copied byte against the layout. It covers each strip mode and a converted format.
- `astc_vulkan` (with `SNX_TEST_VULKAN`, on lavapipe) runs the SPIR-V: moves both ways for a range of shapes, banded
  and by slice, a swizzle landed with `bl_land` over other memory, ASTC fed from block linear blocks, and conversions.
  Where the device samples BCn it also compares the device's own decoding of random BCn blocks with `bcn.c`: BC6H and
  BC7 exactly, BC1 to BC5 within two steps, the rounding the specification leaves to the device.
  `vulkan_headless` converts packed depth and stencil on the device.
