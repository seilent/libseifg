# libseifg — References & Resources

## 1. What This Is

Custom frame generation engine for Android/Adreno, replacing lsfg-vk's dependency on proprietary Lossless Scaling DLL. Pure Vulkan compute, ships self-contained. Classical hierarchical optical flow (not neural), AHB I/O, FP16 throughout, targeting <12ms per interpolated frame at 1080p on Adreno 730–750.

The name: **S**eilent **E**ngine for **I**nterpolated **F**rame **G**eneration.

---

## 2. Local References (what we already have)

| Path | What it is |
|------|-----------|
| `~/pers/ace/libGameScopeVK-RE-report.md` | Full reverse-engineering of AYANEO's 54-shader frame gen (16 pipeline stages, 10 delta + 6 gamma). THE quality target to replicate. |
| `~/pers/bannerhub/` | BannerHub frame gen wiring: Java host-side control, quality presets (ECO→MAX), `gamescope.control` 10-byte IPC layout, VK_NV_optical_flow on Turnip. |
| `~/pers/GameNative/app/src/main/cpp/lsfg-vk-android/` | Vulkan plumbing to REUSE: AHB import, volk device creation, timeline semaphore sync, command buffer ring. The `framegen/` engine API surface (`createContextFromAHB`/`presentContext`/`waitIdle`) is what libseifg implements against. |
| `~/.yolo-sisyphus/handoff/libseifg-re-shader-details.md` | Exhaustive extraction of all 54 SPIR-V blobs, data flow between stages, softmax blend formula, workgroup sizes, descriptor layouts, push constant structure. |
| `~/.yolo-sisyphus/handoff/libseifg-vulkan-plumbing.md` | AHB import patterns (layer-side vs framegen-side), volk device creation, pipeline/descriptor layout, command buffer dispatch orchestration, synchronization (timeline semaphores, fences, ring buffer). |
| `~/.yolo-sisyphus/handoff/libseifg-bannerhub-reference.md` | BannerHub's VK_NV_optical_flow session lifecycle, preset→flowScale mapping, model=0 vs model=1, multiplier protocol, performance measurements (God of War on Adreno 750). |
| `~/pers/libseifg/PIPELINE.md` | Our designed 8-shader pipeline (18 dispatches), budget estimates, Adreno optimizations. |

---

## 3. Open-Source Optical Flow Implementations

### Lluvia (jadarve/lluvia)
- **URL:** https://github.com/jadarve/lluvia
- **Algorithm:** Hierarchical Lucas-Kanade optical flow, multi-resolution pyramid
- **Language:** Vulkan compute GLSL (with Lua scripting orchestration)
- **License:** Apache-2.0
- **What to take:** Pyramid construction shaders (FP16 bilinear downsample), compute dispatch patterns for hierarchical flow, GLSL optical flow kernel structure. Can use directly.

### ReshadeMotionEstimation (JakobPCoder/ReshadeMotionEstimation)
- **URL:** https://github.com/JakobPCoder/ReshadeMotionEstimation
- **Algorithm:** Hierarchical block matching with diamond search pattern
- **Language:** HLSL (ReShade)
- **License:** Check repo (likely permissive)
- **What to take:** Diamond search implementation pattern (LDSP→SDSP), multi-level refinement logic, search radius selection heuristics. Port search patterns to GLSL compute.

### CShade cFlow.fx (papadanku/CShade)
- **URL:** https://github.com/papadanku/CShade (shader: `cFlow.fx`)
- **Algorithm:** Lucas-Kanade pyramid optical flow
- **Language:** HLSL (ReShade)
- **License:** BSD-3-Clause
- **What to take:** Compact LK implementation with image gradient computation, iterative refinement at each pyramid level. Reference for gradient-based sub-pixel refinement.

### MVTools (pinterf/mvtools)
- **URL:** https://github.com/pinterf/mvtools (AviSynth plugin)
- **Algorithm:** Block matching with exhaustive/diamond/hexagonal search, sub-pixel refinement (pel=2/4), overlap, DCT-based cost
- **Language:** C++ (CPU)
- **License:** GPL-2.0
- **What to take:** Gold-standard documentation of search patterns, SAD computation, sub-pixel interpolation (half-pixel/quarter-pixel parabolic fit), block overlap strategies. The docs and algorithm descriptions are the reference, not the code.

### diwi/PixelFlow
- **URL:** https://github.com/diwi/PixelFlow
- **Algorithm:** Horn-Schunck optical flow (variational, global smoothness constraint)
- **Language:** Processing/GLSL
- **License:** MIT
- **What to take:** GLSL Horn-Schunck implementation for reference on iterative smoothness-constrained flow solvers. Useful if we ever add a global refinement pass.

### keeffEoghan/glsl-optical-flow
- **URL:** https://github.com/keeffEoghan/glsl-optical-flow
- **Algorithm:** Minimal Horn-Schunck in GLSL fragment shaders
- **Language:** GLSL
- **License:** MIT
- **What to take:** Simplest possible GLSL optical flow — useful as a sanity check reference. Not production quality but demonstrates the HS formulation in ~100 lines.

---

## 4. Open-Source Frame Interpolation / Synthesis

### Mob-FGSR (SIGGRAPH 2024)
- **URL:** Paper: "Mob-FGSR: Frame Generation and Super Resolution for Mobile Real-Time Rendering" (ACM)
- **Algorithm:** Non-neural mobile frame generation using depth-aware forward splatting, tested on Snapdragon
- **Language:** Academic (implementation details in paper)
- **License:** Academic paper — algorithm is public knowledge
- **What to take:** Depth-aware splatting strategy for mobile GPUs, performance budgets on Adreno, their approach to handling disocclusion on mobile hardware. Validates that classical (non-ML) frame gen is viable on Snapdragon at real-time rates.

### AMD FidelityFX FSR 3.1 (GPUOpen-Effects/FidelityFX-SDK)
- **URL:** https://github.com/GPUOpen-Effects/FidelityFX-SDK (optical flow in `src/components/opticalflow/`)
- **Algorithm:** Full frame interpolation pipeline: optical flow (hierarchical block matching) → forward/backward warp → blend with disocclusion handling
- **Language:** HLSL compute shaders
- **License:** MIT
- **What to take:** Complete production-quality pipeline. Pyramid construction, block matching kernels, warp shaders, blend logic. Can port HLSL→GLSL directly. Reference for descriptor layouts, push constant design, dispatch organization.

### softmax-splatting (sniklaus/softmax-splatting)
- **URL:** https://github.com/sniklaus/softmax-splatting
- **Algorithm:** Forward warping with learned softmax confidence weighting (solves the many-to-one collision problem in forward warp)
- **Language:** Python/CUDA (PyTorch)
- **License:** Custom academic
- **What to take:** The softmax splatting FORMULA — `exp(importance) * pixel / sum(exp(importance))` — is exactly what libGameScopeVK uses in gamma3 (shader #005). This is the theoretical foundation for our `seifg_blend.comp`.

### phiresky/opencl-motion-interpolation
- **URL:** https://github.com/phiresky/opencl-motion-interpolation
- **Algorithm:** Motion vector self-advection for intermediate-time interpolation, 4-tap bilinear warp
- **Language:** OpenCL
- **License:** MIT
- **What to take:** MV self-advection trick (warp the motion field itself to generate t≠0.5 interpolation), forward-backward consistency check implementation, practical GPU interpolation pipeline structure. Simple but effective.

### SVPflow (SmoothVideo Project)
- **URL:** https://www.svp-team.com/wiki/Algorithms (documentation)
- **Algorithm:** Multi-scale block matching, cover/uncover masking (algo:21), quality mask, scene-change detection
- **Language:** Proprietary (documentation is public)
- **License:** Proprietary software — but algorithm documentation is public reference
- **What to take:** Cover/uncover masking strategy (detecting newly-revealed regions that have no valid motion vector), quality mask concept (per-pixel confidence based on match cost), multi-pass refinement documentation.

### Allen Kuo — Vulkan Flow-Based Frame Interpolation
- **URL:** Medium article series on Vulkan-based optical flow frame interpolation
- **Algorithm:** Hierarchical block matching → forward-backward check → bilinear warp → confidence blend
- **Language:** Vulkan compute (pseudocode)
- **License:** Public article
- **What to take:** Complete pseudocode for a Vulkan compute frame interpolation pipeline. Forward-backward consistency check implementation. Practical considerations for Vulkan compute dispatch organization.

---

## 5. Adreno-Specific Resources

### GL_QCOM_frame_extrapolation (AMFE)
- **What:** Hardware-assisted frame EXTRAPOLATION (not interpolation). Predicts future frame from current + motion.
- **Availability:** Adreno 660+ (Snapdragon 888+)
- **API:** GLES only (`glExtrapolateTex2DQCOM`)
- **Limitation:** Extrapolation only — cannot generate intermediate frames between two known frames. Produces more artifacts than interpolation for our use case.
- **Sample:** https://github.com/SnapdragonGameStudios/adreno-gpu-opengl-es-code-sample-framework/tree/main/samples/amfe_power_saving
- **Relevance:** Shows Qualcomm's approach to mobile frame gen. Their benchmark claims ~50% GPU power reduction at same FPS. But GLES-only and extrapolation-only makes it unsuitable — we need Vulkan compute interpolation.

### VK_QCOM_image_processing
- **What:** Vulkan extension exposing block-matching shader built-ins (SAD/SSD operations) as hardware-accelerated texture operations
- **Availability:** Adreno 730+ (Snapdragon 8 Gen 2+), proprietary driver only
- **Relevance:** Could dramatically accelerate our `seifg_block_match_coarse.comp` and `seifg_refine_level.comp` if available. Falls back to manual SAD computation on Turnip (which doesn't expose this). Future optimization — NOT in V1 since we target Turnip.

### GL_QCOM_motion_estimation
- **What:** Raw per-pixel motion vector generation via hardware-accelerated block matching
- **Availability:** Adreno 640+ (proprietary driver only)
- **API:** GLES only
- **Relevance:** This is what VK_NV_optical_flow reimplements in compute on Turnip. Cannot use directly (GLES, proprietary driver) but documents what Qualcomm's hardware can do.

### Turnip VK_NV_optical_flow
- **What:** Mesa/Turnip's compute-shader reimplementation of NVIDIA's optical flow API for Adreno
- **Implementation:** Pure Vulkan compute using `IMAGE_GATHER` for block matching, FP16 dot products for cost accumulation, hardware mipmap gen for pyramid
- **Per-chip dispatch:** Separate codepaths for Adreno 6xx (chip 6), 7xx (chip 7), 8xx (chip 8) — differ in wave width (64 vs 128), FP16 dot-product accumulators, texture-gather ISA
- **Used by:** libGameScopeVK (BannerHub) — calls `vkCmdOpticalFlowExecuteNV` to get initial motion vectors, then feeds to delta/gamma pipeline
- **Relevance for libseifg:** We do NOT depend on this extension. Our pipeline computes its own optical flow (block matching + refinement). However, if a device has it, we could optionally use it to seed our coarsest level for better initial estimates. V2 consideration.
- **Requirement:** Custom Turnip build with the extension enabled.

---

## 6. Key Algorithms to Implement

### Hierarchical Pyramid (bilinear FP16 downsample)
5 levels, each 2× reduction via box filter (2×2→1 average). FP16 storage throughout. Dispatched once per level (5 dispatches total). Adreno's 2× FP16 ALU throughput makes this nearly free.
- **Best reference:** Lluvia (GLSL compute pyramid), FSR 3.1 (HLSL pyramid shader)
- **libGameScopeVK:** Shader #004 (203 lines), bilinear sampled input → FP16 storage output

### Block Matching ME (diamond + exhaustive)
16×16 blocks, SAD cost function, diamond search pattern (LDSP 9-point → SDSP 5-point, 3 iterations). At coarsest pyramid level (1/16 res), effective reach ±192px at full resolution. Shared memory parallel reduction for SAD.
- **Best reference:** ReshadeMotionEstimation (diamond search), MVTools (search pattern docs)
- **libGameScopeVK:** Shader #001 (1360 lines), shared uint[256] for SAD reduction, workgroup 16×16

### Sub-pixel Refinement
Half-pixel parabolic fit at finest level (level 0 only). Evaluate SAD at 3 positions along each axis, fit parabola, take minimum. Adds ±0.5px precision for <1% compute cost.
- **Best reference:** MVTools pel=2/4 documentation, FSR 3.1 sub-pixel logic
- **libGameScopeVK:** Internal flow uses 2.0× scale factor (half-pixel units stored as integers)

### Forward-Backward Consistency Check
Compute forward flow (N-1→N) and backward flow (N→N-1). Warp forward flow by backward flow. If round-trip error > threshold → pixel is occluded/unreliable. Generates per-pixel occlusion mask.
- **Best reference:** Allen Kuo article (clear pseudocode), phiresky (working OpenCL implementation)
- **libGameScopeVK:** Shader #014 (187 lines, delta7/gamma2), outputs 4-channel confidence

### Forward Splatting with Softmax Confidence
Resolve many-to-one collisions when multiple source pixels warp to the same destination. Weight contributions by `exp(confidence)`, normalize by sum of weights. Produces artifact-free blending at motion boundaries.
- **Best reference:** sniklaus/softmax-splatting (the paper + formula), libGameScopeVK shader #005 (50 lines — THE core formula)
- **Formula:** `output = Σ(warped_sample * exp(confidence_i)) / Σ(exp(confidence_i))`

### MV Self-Advection for Intermediate Time
To interpolate at t≠0.5: warp the motion field itself by `t * flow` to get the flow at the intermediate timestamp. Avoids computing flow from scratch for each t value. Essential for >2× multiplier support.
- **Best reference:** phiresky/opencl-motion-interpolation (demonstrated working)
- **libGameScopeVK:** Push constant `_m1` = temporal factor t (0.5 for 2×, 0.33/0.67 for 3×)

### Cover/Uncover Masking
Detect newly-revealed regions (disocclusion) where no valid motion vector exists. Use only the frame that actually contains the revealed content (backward warp from frame N for right-moving disocclusions). Prevents ghosting at object edges.
- **Best reference:** SVPflow algo:21 documentation (detailed strategy)
- **libGameScopeVK:** Handled by 4-candidate softmax blend — occluded candidates get low confidence → effectively masked out

### Temporal Accumulation (V2)
Blend current interpolated frame with motion-compensated reprojection of previous interpolated frame. Reduces flicker and temporal noise. Weight: high motion → trust current, low motion → trust history, disocclusion → reset history.
- **Best reference:** libGameScopeVK RE shader #018 (395 lines), TAA literature
- **Status:** NOT in V1 (simplification). V2 addition when base pipeline is stable.

---

## 7. Performance Baselines

### libGameScopeVK on Adreno 750 (BannerHub, God of War)

| Preset | flowScale | Model | FPS (from 42 real) | GPU% | Power |
|--------|-----------|-------|---------------------|------|-------|
| Clear | 0.60 | 1 | 75 | 97% | -16.9W |
| Flow | 0.40 | 0 | 80 | 85% | -14.8W |
| OFF | — | — | 42 | 86% | -17.3W |

Scaling: 1.79×–1.90× (perfect 2× impossible due to flow-pass overhead).

### lsfg-vk on Adreno 740 (GameNative, various games)
- Working at 30→60 FPS (2× multiplier) via host-side Vulkan layer
- Uses Lossless Scaling DLL's DXBC shaders (translated to SPIR-V at runtime)
- Default flowScale: 0.30 (lower than BannerHub's 0.60 default)

### AMFE Hardware Extrapolation
- Qualcomm claims ~50% GPU power reduction at same displayed FPS
- GLES-only, extrapolation-only — not directly comparable to our interpolation approach

### Target Budget for libseifg
- **<12ms** per interpolated frame at 1080p on Adreno 740
- Estimated from PIPELINE.md: 10.5ms at 1080p/flowScale=0.6, leaving 1.5ms headroom
- Must fit within 16.67ms frame budget (60Hz target) alongside the game's own render time of ~30ms (presented at 30 FPS → one real frame every 33ms, one synth frame generated in the 33ms gap)

---

## 8. What We Can Reuse From lsfg-vk

### Vulkan Device Creation (volk)
`framegen/src/core/device.cpp` — creates a separate VkDevice for compute work:
- volk meta-loader for all function pointers (`volkLoadDevice`)
- Physical device selection by UUID match (`(vendorID << 32 | deviceID)`)
- Compute queue family selection
- Required extensions: `VK_ANDROID_external_memory_android_hardware_buffer`, `VK_KHR_external_memory`, `VK_KHR_dedicated_allocation`, `VK_KHR_get_memory_requirements2`, `VK_KHR_bind_memory2`, `VK_KHR_maintenance1`, `VK_KHR_sampler_ycbcr_conversion`
- Optional: `VK_KHR_synchronization2`, `VK_KHR_timeline_semaphore`, `VK_KHR_shader_float16_int8`

### AHardwareBuffer Import Pattern
**Layer side** (game device, `src/mini/image.cpp`):
- Allocates AHB with `AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | GPU_COLOR_OUTPUT`
- Creates VkImage with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID`
- Skips `vkGetAndroidHardwareBufferPropertiesANDROID` (Vortek limitation) — uses `vkGetImageMemoryRequirements` instead
- Dedicated allocation: `VkMemoryDedicatedAllocateInfo` + `VkImportAndroidHardwareBufferInfoANDROID`

**Framegen side** (our device, `framegen/src/core/image.cpp`):
- Imports AHB pointer received from layer
- CAN call `vkGetAndroidHardwareBufferPropertiesANDROID` (host Turnip/Mesa)
- Same dedicated allocation pattern
- Images start in `VK_IMAGE_LAYOUT_GENERAL` when AHB-backed

### createContextFromAHB / presentContext / waitIdle API Surface
```cpp
int32_t createContextFromAHB(AHardwareBuffer* in0, AHardwareBuffer* in1,
    const vector<AHardwareBuffer*>& outN, VkExtent2D extent, VkFormat format);
void presentContext(int32_t id, int inSem, const vector<int>& outSem);
void waitIdle();
```
This is the interface our engine implements. Layer allocates AHBs, passes pointers, calls `presentContext` per frame, then `waitIdle` (full GPU drain — only cross-device sync available on Vortek).

### Shader Loader Interface
```cpp
void initialize(uint64_t deviceUUID, bool isHdr, float flowScale, uint64_t generationCount,
    const std::function<std::vector<uint8_t>(const std::string&)>& loader);
```
We provide our own loader callback that returns pre-compiled SPIR-V (from embedded assets or compiled-in arrays) instead of extracting from a DLL.

### Pipeline / Descriptor Set Layout Helpers
`framegen/src/core/shadermodule.cpp` — descriptor set layout from vector of `(count, VkDescriptorType)` pairs. One pipeline = one shader = one descriptor set layout. No push constants in current API (we'll add a 32-byte push constant range).

### Timeline Semaphore + Command Buffer Ring
- 8-slot ring buffer (`data.at(frameIdx % 8)`)
- Fence per slot, waited before reuse
- `ONE_TIME_SUBMIT` command buffers, fresh each frame
- Timeline semaphores for internal pipelining between command buffer submissions
- Binary semaphores for submit-to-submit chaining

### Synchronization (Android-specific)
- No OPAQUE_FD cross-device semaphores (Vortek doesn't support)
- Layer flushes game queue → `presentContext(-1, {})` → `waitIdle()`
- External queue family barriers (`VK_QUEUE_FAMILY_EXTERNAL ↔ computeFamilyIdx`) around shared AHB images

---

## 9. Legal Notes

### libGameScopeVK (AYANEO / BannerHub)
**Proprietary binary.** Cannot copy shaders, weights, or code. The reverse-engineering report (`libGameScopeVK-RE-report.md`) describes algorithms and data flow — algorithm descriptions are factual observations, not copyrightable expression. Clean-room reimplementation from algorithmic understanding is legally sound. We never look at disassembled shader code while writing our shaders.

### Lossless Scaling (2GIS)
**Proprietary DXBC shaders** embedded in a Windows PE DLL. This is WHY libseifg exists — lsfg-vk currently extracts and translates these shaders at runtime, which cannot be shipped commercially or open-sourced. libseifg replaces this dependency entirely with original GLSL compute shaders.

### Lluvia
**Apache-2.0** — can use shader code directly, modify freely, must preserve attribution. Pyramid and flow shaders are directly usable.

### AMD FidelityFX FSR 3.1
**MIT license** — can port HLSL shaders to GLSL freely. Must include MIT license text. Most permissive option for complete pipeline reference.

### softmax-splatting (sniklaus)
Custom academic license — check specific terms. The mathematical formula (`softmax(confidence) * pixel`) is not copyrightable; only their specific implementation is. We implement from the formula.

### ReshadeMotionEstimation
License unclear — check repository. Algorithm (diamond search block matching) is well-known prior art regardless. Our implementation will be original GLSL.

### Mob-FGSR
Academic paper (SIGGRAPH 2024). Algorithm is public knowledge once published. No code to license — we implement from the paper's description.

### MVTools
**GPL-2.0** — cannot incorporate code into a non-GPL project. However, the algorithm documentation and descriptions of search patterns, SAD computation, and sub-pixel refinement are factual information, not code. We reference the docs, not the implementation.

### phiresky/opencl-motion-interpolation
**MIT** — can reference and port freely.

### SVPflow
Proprietary software. Algorithm documentation on their wiki is publicly available reference material. We cite the algorithm description, not their code.

---

## 10. libGameScopeVK RE — Critical Implementation Details

### Pipeline Structure (54 shaders, 16 stages)

| Stage Group | Stages | Function |
|-------------|--------|----------|
| delta0 | Coarsest block matching (SAD, shared uint[256], 16×16 blocks) |
| delta1–5 | Hierarchical coarse-to-fine refinement (upscale MV 2×, local search) |
| delta6 | MV filtering/smoothing (median/bilateral) |
| delta7 | Occlusion detection (fwd-bwd consistency) |
| delta8 | MV upscale to full resolution |
| delta9 | Final flow output (4-component: fwd.xy + bwd.xy) |
| gamma0 | Forward warp (prev frame × flow_fwd × t) |
| gamma1 | Backward warp (next frame × flow_bwd × (1-t)) |
| gamma2 | Confidence/weight map (4 channels) |
| gamma3 | Softmax blend of 4 warp candidates |
| gamma23 | Fused gamma2+3 (bandwidth optimization) |
| gamma4 | Final composition (sharpen + tone map) |

### The Softmax Blend Formula (shader #005, 50 lines)
```glsl
vec2 warp_fwd_a = (pixel + (flow_fwd.xy * 2.0 * t)) / size;
vec2 warp_fwd_b = (pixel + (flow_fwd.zw * 2.0 * (1-t))) / size;
vec2 warp_bwd_a = (pixel + (flow_bwd.xy * 2.0 * t)) / size;
vec2 warp_bwd_b = (pixel + (flow_bwd.zw * 2.0 * (1-t))) / size;

vec4 weights = exp(vec4(conf_a, conf_b, conf_c, conf_d));
weights /= (weights.x + weights.y + weights.z + weights.w);

output = (frame_prev[warp_a] * (1-t) * w.x +
          frame_next[warp_b] * t     * w.y +
          frame_prev[warp_c] * (1-t) * w.z +
          frame_next[warp_d] * t     * w.w) / total_weight;
```

Key: 4 candidates (not 2), flow stored in half-pixel units (×2.0 at warp time), dual-hypothesis per direction (.xy and .zw).

### Universal Constants
- ALL shaders: workgroup 16×16×1
- Dispatch: `ceil(width/16) × ceil(height/16) × 1`
- Delta pipeline reads frames as raw `uint` buffer arrays (storage buffers)
- Gamma pipeline reads via `texelFetch` from combined image samplers (4 inputs)
- Gamma outputs to 4 storage images simultaneously (MRT pattern in compute)
- Push constants: `_m0` = flowScale (0.2–1.0), `_m1` = temporal t, `_m2` = reserved

### Extensions Required
- `GL_AMD_gpu_shader_half_float` — FP16 ALU
- `GL_EXT_shader_explicit_arithmetic_types_float16` — explicit float16 types
- `GL_EXT_shader_16bit_storage` — 16-bit storage read/write
- `VK_NV_optical_flow` — hardware MV seeding (optional, NOT required for our pipeline)

---

## 11. BannerHub Control Protocol

### `gamescope.control` — 10-byte mmap (LE)

| Offset | Size | Field | Range |
|--------|------|-------|-------|
| 0–1 | uint16 | FPS limit | 0 = off |
| 2 | byte | enabled | 0/1 |
| 3 | byte | NativeRenderingMode | 0=Auto, 1=Never, 2=Always |
| 4–7 | float32 | flowScale | 0.2–1.0 |
| 8 | byte | model | 0=standard, 1=clear |
| 9 | byte | multiplier | 2–4 (clamped) |

### Preset Mapping (for reference when designing our own presets)

| Preset | flowScale | model | Notes |
|--------|-----------|-------|-------|
| ECO | 0.20 | 0 | Lowest overhead |
| FLOW | 0.40 | 0 | Best FPS/watt on Adreno 750 |
| BAL | 0.60 | 0 | BannerHub default |
| BOOST | 0.80 | 0 | Stronger smoothing |
| CLEAR | 0.60 | 1 | Fewer motion artifacts |
| MAX | 0.80 | 1 | Highest quality + cost |

GameNative LSFG default: flowScale = **0.30** (between ECO and FLOW).

---

## 12. V1 vs V2 Scope

### V1 (target: working pipeline)
- 8 shaders, 18 dispatches per frame
- 5-level pyramid, diamond search (3-iter), half-pixel sub-pixel at level 0
- 2-candidate softmax blend (simplified from libGameScopeVK's 4)
- Separate warp + blend passes
- No temporal accumulation
- No Laplacian blend
- No VK_NV_optical_flow dependency
- flowScale controls search radius
- Budget: <12ms at 1080p

### V2 (after V1 validated)
- Temporal accumulation (shader #018 equivalent)
- 4-candidate blend (dual-hypothesis flow)
- Fused gamma23 (MRT, reduce bandwidth)
- Laplacian multi-band blending
- Optional VK_NV_optical_flow seeding (when available)
- model=0 vs model=1 differentiation
- >2× multiplier support (t=0.33/0.67 for 3×)
