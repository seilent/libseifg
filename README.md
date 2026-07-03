# libseifg

Vulkan compute frame generation for Android/Adreno. Interpolates frames between two real ones (30 to 60 fps), GPU only, no ML, no runtime deps. Targets Adreno 730-750 (Snapdragon 8 Gen 1-3), ~7 ms/frame at 1280x720 on an Adreno 740.

## Pipeline

Compute only, over a 6 level pyramid:

1. Luma convert: RGBA to R16F.
2. Pyramid downsample: 6 levels, bilinear.
3. LK coarse: gradient flow at the top level, 7x7 Gaussian window.
4. LK refine (x5): upscale the coarser flow and refine. Full res level is upscale only, flow solved at half res. Halves the cost, no quality loss.
5. Flow filter: 3x3 median. Backward flow is the negated forward flow.
6. Occlusion: forward/backward consistency, per pixel confidence.
7. Warp (x2): cubic when the device has VK_EXT_filter_cubic, bilinear otherwise.
8. Blend: photometric agreement. Each warp weighted by how well it matches its source, fading to a plain blend where they disagree.

Flow runs once per real frame pair. For an Nx multiplier the engine makes N-1 frames with warp and blend only, so higher multipliers cost little.

Lucas-Kanade, not block matching. Block search gives blocky, flickering flow on game content.

## Capabilities

- 2x, 3x, 4x multipliers
- AHardwareBuffer in and out, zero copy
- FP16 intermediates
- 16x16 workgroups (Adreno wave width)
- SPIR-V shaders embedded at build time

## Building

Needs NDK r28c, CMake, arm64-v8a, C++20, a Vulkan 1.3 Adreno device. Only dependency is [volk](https://github.com/zeux/volk), fetched at configure. Shaders compiled with `glslangValidator` (`--target-env vulkan1.3`).

```sh
NDK=/path/to/android-ndk-r28c
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Add `-DSEIFG_BUILD_TESTS=ON` for the test harness. Consumable via `add_subdirectory(libseifg)` as a static lib.

## API

```cpp
namespace seifg;

void    initialize(uint64_t deviceUUID, bool hdr, uint32_t quality,
                   uint64_t multiplier, const ShaderLoader& loader);
int32_t createContextFromAHB(AHardwareBuffer* in0, AHardwareBuffer* in1,
                             const std::vector<AHardwareBuffer*>& outN,
                             VkExtent2D extent, VkFormat format);
void    presentContext(int32_t id, int inSemFd, const std::vector<int>& outSemFds);
void    deleteContext(int32_t id);
void    waitIdle();
void    finalize();
```

`quality` is ignored. Flow runs one refine iteration per level; the old tiers looked the same. The param stays for signature compatibility. `createContextFromAHB` takes the two inputs and N-1 output buffers; `presentContext` runs the pipeline and fills them.

## Testing

`tests/flowtest` runs synthetic motion on the device and reports accuracy, smoothness, and timing. The headline metric is discontinuity: spurious edge energy in the output that isn't in the source. It catches the blocky flicker that raw per pixel error misses. Patterns cover horizontal and diagonal pans, zoom, rotation, occlusion edges, a periodic grid, and a brightness fade.

```sh
adb push build/seifg_flowtest /data/local/tmp/
adb shell /data/local/tmp/seifg_flowtest <shift> <multiplier> <quality>
```

## Known limitations

Inherent optical flow cases, not bugs:

- Periodic patterns (fences, grids) at motion near their spacing. Aperture and periodicity ambiguity locks the flow onto the pattern.
- Illumination changes (fades, flashes) break brightness constancy and fake motion.
- Motion past ~55-60 px/frame exceeds the coarsest pyramid level.

Translation, rotation, and zoom are clean. What remains is at disocclusion edges.
