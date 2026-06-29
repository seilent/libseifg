# libseifg

A Vulkan compute frame-generation library for Android / Adreno GPUs. It interpolates
intermediate frames between two real frames (e.g. 30 → 60 fps) using optical flow,
entirely on the GPU with no machine learning and no external runtime dependencies.

Target hardware: Adreno 730–750 (Snapdragon 8 Gen 1–3). Measured ~6.6 ms/frame at
1280×720 on an Adreno 740.

## How it works

Pure Vulkan compute pipeline operating on a 5-level image pyramid:

1. **Luma conversion** — RGBA → R16F luma
2. **Pyramid downsample** — 5 levels, bilinear
3. **Lucas–Kanade coarse** — gradient-based flow at the coarsest level, 7×7 Gaussian window
4. **Lucas–Kanade refine** ×4 — bilinear-upscale coarser flow and refine; the full-resolution
   level is upscale-only (flow is solved to half resolution, which roughly halves cost with no
   measurable quality loss)
5. **Flow filter** — 3×3 median; backward flow = −forward flow
6. **Occlusion** — forward/backward consistency → per-pixel confidence
7. **Warp** ×2 — bilinear warp of each input toward the target time
8. **Blend** — photometric-agreement blend: warp where the two warps agree, cross-fade where
   they disagree

Optical flow is computed **once** per real frame pair. For an N× multiplier the engine then
synthesizes N−1 interpolated frames at t = i/N (warp + blend only), so higher multipliers add
little cost on top of the shared flow.

Gradient-based (Lucas–Kanade) flow is used rather than block matching: it produces smooth,
sub-pixel, temporally stable motion without the blocky, flickering artifacts that discrete
block search exhibits on this content.

## Capabilities

- 2×, 3×, and 4× frame multiplication
- AHardwareBuffer input/output (zero-copy import)
- FP16 intermediates throughout
- 16×16 compute workgroups (matched to Adreno wave width)
- SPIR-V shaders compiled and embedded at build time

## Building

Requirements: Android NDK r28c, CMake, a Vulkan 1.3 capable Adreno device, arm64-v8a, C++20.
The only dependency is [volk](https://github.com/zeux/volk), fetched at configure time.
Shaders are compiled to SPIR-V with `glslangValidator` (`--target-env vulkan1.3`).

```sh
NDK=/path/to/android-ndk-r28c
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

To also build the on-device test harness, add `-DSEIFG_BUILD_TESTS=ON`.

The library may also be consumed directly with `add_subdirectory(libseifg)` as a static library.

## API

```cpp
namespace seifg;

void    initialize(uint64_t deviceUUID, bool hdr, float flowScale,
                   uint64_t multiplier, const ShaderLoader& loader);
int32_t createContextFromAHB(AHardwareBuffer* in0, AHardwareBuffer* in1,
                             const std::vector<AHardwareBuffer*>& outN,
                             VkExtent2D extent, VkFormat format);
void    presentContext(int32_t id, int inSemFd, const std::vector<int>& outSemFds);
void    deleteContext(int32_t id);
void    waitIdle();
void    finalize();
```

`createContextFromAHB` takes the two input frames and N−1 output buffers (one per interpolated
frame). `presentContext` runs the pipeline and fills every output buffer.

## Testing

`tests/flowtest` runs synthetic motion patterns on-device and reports per-frame accuracy,
structural smoothness, and timing. The headline metric is **discontinuity** (spurious edge
energy in the output that is not present in the source): it measures the blocky, flickering
artifacts that matter visually, independent of raw per-pixel accuracy — a smoothly-wrong warp
scores low, while block-quantized flow scores high even when its average error is small.
Patterns cover horizontal and diagonal panning, zoom, rotation, occlusion edges, a periodic
grid/fence, and a brightness fade:

```sh
adb push build/seifg_flowtest /data/local/tmp/
adb shell /data/local/tmp/seifg_flowtest <shift> <multiplier> <flowScale>
```

## Known limitations

These are inherent optical-flow cases rather than implementation bugs:

- **Periodic high-frequency patterns** (fences, grids) at motion close to their spacing — the
  aperture/periodicity ambiguity can lock flow onto the pattern.
- **Global illumination changes** (fades, flashes) — violate the brightness-constancy
  assumption and can induce spurious motion.
- **Very large motion** (> ~30 px/frame) — exceeds the pyramid's coarse-level range.

Translation, rotation, and zoom are handled cleanly; artifacts are otherwise localized to
disocclusion edges.
