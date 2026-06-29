# libseifg — Shader Pipeline Design

## Architecture Overview

**Input:** Two consecutive real frames (AHardwareBuffer, RGBA8_UNORM or RGB10A2_UNORM)
**Output:** One interpolated mid-frame (AHardwareBuffer, same format as input)
**Target:** Adreno 730–750 (Snapdragon 8 Gen 1–3), FP16 fast path
**Budget:** <12 ms total at 1080p (fits within 16.67 ms vsync interval for 2× at 30→60)

### High-Level Data Flow

```
Frame N-1 (prev) ──┐
                    ├──→ Pyramid Downsample (5 levels, FP16 luma)
Frame N (curr) ────┘
        │
        ▼
  [Coarse Block Match] @ level 4 (1/16 res) → initial MVs
        │
        ▼
  [Refine Level 3] → [Refine Level 2] → [Refine Level 1] → [Refine Level 0]
        │
        ▼
  [Forward-Backward Consistency + Occlusion Mask]
        │
        ▼
  [Forward Warp (prev by flow_fwd * t)] + [Backward Warp (curr by flow_bwd * (1-t))]
        │
        ▼
  [Confidence-Weighted Softmax Blend] → Interpolated Frame (AHB output)
```

### Design Principles

- Classical hierarchical optical flow (NOT neural/ML)
- All compute shaders, 16×16×1 workgroups throughout
- FP16 intermediate storage everywhere except SAD accumulation (uint16 sufficient)
- Single descriptor set per pipeline (matches lsfg-vk plumbing)
- AHardwareBuffer sharing between layer device (Vortek) and framegen device (Turnip)
- No VK_NV_optical_flow dependency (pure compute, works on any Vulkan 1.1 device)

---

## Shader List

### 1. `seifg_luma_convert.comp`

**Purpose:** Convert RGBA8/RGB10A2 input frames to FP16 luma for pyramid construction.

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0) uniform sampler2D srcFrame` (AHB image) |
| Outputs | `layout(set=0, binding=1, r16f) writeonly image2D lumaOut` |
| Push Constants | `uvec2 resolution` |
| Workgroup | 16×16×1 |
| Dispatch | ceil(W/16) × ceil(H/16) × 1 |
| Algorithm | BT.709 luma: `Y = 0.2126*R + 0.7152*G + 0.0722*B` in FP16. Reads via sampler (hardware sRGB decode if needed), writes FP16 luma. |
| Cost | ~2% budget (bandwidth-bound, tiny) |

### 2. `seifg_pyramid_downsample.comp`

**Purpose:** Generate one pyramid level by 2×2 box-filter downscale (FP16).

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0) uniform sampler2D srcLevel` (bilinear sampler) |
| Outputs | `layout(set=0, binding=1, r16f) writeonly image2D dstLevel` |
| Push Constants | `uvec2 dstResolution` |
| Workgroup | 16×16×1 |
| Dispatch | ceil(dstW/16) × ceil(dstH/16) × 1 |
| Algorithm | Each thread samples one output texel at `(x+0.5)/dstW` via bilinear sampler → hardware 2×2 averaging. One dispatch per level. 4 levels = 4 dispatches per frame (×2 frames = 8 total). |
| Cost | ~3% budget (8 tiny dispatches, pyramid shrinks fast) |

### 3. `seifg_block_match_coarse.comp`

**Purpose:** Initial motion estimation at coarsest pyramid level via exhaustive block-matching SAD.

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0, r16f) readonly image2D prevLuma4` (1/16 res), `layout(set=0, binding=1, r16f) readonly image2D currLuma4` |
| Outputs | `layout(set=0, binding=2, rg16f) writeonly image2D mvCoarse` (2-component: dx, dy) |
| Push Constants | `uvec2 resolution; float searchRadius; float flowScale` |
| Workgroup | 16×16×1 |
| Dispatch | ceil(coarseW/16) × ceil(coarseH/16) × 1 |
| Algorithm | Each workgroup = one 16×16 block in the current frame. Loads the block into shared memory (256 FP16 values). Iterates over search window (diamond pattern: 9-point diamond × 3 iterations = effective ±12 px). Computes SAD per candidate via parallel reduction in shared memory (subgroupAdd for partial sums, then shared mem for cross-subgroup). Best match = minimum SAD → output MV. `searchRadius` scaled by `flowScale` (lower flowScale = smaller search = faster). |
| Shared Memory | `shared float16_t block[16][16]` + `shared uint sadAccum[64]` (one per wave lane group) |
| Cost | ~15% budget (most expensive single shader — exhaustive search at coarse level) |

### 4. `seifg_refine_level.comp`

**Purpose:** Hierarchical MV refinement at one pyramid level (dispatched 4 times: level 3→2→1→0).

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0, rg16f) readonly image2D mvCoarser` (from previous level), `layout(set=0, binding=1, r16f) readonly image2D prevLuma` (this level), `layout(set=0, binding=2, r16f) readonly image2D currLuma` (this level) |
| Outputs | `layout(set=0, binding=3, rg16f) writeonly image2D mvRefined` |
| Push Constants | `uvec2 resolution; uint level; float flowScale` |
| Workgroup | 16×16×1 |
| Dispatch | ceil(levelW/16) × ceil(levelH/16) × 1 |
| Algorithm | Per pixel: (1) Read coarser MV, upscale ×2 (both spatial position and magnitude). (2) Small local diamond search ±2 px around inherited MV center. (3) SAD cost via `textureGather` (4 luma samples in one fetch — avoids 4 separate reads). (4) At level 0 (full-res): half-pixel sub-pixel refinement using parabolic fit on 3-point cost curve. (5) Output refined MV. Level 0 is most expensive (full-res pixels). |
| Shared Memory | `shared float16_t refBlock[20][20]` (16+4 halo for ±2 search) |
| Cost | Level 3: ~3%, Level 2: ~5%, Level 1: ~10%, Level 0: ~18% (total refinement: ~36%) |

### 5. `seifg_flow_filter.comp`

**Purpose:** Median-filter motion vectors to remove outliers + compute backward flow.

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0, rg16f) readonly image2D mvForwardRaw` |
| Outputs | `layout(set=0, binding=1, rg16f) writeonly image2D mvForward` (filtered), `layout(set=0, binding=2, rg16f) writeonly image2D mvBackward` |
| Push Constants | `uvec2 resolution` |
| Workgroup | 16×16×1 |
| Dispatch | ceil(W/16) × ceil(H/16) × 1 |
| Algorithm | (1) 3×3 weighted median filter on forward MV (rank-9 sort network in registers). (2) Backward flow estimated as `mvBackward[x + mv.x, y + mv.y] = -mv` (splatting with closest-wins via atomicMin on packed magnitude+index). Simple approximation: for v1, just negate forward flow at each pixel (`mvBackward[p] = -mvForward[p]`) which is correct for translational motion and adequate for the softmax blend to handle. |
| Cost | ~5% budget |

### 6. `seifg_occlusion.comp`

**Purpose:** Forward-backward consistency check → per-pixel occlusion/confidence mask.

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0, rg16f) readonly image2D mvForward`, `layout(set=0, binding=1, rg16f) readonly image2D mvBackward` |
| Outputs | `layout(set=0, binding=2, r16f) writeonly image2D confidence` (0=occluded, 1=consistent) |
| Push Constants | `uvec2 resolution; float threshold` |
| Workgroup | 16×16×1 |
| Dispatch | ceil(W/16) × ceil(H/16) × 1 |
| Algorithm | Per pixel p: `fwd = mvForward[p]`, `bwd = mvBackward[p + fwd]` (bilinear lookup). Round-trip error = `length(fwd + bwd)`. Confidence = `clamp(1.0 - error/threshold, 0, 1)`. Threshold default = 2.0 px. Also factor in flow magnitude: `conf *= exp(-length(fwd) * 0.05)` (large motion = lower confidence). |
| Cost | ~4% budget |

### 7. `seifg_warp.comp`

**Purpose:** Warp one source frame by motion vectors (used twice: forward + backward).

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0) uniform sampler2D srcFrame` (RGBA8 source, bilinear sampler), `layout(set=0, binding=1, rg16f) readonly image2D motionVectors` |
| Outputs | `layout(set=0, binding=2, rgba16f) writeonly image2D warpedFrame` |
| Push Constants | `uvec2 resolution; float t` (interpolation factor, 0.5 for 2×) |
| Workgroup | 16×16×1 |
| Dispatch | ceil(W/16) × ceil(H/16) × 1 |
| Algorithm | Per output pixel p: `srcCoord = (vec2(p) + mv[p] * t) / vec2(resolution)`. Sample source frame at srcCoord via bilinear sampler (hardware-filtered). For forward warp: src=prevFrame, mv=mvForward, t=0.5. For backward warp: src=currFrame, mv=-mvBackward, t=0.5 (i.e., `mv * (1-t)`). Output FP16 RGBA to preserve precision for blend stage. |
| Cost | ~5% budget (×2 dispatches = ~10% total) |

### 8. `seifg_blend.comp`

**Purpose:** Confidence-weighted blending of forward and backward warped frames → final output.

| Field | Value |
|-------|-------|
| Inputs | `layout(set=0, binding=0, rgba16f) readonly image2D warpedFwd`, `layout(set=0, binding=1, rgba16f) readonly image2D warpedBwd`, `layout(set=0, binding=2, r16f) readonly image2D confidence`, `layout(set=0, binding=3) uniform sampler2D prevFrame`, `layout(set=0, binding=4) uniform sampler2D currFrame` |
| Outputs | `layout(set=0, binding=5, rgba8) writeonly image2D outputFrame` (AHB output image) |
| Push Constants | `uvec2 resolution; float t` |
| Workgroup | 16×16×1 |
| Dispatch | ceil(W/16) × ceil(H/16) × 1 |
| Algorithm | Per pixel: `confFwd = confidence[p]`, `confBwd = confidence[p + mv*t]` (approximate — use `confidence[p]` mirrored). Softmax weights: `wFwd = exp(confFwd * 4.0)`, `wBwd = exp(confBwd * 4.0)`, normalize: `wFwd /= (wFwd + wBwd)`. Blend: `out = warpedFwd * wFwd * (1-t) + warpedBwd * wBwd * t`. In occluded regions (conf < 0.3): fallback to simple linear blend of raw `prevFrame*(1-t) + currFrame*t` at pixel p (no warp — avoids garbage in disoccluded areas). Output quantized to RGBA8. |
| Cost | ~7% budget |

---

## Pipeline Stages (Execution Order)

All dispatches are recorded into a single command buffer per frame. Barriers between stages use `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` → `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` with appropriate image memory barriers.

| # | Stage | Shader | Dispatches | Resolution | Barrier After |
|---|-------|--------|-----------|------------|---------------|
| 1 | Luma convert | `seifg_luma_convert` | 2 (prev + curr) | Full | Yes (storage→read) |
| 2 | Pyramid L1 | `seifg_pyramid_downsample` | 2 (prev + curr) | W/2 × H/2 | — |
| 3 | Pyramid L2 | `seifg_pyramid_downsample` | 2 | W/4 × H/4 | — |
| 4 | Pyramid L3 | `seifg_pyramid_downsample` | 2 | W/8 × H/8 | — |
| 5 | Pyramid L4 | `seifg_pyramid_downsample` | 2 | W/16 × H/16 | Yes (all pyramid levels readable) |
| 6 | Coarse match | `seifg_block_match_coarse` | 1 | W/16 × H/16 | Yes |
| 7 | Refine L3 | `seifg_refine_level` | 1 | W/8 × H/8 | Yes |
| 8 | Refine L2 | `seifg_refine_level` | 1 | W/4 × H/4 | Yes |
| 9 | Refine L1 | `seifg_refine_level` | 1 | W/2 × H/2 | Yes |
| 10 | Refine L0 | `seifg_refine_level` | 1 | W × H | Yes |
| 11 | Flow filter | `seifg_flow_filter` | 1 | W × H | Yes |
| 12 | Occlusion | `seifg_occlusion` | 1 | W × H | Yes |
| 13a | Forward warp | `seifg_warp` | 1 | W × H | — |
| 13b | Backward warp | `seifg_warp` | 1 | W × H | Yes (both warps done) |
| 14 | Blend | `seifg_blend` | 1 | W × H | Yes (output ready for present) |

**Total dispatches:** 18 (2 luma + 8 pyramid + 1 coarse + 4 refine + 1 filter + 1 occlusion + 2 warp + 1 blend, noting 13a/13b are independent and can run concurrently).

---

## Key Design Decisions

### Block Size: 16×16
- Matches Adreno wave64 (4 warps of 16 threads each fill one workgroup exactly)
- 256 threads = natural for parallel SAD reduction
- Aligns with libGameScopeVK (all 54 shaders use 16×16×1)

### Search Pattern: Diamond (iterative)
- 3-iteration diamond search at coarsest level (9 candidates per iteration = 27 total evaluations)
- Effective reach: ±12 pixels at 1/16 resolution = ±192 pixels at full res (covers large motion)
- Much cheaper than exhaustive (27 vs 625 evaluations for ±12 window)
- Quality comparable to exhaustive for typical game motion (smooth, non-random)

### Pyramid Levels: 5 (full, /2, /4, /8, /16)
- 4 refinement passes + 1 coarse match
- Coarsest (1/16): 120×68 at 1080p — small enough for diamond search to cover full screen motion
- `flowScale` parameter: scales search radius at each level. flowScale=0.3 → radius ×0.3 (faster, less accurate for large motion). flowScale=1.0 → full radius (highest quality)

### Sub-Pixel: Half-pixel parabolic at level 0
- 3-point parabolic interpolation along each axis at the finest level
- Adds ~5% cost to the level-0 refinement dispatch
- Eliminates staircase artifacts in slow-motion regions

### FP16 Throughout
- All intermediate images: R16F (luma), RG16F (motion vectors), RGBA16F (warped frames)
- Adreno 730+ has 2× FP16 ALU throughput vs FP32
- SAD uses integer arithmetic on quantized FP16 differences (no precision loss for 8-bit source content)

### Occlusion: Forward-Backward Consistency
- Threshold-based (default 2.0 px round-trip error)
- Smooth confidence (0–1) rather than binary mask
- Drives softmax blend weights: occluded pixels fall back to temporal average

### Blending: Softmax Confidence Weights
- `exp(conf * temperature)` with temperature=4.0 (sharper transition than raw linear blend)
- Two candidates (forward warp, backward warp) — simpler than libGameScopeVK's 4-candidate
- Occluded fallback: simple `lerp(prev, curr, t)` (no warp) — visible but correct, avoids garbage

### flowScale Parameter
- Controls: search radius at each pyramid level (multiplicative)
- flowScale=0.2 (ECO): ±2 px at coarsest, ±1 at refinement levels → <8 ms
- flowScale=0.6 (BAL): ±7 px at coarsest, ±2 at refinement → ~10 ms
- flowScale=1.0 (MAX): ±12 px at coarsest, ±4 at refinement → ~12 ms
- Also gates level-0 sub-pixel: disabled when flowScale < 0.4 (saves ~2%)

---

## Data Layout

### Image Formats

| Image | VkFormat | Resolution | Count |
|-------|----------|------------|-------|
| Input frames (AHB) | R8G8B8A8_UNORM or A2B10G10R10_UNORM | 1920×1080 | 2 (prev, curr) |
| Luma pyramid L0 | R16_SFLOAT | 1920×1080 | 2 |
| Luma pyramid L1 | R16_SFLOAT | 960×540 | 2 |
| Luma pyramid L2 | R16_SFLOAT | 480×270 | 2 |
| Luma pyramid L3 | R16_SFLOAT | 240×135 | 2 |
| Luma pyramid L4 | R16_SFLOAT | 120×68 | 2 |
| MV coarse | R16G16_SFLOAT | 120×68 | 1 |
| MV refined L3 | R16G16_SFLOAT | 240×135 | 1 |
| MV refined L2 | R16G16_SFLOAT | 480×270 | 1 |
| MV refined L1 | R16G16_SFLOAT | 960×540 | 1 |
| MV refined L0 (full) | R16G16_SFLOAT | 1920×1080 | 1 |
| MV forward (filtered) | R16G16_SFLOAT | 1920×1080 | 1 |
| MV backward | R16G16_SFLOAT | 1920×1080 | 1 |
| Confidence map | R16_SFLOAT | 1920×1080 | 1 |
| Warped forward | R16G16B16A16_SFLOAT | 1920×1080 | 1 |
| Warped backward | R16G16B16A16_SFLOAT | 1920×1080 | 1 |
| Output frame (AHB) | R8G8B8A8_UNORM | 1920×1080 | 1 |

### Memory Budget at 1080p (1920×1080)

| Category | Calculation | Size |
|----------|-------------|------|
| Luma pyramids (×2) | 2 × (2.0 + 1.0 + 0.5 + 0.25 + 0.06) MB | ~7.6 MB |
| MV intermediates (5 levels) | 0.03 + 0.13 + 0.5 + 2.0 + 7.9 MB (RG16F) | ~10.6 MB |
| MV filtered (fwd + bwd) | 2 × 7.9 MB | ~15.8 MB |
| Confidence | 3.96 MB | ~4.0 MB |
| Warped frames (×2) | 2 × 15.8 MB (RGBA16F) | ~31.7 MB |
| **Total intermediates** | | **~69.7 MB** |

Note: Input/output AHBs are allocated externally (by the layer). The 70 MB is the framegen device's working set. Acceptable for Adreno 730+ (6–12 GB shared LPDDR5).

### Push Constant Struct (Unified)

```c
struct SeifgPushConstants {
    uint32_t width;
    uint32_t height;
    float    flowScale;    // 0.2–1.0, controls search radius
    float    t;            // interpolation factor (0.5 for 2×)
    uint32_t level;        // pyramid level index (for refine shader)
    float    threshold;    // occlusion threshold (default 2.0)
    float    temperature;  // softmax temperature (default 4.0)
    uint32_t _pad;         // align to 32 bytes
};
```

Size: 32 bytes (well within Vulkan's guaranteed 128-byte push constant minimum).

---

## Adreno-Specific Optimizations

### FP16 (2× ALU Throughput)
- All luma, MV, confidence, warped-frame storage is FP16
- Use `GL_EXT_shader_explicit_arithmetic_types_float16` / `VK_KHR_shader_float16_int8`
- SAD accumulates in `float16_t` (max SAD for 16×16 block of 8-bit luma: 255×256 = 65280 — fits in FP16 range though loses precision; use `uint16_t` for accumulation, convert at end)
- Pyramid, warp, blend: pure FP16 math (2× throughput on Adreno ALU vs FP32)

### Texture Gather for 4-Tap SAD
- `textureGather(sampler, coord)` returns 4 adjacent texel values in one instruction
- In refinement shader: gather-based SAD computes cost for 2×2 sub-block in one fetch
- Reduces texture unit pressure by 4× compared to individual loads
- Requires `VK_FORMAT_R16_SFLOAT` images to be gather-capable (check `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`)

### Shared Memory for Block Match
- Coarse shader loads 16×16 reference block into shared memory once
- All 27 diamond-search candidates reuse the cached block (no redundant global reads)
- Shared memory per workgroup: 256 × 2 bytes (FP16) = 512 bytes + SAD accumulators
- Adreno 730: 32 KB shared memory per workgroup → ample

### Wave64 Subgroup Operations
- Adreno 730+ subgroupSize = 64 (wave64)
- `subgroupAdd(sadPartial)` for intra-wave parallel reduction (no shared memory needed for 64-thread partial sums)
- `subgroupMin(cost)` + `subgroupShuffle` to find best match within a wave
- Only need shared memory for cross-wave communication when workgroup > 64 threads (our 256-thread workgroup = 4 waves → one shared-memory reduction step)

### Additional Adreno Considerations
- **Avoid bank conflicts:** shared memory stride = 17 instead of 16 for 16-wide access patterns (padding trick)
- **Prefer image loads over buffer loads:** Adreno's texture cache is larger and better-suited for 2D locality
- **Minimize barriers:** batch independent dispatches (13a + 13b warps have no data dependency → no barrier between them)
- **Register pressure:** FP16 halves register usage → higher occupancy → better latency hiding

---

## Budget Breakdown (1080p, flowScale=0.6, Adreno 740)

| Stage | Estimated ms | % of 12 ms |
|-------|-------------|------------|
| Luma convert (×2) | 0.3 | 2.5% |
| Pyramid downsample (×8) | 0.4 | 3.3% |
| Coarse block match | 1.8 | 15.0% |
| Refine L3 | 0.4 | 3.3% |
| Refine L2 | 0.6 | 5.0% |
| Refine L1 | 1.2 | 10.0% |
| Refine L0 (+ sub-pixel) | 2.2 | 18.3% |
| Flow filter | 0.6 | 5.0% |
| Occlusion | 0.5 | 4.2% |
| Forward warp | 0.6 | 5.0% |
| Backward warp | 0.6 | 5.0% |
| Blend | 0.8 | 6.7% |
| Barriers + overhead | 0.5 | 4.2% |
| **Total** | **10.5** | **87.5%** |

Headroom: ~1.5 ms for driver overhead, AHB transitions, and variance.

---

## Simplifications vs libGameScopeVK (V1 Scope)

| libGameScopeVK | libseifg V1 | Rationale |
|----------------|-------------|-----------|
| 4 warp candidates (fwd.xy, fwd.zw, bwd.xy, bwd.zw) | 2 candidates (fwd, bwd) | Simpler, adequate for 2× at 30 FPS |
| 6 pyramid levels | 5 levels (1/16 coarsest) | Saves one refine pass; ±192 px range sufficient |
| VK_NV_optical_flow seeding | Pure compute (no HW extension) | Portability; Vortek can't chain the ext |
| Temporal accumulation (#018) | Omitted in V1 | Add in V2 if flicker is noticeable |
| Laplacian multi-band blend (#034) | Simple softmax blend | Add if blur is objectionable |
| Multiple 3×3 convolution passes | Single median filter on flow | Minimal smoothing, let blend handle noise |
| 'clear' model variant | Single model | Add alternate blend params later |
| MRT 4-output gamma shaders | Separate warp + blend passes | Simpler to debug; fuse in V2 if bandwidth-bound |
| Sharpening post-pass | Omitted | Source frames are already sharp; bilinear warp blur is mild |

---

## Future V2 Additions (NOT in V1 scope)

1. **Temporal accumulation:** EMA blend with motion-compensated history buffer (reduces flicker)
2. **Laplacian blend:** Multi-frequency-band blending to preserve high-freq detail
3. **4-candidate warp:** Dual-hypothesis flow (two MVs per pixel) for better occlusion handling
4. **Fused gamma23:** Merge warp + blend into single dispatch (saves one full-res read/write)
5. **3× multiplier:** Interpolate at t=0.33 and t=0.67 (two outputs per frame pair)
6. **Adaptive flowScale:** Auto-detect motion complexity per frame, adjust dynamically
