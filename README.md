# libseifg

Frame interpolation for Adreno. ~7 ms a frame at 720p on an Adreno 740.

## Pipeline

Compute only, 6 level pyramid.

1. Luma convert: RGBA to R16F.
2. Pyramid downsample: 6 levels, bilinear.
3. LK coarse: gradient flow at the top level, 7x7 Gaussian window.
4. LK refine (x5): upscale the coarser flow and refine. Full res is upscale only, flow at half res.
5. Flow filter: 3x3 median. Backward flow is the negated forward.
6. Occlusion: forward/backward consistency, per pixel confidence.
7. Warp (x2): cubic with VK_EXT_filter_cubic, else bilinear.
8. Blend: each warp weighted by photometric agreement with its source.

Flow runs once per frame pair. Nx makes N-1 frames, warp and blend only.

## Capabilities

- 2x and 3x
- AHardwareBuffer in and out, zero copy
- FP16 intermediates
- 16x16 workgroups
- SPIR-V embedded at build time

## Building

NDK r28c, CMake, arm64-v8a, C++20, a Vulkan 1.3 Adreno device. Only dep is [volk](https://github.com/zeux/volk), fetched at configure. Shaders built with glslangValidator.

```sh
NDK=/path/to/android-ndk-r28c
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

-DSEIFG_BUILD_TESTS=ON for the tests. Use with add_subdirectory(libseifg) as a static lib.

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

## Testing

tests/flowtest runs synthetic motion and reports accuracy, smoothness, timing, and discontinuity (edge energy not in the source). Patterns: horizontal and diagonal pans, zoom, rotation, occlusion edges, a periodic grid, a brightness fade.

```sh
adb push build/seifg_flowtest /data/local/tmp/
adb shell /data/local/tmp/seifg_flowtest <shift> <multiplier> <quality>
```

## Known limitations

- Periodic patterns (fences, grids) near their spacing.
- Illumination changes (fades, flashes).
- Motion past ~55-60 px/frame.
