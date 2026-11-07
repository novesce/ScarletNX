<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Graphics descriptor measurements

`vk_descriptors=0` selects sets and `1` buffers. `2` and the default `3` do the same: heaps, else buffers, else
sets, by device support. An unencodable heap draw retries with buffers or sets before any
draw command. Heap encodings, index tables and buffer descriptors remain immutable until their frame retires.

Build `snx-test-vulkan` first. Stop builds and other timing jobs, then use a new local output directory:

```sh
python3 tools/bench_gpu_descriptors.py --rounds 7 --sets 512 --counts 8 32 --images 0 8 \
    --indexed 0 1 16 --backends 0 1 2 --out .cache/descriptor-comparison
```

The harness first requires the renderer's pixel checks. It runs complete repeated and unique descriptor workloads
serially in shuffled paired rounds and verifies the selected backend. Indexed workloads write both complete texture
arrays, including repeated handles. Counters report encoding calls and hits, copied bytes, index bytes, memory and
frame retirement. Retirement waits participate in submission timing. The report ranks each workload separately;
neither the CPU submission interval nor a cache hit rate establishes an FPS gain.

Within one image-array write, a consecutive run with the same image view and layout resolves one immutable resource
index. Combined image/sampler runs also require the same sampler and resolve its index once. Separate image
descriptors ignore their unused sampler field. Every element still receives its complete little-endian index;
replication stores the CPU index value and never reads mapped GPU memory. Different views, layouts or consumed
samplers end the run. Scalar writes and alternating array entries retain their separate lookups.

Logical request and payload counters still include every element. For complete successful writes, each image or
sampler request is accounted for by one resolver lookup or one replica. Image driver calls plus encoding hits equal
image lookups. Sampler driver calls and hits can overlap: native encoding can be followed by a byte-interning hit,
so their sum is not a lookup count. For example, two complete 256-entry arrays containing one repeated descriptor
still report 512 image requests and 512 sampler requests, while using two lookups of each kind. Index-table bytes
still include every stored element. `payload_bytes` describes the logical descriptor payload, including replicas;
it is not a measurement of physical memory traffic. The separate cursor, index-byte and allocation counters retain
their own meanings. Reuse counts alone do not establish a timing improvement.

Measure scalar buffer controls with indexed arrays: compiler layout and the writer's register budget can change
their cost even when native encoding calls and allocated bytes stay equal. Prefer the backend measured for the
workload. An indexed-array gain does not establish a gain for every buffer-heavy draw.

For driver validation, set `vk_validation=1` on rendering checks, outside timed batches. Keep the complete log.
Validation layers add CPU work and should remain disabled for performance measurements. The standalone
`SNX_TEST_NVN_CASE=descriptor_heaps` fixture also checks heap/legacy transitions, custom border colors and view
swizzles. Device feature skips are explicit.

Khronos validation 1.4.367 has a descriptor-buffer-to-heap transition defect: its draw check reads stale legacy
slots after validating heap state and reports `08117`. Keep those messages in validation results. The
[heap invalidation rules](https://docs.vulkan.org/spec/latest/chapters/descriptorheaps.html#descriptorheaps-bind)
and the [validator's fall-through](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/2b27c8bbb93842870f33f83e6892990dfd3bcb4a/layers/core_checks/cc_drawdispatch.cpp#L1672-L1735)
describe the mismatch. When a command buffer switches from previously bound descriptor-buffer offsets to heaps,
the renderer first disturbs the legacy graphics slots with an incompatible empty-set binding. The immutable empty
set belongs to the device; ordinary heap draws add no transition command. Keep validation enabled for the transition
fixture and require both exact output and a clean validation log. The renderer suppresses no validation messages.

## Cache capacity experiments

`analyze_descriptor_cache.py` replays explicit descriptor keys across bounded cache capacities and associativities.
It uses the runtime's key hash and current replacement rules for buffer and image caches. Other associativities
are labeled candidate policies. Full keys and frame epochs determine a hit; a hash collision never becomes a hit.

```sh
python3 tools/analyze_descriptor_cache.py --trace /local/descriptor-keys.json \
    --buckets 16,64,256,1024,4096 --ways 1,2,4 --output .cache/cache-analysis.json
python3 tools/analyze_descriptor_cache.py --synthetic working_set --working-set 128 \
    --write-trace .cache/cache-trace.json --output .cache/cache-pressure.json
```

The script reports hits, misses, replacements, probes, aligned memory and choices for which neither memory nor
misses can be reduced without increasing the other. Synthetic pressure is labeled synthetic. A small cache can
save memory while increasing driver work. Optional measured hit/miss costs produce a cost model, not a new timing.
Keep the trace, its hash and the source of those costs with the experiment.

`analyze_gpu_budget.py` separately accepts measured component profiles and computes Amdahl limits and roofline
lower bounds. Exclusive costs must share a timing domain before they can be combined. Its frame comparison requires
matching workload identities, hardware and correctness evidence; component timings cannot establish a frame target.

```sh
python3 tests/bench_gpu_descriptors_test.py
python3 tests/analyze_descriptor_cache_test.py
python3 tests/analyze_gpu_budget_test.py
```

## Repeated draw state

Guest draws compare graphics push constants as bytes. An identical range needs no command; a changed range keeps
its original offset and size. The cache covers bytes 0 through 127, before heap descriptor table addresses. New
command buffers, helper graphics work and heap mode or epoch changes invalidate it. Float bits, including NaNs and
signed zeros, remain part of the comparison. Vertex bindings also retain their complete native buffer window:
handle, offset, size and applicable dynamic stride. A pipeline change forces a new vertex bind.

`SNX_TEST_NVN_CASE=push_constants` checks repeated constants, changed alpha references, masked clears and command
buffer changes. `SNX_TEST_NVN_CASE=vertex_binding_cache` checks changed vertex windows and state resets. Also run the
latter with `--vertex_input_dynamic_state=off` to check strides supplied by the binding command.

For timing, build immutable baseline and candidate binaries with identical fixtures. The large fixture warms each
phase, then measures 65,536 draws, GPU completion included. Each phase checks exact pixels after its timed interval.
Run without validation, then run the correctness cases separately with `--vk_validation=1`:

```sh
SNX_TEST_NVN_CASE=push_constants_bench python3 tools/bench_gpu_hotpaths.py \
    --baseline /local/baseline --candidate cached=/local/candidate --rounds 7 \
    --arg=-H --arg=--vk_memory=1 --arg=--vk_descriptors=2 --arg=--vk_record=1 \
    --out /local/draw-state-results
```

The replay fixture has nine CPU workloads: 1, 32 or 256 records per submit, in batches of 1, 8 or 32 submits. It
checks draw counts and the exact completion frontier. Out-of-order queue tests and shuffled completion tests also
check that command memory remains live until all earlier submissions finish.

```sh
python3 tools/bench_gpu_hotpaths.py --baseline /local/replay-baseline \
    --candidate frontier=/local/replay-candidate --arg=--bench-replay \
    --dimension=records --dimension=batch --rounds 7 --out /local/replay-results
```

These measurements describe fixed preparation workloads. Keep their results separate from game frame times.

Storage-image setup defers image transitions until all graphics stages resolve their descriptors. One combined
transition per unique surface supplies the complete graphics access scope before the draw. The
`SNX_TEST_NVN_CASE=storage_image_barriers` fixture counts actual image dependencies outside its timed batch and checks
every stored and untouched byte. `storage_image_barriers_bench` runs a larger batch.

An advanced color equation needs no fallback work when its color mask is zero or raster discard is active. The
ordinary draw still executes its applicable shader stores, depth/stencil operations, queries and transform feedback.
`SNX_TEST_NVN_CASE=inert_advanced` compares those effects and preserved color against ordinary draws.

sRGB targets retain sRGB backing images. Where supported, extended image usage permits storage access through a
compatible linear view. Their snapshots retain the requested image format and view interpretation, with separate
copies for other format reinterpretations. `SNX_TEST_NVN_CASE=srgb_crop` checks native backing formats, full and
cropped snapshots, actual copy commands, cache reuse, refreshed generations, sampled bytes and sRGB conversion.

`vk_gpu_profile=1` also reports pass-end sites and capture expansion every ten seconds. Capture totals distinguish
guest attempts from internal primitive slices and give the largest expansion for advanced blend, coverage-to-color
and mixed-sample fallbacks. These counters expose work hidden by the ordinary guest-draw total.
