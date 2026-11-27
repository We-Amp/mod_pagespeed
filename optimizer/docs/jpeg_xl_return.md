# JPEG XL Support: Exhaustive Implementation Plan

> **ARCHIVED — Superseded by SVG auto-vectorization.** Format bits `11` in the
> capability mask were repurposed from kJxl to kSvg. See `lib/image/svg_vectorizer.h`
> and `lib/classify/capability_mask.h` for the current implementation.

## Context

Chrome 145 (released Feb 11, 2026) restores JPEG XL decoding as an origin trial,
using a Rust-based `jxl-rs` decoder. Safari has supported JXL since version 17.
Firefox has shown interest. The trajectory is clear: JXL is coming back to the web
platform.

This document is an exhaustive plan for **Option A**: expand the `ImageFormat` field
from 2 bits to 3 bits via a 16-bit `AlternateId`, add `libjxl` as a build
dependency, and integrate JXL encoding into the worker's proactive variant pipeline.

---

## 1. The AlternateId Expansion

### Current bit layout (8-bit AlternateId, fully packed)

```
Bit 0-1:  Image Format     (2 bits, 4 values: Original/WebP/AVIF/SVG)
Bit 2-3:  Viewport          (2 bits, 4 values: Mobile/Tablet/Desktop/SENTINEL)
Bit 4:    Pixel Density     (1 bit,  2 values: 1x/2x+)
Bit 5:    Save-Data         (1 bit,  2 values: off/on)
Bit 6-7:  Transfer Encoding (2 bits, 4 values: Identity/Gzip/Brotli/Reserved)
                             ────────
                             8 bits total = uint8_t AlternateId
```

Expanding format to 3 bits adds 1 bit → **9 bits total**, which overflows `uint8_t`.

### Solution: 16-bit AlternateId

Expand `AlternateId` from `uint8_t` to `uint16_t` across both Cyclone and
PageSpeed. This preserves all existing dimensions (including Save-Data) and
provides 7 reserved bits for future expansion.

### New bit layout (16-bit AlternateId, 9 bits used)

```
Bit 0-2:   Image Format     (3 bits, 8 values)
Bit 3-4:   Viewport          (2 bits, 4 values: Mobile/Tablet/Desktop/SENTINEL)
Bit 5:     Pixel Density     (1 bit,  2 values: 1x/2x+)
Bit 6:     Save-Data         (1 bit,  2 values: off/on)
Bit 7-8:   Transfer Encoding (2 bits, 4 values: Identity/Gzip/Brotli/Reserved)
Bit 9-15:  Reserved          (7 bits, for future use)
                              ────────
                              16 bits total = uint16_t AlternateId
```

### New ImageFormat enum values

```cpp
enum class ImageFormat : uint8_t {
  kOriginal = 0,  // 000
  kWebP = 1,      // 001
  kAvif = 2,      // 010
  kJxl = 3,       // 011  ← NEW
  kSvg = 4,       // 100  (was 3, moves to make room)
  // 5-7 reserved for future formats
};
```

### Sentinel space

Sentinels are identified by viewport bits = 3 (0b11). With viewport at bits 3-4:

```
IsSentinel(id) = ((id >> 3) & 0x03) == 0x03
```

Sentinel IDs use format(3 bits) + density(1 bit) + save-data(1 bit) +
encoding(2 bits) + reserved(7 bits) = 14 free bits → thousands of possible
sentinel values (currently only 7 used).

### New default mask value

`CapabilityMask()` default = Desktop/Identity/Original/1x/SaveData-off:

```
Old: viewport(10) at bits 2-3, save-data(0) at bit 5 = 0x08
New: viewport(10) at bits 3-4, save-data(0) at bit 6 = 0x0010
```

**The default mask changes from `0x08` to `0x0010`.** This is a cache-breaking
change.

---

## 2. Cyclone Library Changes (`reference/cyclone/`)

The 16-bit AlternateId requires changes to the Cyclone cache library itself. These
are the foundation that all other changes depend on.

### 2.1 `reference/cyclone/include/cyclone/alternate.hpp`

- **Line 35**: Change enum base type:
  ```cpp
  // Old: enum class AlternateId : uint8_t
  enum class AlternateId : uint16_t {
    Original = 0,
    // ... existing named values unchanged (all < 256)
  };
  ```
- **Line 30**: `kMaxAlternatesPerKey = 64` — unchanged (chain depth limit, not ID
  range)
- **Line 102** (`AlternateInfo` struct): `AlternateId id` field automatically
  widens via enum type change

### 2.2 `reference/cyclone/src/core/document.hpp`

On-disk document header layout (132 bytes). The `alternate_id` field at offset 120
is currently `uint8_t` with 3 bytes of reserved padding after it:

```
Offset 120:    alternate_id (1 byte)    ← expand to 2 bytes
Offset 121-123: reserved2[3] (3 bytes)  ← shrink to 1 byte
```

Changes:
- **Line 76**: `uint8_t alternate_id = 0;` → `uint16_t alternate_id = 0;`
- **Line 77**: `uint8_t reserved2[3] = {};` → `uint8_t reserved2[1] = {};`
- **Header size stays 132 bytes** — no layout shift for fields after this block
  (the `uint16_t` + `uint8_t[1]` + alignment = same 4 bytes as before due to the
  `uint64_t last_access` at offset 124 forcing 8-byte alignment anyway)
- **Line 86**: `kHeaderSize = 132` — unchanged
- **Line 92**: `kAlternateIdOffset = 120` — unchanged (start offset same)

**On-disk format change**: The byte at offset 121 changes from `reserved2[0]` to
the high byte of `alternate_id`. Old cache files have `0x00` there (reserved was
zero-initialized), which means old uint8_t IDs read as uint16_t will have the same
numeric value (zero-extended). **However**, the bit semantics change (positions
shifted), so v2 metadata is still unreadable. Version bump required.

### 2.3 `reference/cyclone/src/core/document.cpp`

- **Line 111-112** (serialize): `std::memcpy(ptr, &alternate_id, sizeof(...))` —
  `sizeof` automatically changes from 1 to 2. **Endianness note**: Cyclone uses
  platform-native byte order (memcpy of raw struct fields). Both x86-64 and
  ARM64 are little-endian, so this is safe for all current deployment targets.
  If cross-architecture cache sharing is ever needed, explicit `htole16()`/
  `le16toh()` conversion should be added. For now, document this as a known
  constraint.
- **Line 164-165** (deserialize): Mirror change for read.
- **Lines 225-229** (`DocumentBuilder::set_alternate_id`): Change parameter from
  `uint8_t` to `uint16_t` (or `AlternateId`).

### 2.4 `reference/cyclone/src/core/volume.cpp`

- **Line 1103**: `set_alternate_id(static_cast<uint8_t>(alternate_id))` →
  `set_alternate_id(static_cast<uint16_t>(alternate_id))`
- **Line 1699**: `static_cast<AlternateId>(doc.alternate_id)` — works
  automatically with wider type

### 2.5 `reference/cyclone/src/ram_cache/ram_cache.hpp`

- **Line 38** (`RamCacheKey`): `AlternateId alternate_id` — automatically widens
- **Lines 86-93** (hash specialization): Update cast:
  ```cpp
  // Old: auto id = static_cast<size_t>(static_cast<uint8_t>(k.alternate_id));
  auto id = static_cast<size_t>(static_cast<uint16_t>(k.alternate_id));
  ```

### 2.6 `reference/cyclone/src/core/hit_tracker.hpp`

- **Line 35** (`HitTrackingKey`): `AlternateId alternate_id` — automatically widens
- **Line 50** (hash): Update cast:
  ```cpp
  // Old: std::hash<uint8_t>{}(static_cast<uint8_t>(k.alternate_id))
  std::hash<uint16_t>{}(static_cast<uint16_t>(k.alternate_id))
  ```

### 2.7 Cyclone version bump

Increment `Document::kVersionMinor` to signal on-disk incompatibility. Old cache
files will be rejected on open (Cyclone already validates version on read).

---

## 3. PageSpeed Core Changes

### Phase 1: Bit Layout Expansion (cache-breaking)

#### 3.1 `lib/classify/capability_mask.h`

- **Lines 11-12**: Update documentation comment for new bit layout (3-bit format,
  shifted positions)
- **Lines 31-36**: Rewrite `ImageFormat` enum:
  - `kSvg` changes from 3 to 4
  - Add `kJxl = 3`
  - Add comment documenting reserved values 5-7
- **Lines 109-120**: Update all shift/mask constants:

  | Constant | Old | New |
  |----------|-----|-----|
  | `kImageFormatShift` | 0 | 0 |
  | `kImageFormatMask` | `0x03` | `0x07` |
  | `kViewportShift` | 2 | 3 |
  | `kViewportMask` | `0x03` | `0x03` |
  | `kPixelDensityShift` | 4 | 5 |
  | `kPixelDensityMask` | `0x01` | `0x01` |
  | `kSaveDataShift` | 5 | 6 |
  | `kSaveDataMask` | `0x01` | `0x01` |
  | `kEncodingShift` | 6 | 7 |
  | `kEncodingMask` | `0x03` | `0x03` |

- `Encode()` return type stays `uint32_t` (already wider than needed). Only the
  low 9 bits are used for AlternateId; full 32-bit value stored in metadata
  `full_mask` as before.

#### 3.2 `lib/classify/capability_mask.cc`

- **Lines 206-218** (`Encode` method): Update with new shift positions.
  All 5 dimensions still encoded into the same `uint32_t`, just at shifted offsets.
- **Lines 221-233** (`Decode` method): Mirror changes.
- **Lines 164-204** (`FromHeaders`): Add JXL detection in Accept header parsing:

  ```cpp
  // New priority: JXL > AVIF > WebP > Original
  if (AcceptContains(accept, "image/jxl")) {
    mask.image_format_ = ImageFormat::kJxl;
  } else if (AcceptContains(accept, "image/avif")) {
    mask.image_format_ = ImageFormat::kAvif;
  } else if (AcceptContains(accept, "image/webp")) {
    mask.image_format_ = ImageFormat::kWebP;
  } else {
    mask.image_format_ = ImageFormat::kOriginal;
  }
  ```

  **Priority rationale**: JXL has better compression than AVIF for photos and
  supports progressive decoding. If a browser sends both `image/jxl` and
  `image/avif`, prefer JXL. SVG is never set from headers (worker-only).

#### 3.3 `lib/classify/alternate_id.h`

- **Line 21**: Widen type alias:
  ```cpp
  // Old: using AlternateId = uint8_t;
  using AlternateId = uint16_t;
  ```
- **Lines 25-31**: Update helper functions:
  ```cpp
  inline constexpr AlternateId MaskToAlternateId(uint16_t mask_bits) {
    return static_cast<AlternateId>(mask_bits & 0x01FF);  // Low 9 bits
  }
  inline constexpr uint16_t AlternateIdToMask(AlternateId id) {
    return static_cast<uint16_t>(id);
  }
  ```
- **Lines 34-45**: Recalculate all `SentinelId` values. Viewport bits move from
  bits 2-3 to bits 3-4. Sentinel marker: viewport = 3 (0b11) at bits 3-4:

  | Sentinel | Old Value | New Value | Notes |
  |----------|-----------|-----------|-------|
  | `kOriginalContent` | `0x0C` | `0x0018` | viewport=3 at bits 3-4 |
  | `kEarlyHints` | `0x1C` | `0x0019` | |
  | `kWarmupRequest` | `0x2C` | `0x001A` | |
  | `kContentHash` | `0x3C` | `0x001B` | |
  | `kSubresourceManifest` | `0x4C` | `0x0038` | |
  | `kBrowserProfile` | `0x5C` | `0x0039` | |
  | `kHeadersSidecar` | `0x6C` | `0x003A` | was `kReserved2` when this was written |

  (Exact values to be computed during implementation. The key invariant: bits 3-4
  must be `0b11`. Other bits chosen to avoid colliding with valid format/density/
  save-data/encoding combinations.)

- **Lines 37**: Update `SentinelId` base type:
  ```cpp
  // Old: enum class SentinelId : uint8_t
  enum class SentinelId : uint16_t { ... };
  ```

- **Lines 48-50**: Update `IsSentinel()`:
  ```cpp
  // Old: ((id >> 2) & 0x03) == 0x03
  inline constexpr bool IsSentinel(AlternateId id) {
    return ((id >> 3) & 0x03) == 0x03;
  }
  ```
- **Lines 57-67**: Update all `static_assert` statements for new sentinel values.

#### 3.4 `lib/classify/alternate_metadata.h` / `.cc`

- **Wire format version bump**: Increment to **v3**.
  ```
  v3: [1B version=3][4B full_mask LE][1B content_type][1B flags][2B ct_len LE][ct...]
  ```
  The wire format itself doesn't change structurally — `full_mask` is already 32
  bits and accommodates the new bit positions. But the version bump signals that
  the bit *semantics* changed. The v3 reader rejects v1/v2 metadata (forces cache
  rebuild on upgrade).

#### 3.5 `lib/classify/pagespeed_selector.cc`

- **Lines 12-22**: Update all bit extraction constants to match new layout:

  | Constant | Old | New |
  |----------|-----|-----|
  | `kFormatShift` | 0 | 0 |
  | `kFormatMask` | `0x03` | `0x07` |
  | `kViewportShift` | 2 | 3 |
  | `kDensityShift` | 4 | 5 |
  | `kSaveDataShift` | 5 | 6 |
  | `kEncodingShift` | 6 | 7 |

- **Line 25**: `constexpr int kSvgFormat = 3;` → `constexpr int kSvgFormat = 4;`
- **Lines 45-103** (`ScoreAlternate`):
  - All `ExtractXxx()` helpers automatically pick up new shifts
  - Add JXL to scoring: same as WebP/AVIF (exact match +1000, no special bonus).
    JXL is a standard raster format — no universal bonus like SVG.
  - Save-Data scoring unchanged (+20 for match, +50 for SVG with save-data=on)
- **Line 125**: `auto alt_byte = static_cast<uint8_t>(alt.id);` → widen:
  ```cpp
  auto alt_bits = static_cast<uint16_t>(alt.id);
  ```
- **Line 135**: `AlternateIdToMask(alt_byte)` → `AlternateIdToMask(alt_bits)`

#### 3.6 `lib/classify/dimension_iterator.h`

- **Lines 14, 45**: Update format array and count:
  ```cpp
  static constexpr CapabilityMask::ImageFormat kFormats[] = {
    CapabilityMask::ImageFormat::kOriginal,
    CapabilityMask::ImageFormat::kWebP,
    CapabilityMask::ImageFormat::kAvif,
    CapabilityMask::ImageFormat::kJxl,      // NEW
  };
  ```
  Count changes from `*= 3` to `*= 4`.
- Save-Data iteration unchanged (still `*= 2` when enabled).
- **New variant count**: 4 formats x 3 viewports x 2 densities x 2 save-data =
  **48** identity variants per URL (was 36).

### 3.7 `lib/pagespeed/pagespeed.h` (C API — ABI break)

- **Line 174**: `ps_write_params_t { uint8_t alternate_id; }` →
  `uint16_t alternate_id;` — adjust padding fields
- **Line 216**: `ps_alternate_info_t { uint8_t alternate_id; uint8_t _padding[7]; }`
  → `uint16_t alternate_id; uint8_t _padding[6];`
- **Line 140**: `ps_cache_read_alternate(..., uint8_t alternate_id, ...)` →
  `uint16_t alternate_id`
- **Line 190**: `ps_cache_write_sentinel(..., uint8_t sentinel_id, ...)` →
  `uint16_t sentinel_id`
- **Line 204**: `ps_cache_alternate_exists(..., uint8_t alternate_id)` →
  `uint16_t alternate_id`
- **Sentinel macros** (lines 98-102): Recalculate to new values (see 3.3)
- **Bump `PS_API_VERSION_MAJOR`**: This is an ABI-breaking change. All consumers
  (nginx module, ASP.NET middleware, external tools) must be recompiled.

### 3.8 `lib/pagespeed/pagespeed.cc` (C API implementation)

- **Line 298**: `static_cast<AlternateId>(alternate_id)` — works with uint16_t
- **Line 413**: `static_cast<AlternateId>(params->alternate_id)` — works
- **Line 540**: `static_cast<AlternateId>(alternate_id)` — works
- **Line 597**: `arr[i].alternate_id = static_cast<uint8_t>(alts[i].id);` →
  `static_cast<uint16_t>(alts[i].id)`

### 3.9 `lib/cache/cache.cc`

- **Line 138**: `static_cast<cyclone::AlternateId>(id)` — works (both uint16_t now)
- **Line 177**: Same cast — works
- **Line 216**: Sentinel cast — works with uint16_t base
- **Line 254**: Same — works
- **Lines 108-114**: `request_metadata` encoding — `CapabilityMask::Encode()`
  returns uint32_t, packed into 4 bytes. No change needed (selector reads the full
  32-bit mask from metadata for scoring).

---

## 4. JXL Encoder Integration

### 4.1 Build dependency: `libjxl`

**WORKSPACE** — add `http_archive`:
```python
http_archive(
    name = "libjxl",
    build_file = "//third_party:libjxl.BUILD",
    sha256 = "...",
    strip_prefix = "libjxl-0.11.1",
    urls = [
        "https://github.com/libjxl/libjxl/archive/refs/tags/v0.11.1.tar.gz",
    ],
)
```

Use latest stable release. All transitive dependencies already exist:
- `@highway` (1.2.0) — SIMD library, already in WORKSPACE
- `@brotli` (1.1.0) — compression, already in WORKSPACE
- `@libpng` (1.6.40) — already in WORKSPACE
- `@libjpeg_turbo` (2.1.5.1) — already in WORKSPACE
- `@skcms` — color management, already in WORKSPACE

### 4.2 `third_party/libjxl.BUILD`

Build via `rules_foreign_cc cmake` (same pattern as libaom):

```python
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

cmake(
    name = "jxl_enc",
    cache_entries = {
        "CMAKE_BUILD_TYPE": "Release",
        "BUILD_SHARED_LIBS": "OFF",
        "BUILD_TESTING": "OFF",
        "JPEGXL_ENABLE_TOOLS": "OFF",
        "JPEGXL_ENABLE_DOXYGEN": "OFF",
        "JPEGXL_ENABLE_MANPAGES": "OFF",
        "JPEGXL_ENABLE_BENCHMARK": "OFF",
        "JPEGXL_ENABLE_EXAMPLES": "OFF",
        "JPEGXL_ENABLE_JNI": "OFF",
        "JPEGXL_ENABLE_SJPEG": "OFF",
        "JPEGXL_ENABLE_OPENEXR": "OFF",
        "JPEGXL_ENABLE_SKCMS": "ON",
        "JPEGXL_ENABLE_TCMALLOC": "OFF",
        "JPEGXL_STATIC": "ON",
        "JPEGXL_FORCE_SYSTEM_HWY": "OFF",
        "JPEGXL_FORCE_SYSTEM_BROTLI": "OFF",
    },
    lib_source = ":all_srcs",
    out_static_libs = [
        "libjxl.a",
        "libjxl_cms.a",
        "libjxl_threads.a",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@highway",
        "@brotli",
        "@skcms",
    ],
)
```

**Build time estimate**: ~120-180 seconds on first build (comparable to libaom).
Cached after first build.

**Note**: libjxl bundles its own copy of highway and brotli. The cmake flags
`JPEGXL_FORCE_SYSTEM_*` should be tested — if they don't work cleanly with our
Bazel-managed versions, let libjxl use its bundled copies (increases binary size
slightly but avoids version conflicts).

### 4.3 `lib/image/BUILD`

Add JXL library target:

```python
cc_library(
    name = "jxl",
    srcs = ["jxl_encoder.cc"],
    hdrs = ["jxl_encoder.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        ":scanline_interface",
        ":scanline_status",
        "//lib/base:message_handler",
        "@libjxl",
    ],
)
```

Update image transcoder deps in `src/worker/BUILD` to include `//lib/image:jxl`.

### 4.4 `lib/image/jxl_encoder.h` / `jxl_encoder.cc` (NEW FILES)

Implement JXL encoding from raw pixel buffers. Pattern follows the AVIF encoder
in `image_transcoder.cc` (not scanline-based — JXL encoder wants full frame):

```cpp
// jxl_encoder.h
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

struct JxlEncodeConfig {
  float quality = 90.0f;       // 0-100 (VarDCT mode, butteraugli distance derived)
  int effort = 7;              // 1-10 (higher = slower, better compression)
  bool lossless = false;       // Modular mode, distance 0 (for PNG sources)
  bool progressive_dc = true;  // Enable progressive preview (for images > 100KB)
  bool use_container = true;   // ISOBMFF container vs raw codestream
};

struct JxlEncodeResult {
  bool success = false;
  std::string output_data;
  std::string error_message;
};

// Encode raw pixel buffer to JPEG XL.
// pixels: RGB or RGBA, 8-bit per channel, row-major, no padding.
JxlEncodeResult EncodeJxl(const uint8_t* pixels, uint32_t width,
                          uint32_t height, uint32_t num_channels,
                          const JxlEncodeConfig& config);

// Lossless JPEG-to-JXL recompression (JPEG bitstream reconstruction).
// Returns smaller output with bit-exact JPEG reconstruction capability.
JxlEncodeResult RecompressJpegToJxl(std::string_view jpeg_data);

}  // namespace pagespeed
```

Key implementation notes:
- Use `JxlEncoderCreate()` + `JxlThreadParallelRunnerCreate()` for multi-threaded
  encoding. Thread count should match libaom pattern (use available cores).
- **Quality mapping**: Use `JxlEncoderDistanceFromQuality(quality)` — NOT a
  simple linear formula. This function handles the non-linear mapping correctly
  (quality 100 → distance 0, quality 90 → distance ~1.0, quality 30 → distance
  ~6.4). Never compute distance manually.
- **Required API call sequence** for pixel-based encoding:
  1. `JxlEncoderCreate(nullptr)` — create encoder
  2. `JxlThreadParallelRunnerCreate(nullptr, num_threads)` — create thread pool
  3. `JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner)` — attach
  4. `JxlEncoderSetBasicInfo(enc, &basic_info)` — set dimensions, bit depth,
     num_channels, alpha, orientation
  5. `JxlEncoderSetColorEncoding(enc, &color_encoding)` — set sRGB color space
     (use `JxlColorEncodingSetToSRGB(&color_encoding, is_gray)`)
  6. `JxlEncoderFrameSettingsCreate(enc, nullptr)` — create frame settings
  7. `JxlEncoderSetFrameDistance(settings, distance)` — set quality
  8. `JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT,
     effort)` — set effort level
  9. `JxlEncoderAddImageFrame(settings, &pixel_format, pixels, size)` — add frame
  10. `JxlEncoderCloseInput(enc)` — signal no more frames
  11. Loop `JxlEncoderProcessOutput(enc, &next_out, &avail_out)` until
      `JXL_ENC_SUCCESS` — collect compressed output
  12. Cleanup: `JxlEncoderDestroy(enc)`, `JxlThreadParallelRunnerDestroy(runner)`
- **Content-type-driven encoding mode**: Use VarDCT mode (lossy) for JPEG
  sources and modular mode (lossless) for PNG sources. Select via
  `JxlEncoderSetFrameLossless(settings, JXL_TRUE)` with distance 0 for lossless.
  Lossless JXL compresses better than PNG in virtually all cases.
- **Progressive decoding**: Enable via
  `JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_PROGRESSIVE_DC,
  1)` — provides a low-resolution preview before full decode completes. Enable by
  default for images > 100KB.
- **JPEG recompression** (lossless JPEG→JXL, see Section 11):
  `JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE)` +
  `JxlEncoderAddJPEGFrame(settings, jpeg_data, jpeg_size)`.
- **Container format**: Use `JxlEncoderUseContainer(enc, JXL_TRUE)` for JPEG
  recompression (required for reconstruction metadata). For pixel-based encoding,
  raw codestream is sufficient but container is fine too.

### 4.5 `src/worker/image_transcoder.h`

- **`ImageTranscoderConfig`** (lines 33-86): Add JXL quality settings:
  ```cpp
  float jxl_quality = 90.0f;          // 0-100 (VarDCT mode)
  int jxl_effort = 7;                 // 1-10
  float savedata_jxl_quality = 75.0f; // Lower quality for Save-Data hint
  bool jxl_jpeg_recompression = true; // Enable lossless JPEG→JXL recompression
  bool jxl_lossless_png = true;       // Use lossless modular mode for PNG sources
  ```
- **`MultiTranscodeResult`** (lines 106-120): Add JXL field:
  ```cpp
  TranscodeResult jxl;  // NEW
  ```

### 4.6 `src/worker/image_transcoder.cc`

- **`Transcode()` switch** (lines 90-146): Add JXL case:
  ```cpp
  case CapabilityMask::ImageFormat::kJxl: {
    if (cfg.jxl_jpeg_recompression && IsJpeg(input_data)) {
      auto result = RecompressJpegToJxl(input_data);
      if (result.success) return TranscodeResult{true,
          std::move(result.output_data), "image/jxl", ""};
    }
    auto result = ConvertToJxl(input_data);
    if (result.success) return result;
    break;  // Fall through to optimize original
  }
  ```

- **`ConvertToJxl()`** (NEW): Decode input to pixels, call `EncodeJxl()`.
  Pattern matches `ConvertToAvif()` (lines 256-379).

- **`EncodeJxlFromPixels()`** (NEW): Encode from decoded pixel buffer.
  Called by `TranscodeMultiResized()`.

- **`TranscodeMultiResized()` format loop** (lines 997-1108): Add JXL case:
  ```cpp
  case CapabilityMask::ImageFormat::kJxl: {
    JxlEncodeConfig jxl_cfg;
    jxl_cfg.quality = local_config.jxl_quality;
    jxl_cfg.effort = local_config.jxl_effort;
    auto jxl = EncodeJxl(pixels, width, height, channels, jxl_cfg);
    if (jxl.success && jxl.output_data.size() >= input_data.size()) {
      jxl = {false, {}, "JXL output larger than original"};
    }
    result.jxl = TranscodeResult{jxl.success, std::move(jxl.output_data),
                                  "image/jxl", jxl.error_message};
    break;
  }
  ```

- **Content-analysis quality adjustment**: Apply content-aware quality factors to
  JXL like WebP/AVIF. Photos get higher quality, illustrations get lower effort.

---

## 5. Worker Integration

### 5.1 `src/worker/worker.cc`

- **Lines 1913-1917** (`all_formats` array): Add JXL:
  ```cpp
  const CapabilityMask::ImageFormat all_formats[] = {
    CapabilityMask::ImageFormat::kWebP,
    CapabilityMask::ImageFormat::kAvif,
    CapabilityMask::ImageFormat::kJxl,      // NEW
    CapabilityMask::ImageFormat::kOriginal,
  };
  ```

- **Lines 2113-2125** (`write_format` calls): Add JXL variant write:
  ```cpp
  write_format(multi.jxl, CapabilityMask::ImageFormat::kJxl, vp, den, sd);
  ```

- **Lines 2667-2669** (warmup `all_formats`): Same addition.
- **Lines 2796-2801** (warmup `write_warmup`): Same addition.

- **Line 95** (helper function): Update mask extraction:
  ```cpp
  // Old: MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  MaskToAlternateId(static_cast<uint16_t>(mask.Encode() & 0x01FF));
  ```
  Same pattern at lines 2055 and 2784.

- **Stats counters**: Add `jxl_generated` atomic counter.

### 5.2 `src/worker/main.cc`

Add CLI flags:
```
--jxl-quality N         JXL quality (0-100, default 90)
--jxl-effort N          JXL effort level (1-10, default 7)
--no-jxl-recompression  Disable lossless JPEG→JXL recompression
```

### 5.3 `src/worker/cache_handlers.cc`

- **Lines 26-39** (`MaskToJson`): Add JXL case:
  ```cpp
  case CapabilityMask::ImageFormat::kJxl: fmt = "jxl"; break;
  ```
- **Lines 348-360** (MIME type detection): Update hardcoded format bit checks:
  ```cpp
  uint8_t format_bits = static_cast<uint8_t>(result->metadata.full_mask) & 0x07;
  if (format_bits == 1) { content_type = "image/webp"; }
  else if (format_bits == 2) { content_type = "image/avif"; }
  else if (format_bits == 3) { content_type = "image/jxl"; }      // NEW
  else if (format_bits == 4) { content_type = "image/svg+xml"; }  // Was 3
  ```
- **Line 134**: `uint8_t alt_byte = static_cast<uint8_t>(alt.id);` →
  `uint16_t alt_bits = static_cast<uint16_t>(alt.id);`
- **Range validation** (multiple sites): Replace `if (id > 255)` / `uint8_t`
  range checks with `uint16_t`-appropriate validation (`id > 0x01FF` or use
  `IsSentinel()` checks). Hardcoded `0-255` bounds must be updated.

---

## 6. Nginx Module

### 6.1 `src/nginx/ngx_pagespeed_module.cc`

- **Lines 973-982** (MIME type for served images): Same update as cache_handlers:
  ```cpp
  uint8_t format_bits = static_cast<uint8_t>(meta.full_mask) & 0x07;
  if (format_bits == 1) { mime = "image/webp"; }
  else if (format_bits == 2) { mime = "image/avif"; }
  else if (format_bits == 3) { mime = "image/jxl"; }       // NEW
  else if (format_bits == 4) { mime = "image/svg+xml"; }   // Was 3
  ```

- **Vary header**: Already includes `Accept` for images — no change needed.

- **SVG security headers**: Guard condition changes from `format_bits == 3` to
  `format_bits == 4` (SVG moved).

- **Line 633**: Update mask extraction for default alternate write:
  ```cpp
  // Old: MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
  MaskToAlternateId(static_cast<uint16_t>(default_mask.Encode() & 0x01FF));
  ```

- **Lines 1128, 1321**: Sentinel casts — work automatically with uint16_t base.

### 6.2 `src/nginx/mime_util.cc`

Add `.jxl` extension mapping:
```cpp
{".jxl", "image/jxl"},
```

---

## 6.3 ASP.NET Core Middleware (`samples/aspnetcore/`)

The ASP.NET middleware consumes the C API via P/Invoke. The ABI-breaking
`uint8_t` → `uint16_t` change requires corresponding updates:

- **`NativeWriteParams` struct**: `byte AlternateId` → `ushort AlternateId`.
  Padding fields must be adjusted to maintain struct layout alignment.
- **`NativeAlternateInfo` struct**: Same `byte` → `ushort` change for
  `AlternateId` field, adjust padding from `[7]` to `[6]`.
- **`Constants.cs`**: All sentinel `byte` constants → `ushort` with new values
  (see Section 3.3 sentinel table).
- **P/Invoke signatures**: All methods accepting `byte alternateId` must change
  to `ushort alternateId`.
- **`PS_API_VERSION_MAJOR` check**: The middleware should validate the API
  version on init and fail fast with a clear error if linked against an old
  native library (version 1 vs 2).

### 6.4 Workbench (`tools/workbench/`)

The workbench web console displays format statistics and variant info:

- **Format display types**: Add `"jxl"` to the format enum/union type in
  TypeScript. Update format label/color mapping for the UI.
- **Stats counters**: Add `jxl_generated`, `jxl_served` to the stats display.
- **Variant inspector**: Ensure the alternate ID display handles 16-bit values
  (check for `& 0xFF` masks or `uint8` type assertions).
- **Format filter**: Add JXL to any format filter dropdowns/checkboxes.

---

## 7. Tests

### 7.1 `test/lib/classify/alternate_id_test.cc`

This test file has **7 hardcoded sentinel values** and `IsSentinel()` tests that
all break with the bit shift changes. Critical updates:

- All sentinel value assertions must be recalculated (see Section 3.3 table)
- `IsSentinel()` test must use new shift: `((id >> 3) & 0x03) == 0x03`
- `MaskToAlternateId()` tests: mask must be `& 0x01FF` (was `& 0xFF`)
- Add `static_assert` tests for new sentinel values
- Verify `AlternateIdToMask()` round-trip with 16-bit values
- Verify non-sentinel IDs with format bits 0-4 are correctly classified

### 7.2 `test/lib/classify/capability_mask_test.cc`

- **`EncodeBitLayout`**: Update all bit position assertions:
  - Format: `& 0x07` (was `& 0x03`)
  - Viewport: `>> 3` (was `>> 2`)
  - Density: `>> 5` (was `>> 4`)
  - Save-Data: `>> 6` (was `>> 5`)
  - Encoding: `>> 7` (was `>> 6`)
- **`DefaultConstruction`**: Assert default encodes to `0x0010` (was `0x08`)
- **`FromHeadersJxlMapsToOriginal`**: **REPLACE** with `FromHeadersJxlDetected`:
  ```cpp
  TEST(CapabilityMaskTest, FromHeadersJxlDetected) {
    auto mask = CapabilityMask::FromHeaders("image/jxl,image/*", "", "", "");
    EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kJxl);
  }
  ```
- **Add `FromHeadersJxlPriority`**: JXL > AVIF > WebP:
  ```cpp
  TEST(CapabilityMaskTest, FromHeadersJxlPriority) {
    auto mask = CapabilityMask::FromHeaders(
        "image/jxl,image/avif,image/webp,image/*", "", "", "");
    EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kJxl);
  }
  ```
- **`SvgNeverFromHeaders`**: Still valid (SVG = 4, never set from headers)
- **`SvgFormatBitsAreThree`**: Rename to `SvgFormatBitsAreFour`, assert kSvg == 4
- **Add `JxlFormatBitsAreThree`**: Assert kJxl == 3
- **Add `JxlEncodeDecodeRoundtrip`**: Full encode/decode cycle

### 7.3 `test/lib/classify/pagespeed_selector_test.cc`

- Update all shift/mask constants in test helpers
- Update SVG format value from 3 to 4
- **Add JXL tests**:
  - `JxlExactMatchBonus` — 1000 points
  - `JxlMismatchNoBonus` — 0 points (JXL stored, client wants AVIF)
  - `JxlOriginalFallback` — Client wants JXL, Original stored → 100
  - `JxlVsSvg` — SVG beats JXL (1200 > 1000)
- **Update `Combinatorial144NonSvgMasks`**: Expands to include JXL combinations
  (4 formats x 3 viewports x 2 densities x 2 save-data x varied encodings)

### 7.4 `test/lib/classify/dimension_iterator_test.cc`

- `AllEnabledProduces48` (was `AllEnabledProduces36`): 4 formats x 3 viewports x
  2 densities x 2 save-data = 48
- `FormatsOnlyProduces4` (was `Produces3`)
- `FormatsAndViewportsProduces12` (was `Produces9`)

### 7.5 `test/src/worker/image_transcoder_test.cc`

Add JXL conversion tests:
- `JpegToJxl` — Verify JXL container signature bytes
  (`0x0000000C 4A584C20` for ISOBMFF or `0xFF0A` for codestream)
- `PngToJxl` — PNG → JXL conversion
- `GifToJxl` — GIF → JXL conversion
- `JpegToJxlRecompression` — Lossless JPEG→JXL recompression (smaller output,
  bit-exact JPEG reconstruction)
- `SmallJpegToJxlFallback` — Output larger than input → fallback
- `JxlQualitySetting` — Different quality levels produce different sizes

### 7.6 `test/lib/cache/cache_test.cc`

- Update default mask value assertions (`0x08` → `0x0010`)
- Add JXL variant read/write test
- Test three-format selection (WebP + AVIF + JXL, correct one selected)

### 7.7 `test/src/worker/cache_handlers_test.cc`

- Update SVG format bit checks (3 → 4)
- Add JXL MIME type assertion (`format_bits == 3` → `"image/jxl"`)
- Update `uint8_t` casts to `uint16_t`

### 7.8 `test/src/worker/worker_notify_test.cc`

- Update sentinel mask assertions for new bit layout
- Update default mask values
- Add JXL variant generation test

### 7.9 Cyclone tests (`reference/cyclone/test/`)

Cyclone has its own test suite for document header serialization and cache
operations. These must also be updated:

- **Document header round-trip tests**: Verify `uint16_t alternate_id`
  serializes and deserializes correctly at offset 120-121.
- **RAM cache key tests**: Verify `RamCacheKey` hashing works with 16-bit
  `AlternateId` values > 255.
- **Alternate listing tests**: Verify `list_alternates_sync` returns correct
  16-bit IDs.
- **Version bump tests**: Verify old-version cache files are rejected on read.

### 7.10 E2E tests: `tools/e2e/test_user_stories.py`

Add test class:
```python
class TestImageJXL(TestWorkerProcessing):
    def test_jxl_negotiation(self):
        """Request image with Accept: image/jxl and verify JXL served."""
        headers = {"Accept": "image/jxl,image/*"}
        # Cold miss → poll for JXL variant
        # Verify JXL magic bytes: 0x0000000C 4A584C20 (ISOBMFF) or FF 0A
```

Add to existing proactive variant tests:
```python
def test_jxl_available_after_webp_request(self):
    """After WebP request, JXL variant is proactively generated."""
```

### 7.11 Stress tests: `tools/stress/test_proactive_variant.py`

- Update `test_image_variant_throughput`: Assert JXL in `by_format` breakdown
- Adjust format count assertions (was ">=2", now ">=3" including JXL)

---

## 8. Documentation

### 8.1 `CLAUDE.md`

- Update "32-bit Capability Bitmask" section with new bit layout (3-bit format,
  shifted positions, 16-bit AlternateId)
- Update `ImageFormat` values (add kJxl = 3, kSvg = 4)
- Add libjxl to Dependencies table
- Update "Cache Write Invariant" if needed
- Update "Known Issues": remove "JXL format slot repurposed as SVG"
- Document C API version bump

### 8.2 `MEMORY.md`

- Update CapabilityMask Gotcha section with new default mask value
- Note 16-bit AlternateId change

---

## 9. Migration Strategy

### Cache Invalidation

This is a **cache-breaking change**. Both the Cyclone document header (uint16_t
alternate_id) and PageSpeed metadata (v3 bit semantics) are incompatible with
existing data. Deployment plan:

1. **Cyclone version bump** rejects old cache files on open
2. **Metadata v3** rejects v1/v2 prefixes → cache misses → re-fetches from origin
3. Worker regenerates all variants with new bit layout
4. **No explicit cache purge needed** — stale entries naturally evict as new entries
   are written and old ones fail validation

### Rollback Plan

If the deployment needs to be rolled back:

1. **Stop nginx and worker**.
2. **Delete the cache file** — old binaries cannot read the new on-disk format
   (uint16_t alternate_id at offset 120-121). There is no backward-compatible
   read path. This is a full cache loss on rollback.
3. **Redeploy old binaries** (nginx module + worker with old C API).
4. **Restart** — cache repopulates from scratch.

The cost of rollback is a cold cache. This is acceptable because:
- Cache rebuilds organically from traffic (typically full within hours)
- The alternative (maintaining backward-compatible read/write) adds significant
  complexity to the Cyclone header parsing for a scenario that should be rare

### Rolling Deployment

Both nginx and worker must be deployed together (C API version bump). This is a
**coordinated deployment** — not a rolling upgrade:

1. Stop nginx and worker
2. Purge cache (optional but recommended for clean start)
3. Deploy new worker + nginx module (both linked against new C API)
4. Start worker, then nginx
5. Cache repopulates organically from traffic

### Configuration Flags

Add a feature flag to disable JXL encoding for gradual rollout:
```
--no-proactive-jxl-variants    Skip JXL variant generation
```

When disabled, JXL variants are not generated by the worker, but the format bit
layout is still expanded (future-proofing). Clients sending `image/jxl` fall back
to AVIF/WebP/Original via the selector's fallback scoring.

---

## 10. Performance Considerations

### Encoding Speed

JXL encoding is the slowest of the three raster formats:

| Format | Effort | Speed (1080p photo) | Compression |
|--------|--------|---------------------|-------------|
| WebP | default | ~50ms | Good |
| AVIF (libaom) | speed=6 | ~200ms | Better |
| JXL (libjxl) | effort=7 | ~400ms | Best |
| JXL (libjxl) | effort=3 | ~100ms | Good |

**Mitigation strategies**:
1. Default JXL effort=7 (good balance). Let operators tune via `--jxl-effort`.
2. JPEG recompression path is fast (~50ms) and lossless — use it for JPEG sources.
3. Worker already processes variants asynchronously; JXL doesn't block serving.
4. Process JXL last in the format loop (WebP → AVIF → JXL) so faster formats are
   available sooner.

### Variant Count Impact

| Dimension | Old | New (+JXL) |
|-----------|-----|------------|
| Formats | 3 | 4 |
| Viewports | 3 | 3 |
| Densities | 2 | 2 |
| Save-Data | 2 | 2 |
| **Identity variants** | **36** | **48** |
| + Gzip variants | +36 | +48 |
| + Brotli variants | +36 | +48 |
| + SVG (1 variant) | +1 | +1 |
| **Total max** | **109** | **145** |

Adding JXL increases total variant count by ~33%. Disk usage per URL increases
proportionally, but JXL variants are typically 20-30% smaller than AVIF variants
(better compression), partially offsetting the increase.

### Disk Space

JXL typically compresses 20-30% better than AVIF for photos, so JXL variants are
smaller than AVIF variants. For lossless JPEG recompression, JXL is ~20% smaller
than the original JPEG with zero quality loss.

### Binary Size

`libjxl` static library: ~2-3 MB (encoder + decoder + threads).
Current worker binary: ~15 MB. JXL adds ~15-20% to binary size.

---

## 11. JPEG Recompression: The Killer Feature

JXL's lossless JPEG recompression deserves special attention. It takes an existing
JPEG bitstream and re-encodes it in JXL format with:
- ~20% smaller file size
- **Bit-exact JPEG reconstruction** (the original JPEG can be recovered)
- Very fast encoding (~50ms for 1080p)
- No quality loss whatsoever

This is unique to JXL and provides immediate value even before considering lossy
JXL encoding.

### Required API call sequence for JPEG recompression

```cpp
JxlEncoder* enc = JxlEncoderCreate(nullptr);
// Thread pool (same as pixel-based path)
void* runner = JxlThreadParallelRunnerCreate(nullptr, num_threads);
JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner);

// CRITICAL: Must use container format for reconstruction metadata
JxlEncoderUseContainer(enc, JXL_TRUE);

// Enable JPEG reconstruction metadata storage
JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE);

// Create frame settings (no distance/effort needed — lossless)
JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(enc, nullptr);

// Add the raw JPEG bitstream (NOT decoded pixels)
JxlEncoderAddJPEGFrame(settings, jpeg_data, jpeg_size);

// Signal completion
JxlEncoderCloseInput(enc);

// Collect output (same loop as pixel-based path)
std::vector<uint8_t> output(64);
uint8_t* next_out = output.data();
size_t avail_out = output.size();
JxlEncoderStatus status;
while ((status = JxlEncoderProcessOutput(enc, &next_out, &avail_out))
       == JXL_ENC_NEED_MORE_OUTPUT) {
  size_t offset = next_out - output.data();
  output.resize(output.size() * 2);
  next_out = output.data() + offset;
  avail_out = output.size() - offset;
}
output.resize(next_out - output.data());

// Cleanup
JxlEncoderDestroy(enc);
JxlThreadParallelRunnerDestroy(runner);
```

### Integration in `Transcode()`

1. When target is JXL and input is JPEG:
   - Try JPEG recompression path first
   - If `JxlEncoderAddJPEGFrame()` fails (malformed JPEG, progressive JPEG not
     supported by recompression, etc.), fall back to pixel-based lossy encoding
   - If successful, return immediately (skip pixel decode + lossy encode)

2. This path should be **enabled by default** and configurable via
   `--no-jxl-recompression`.

---

## 12. Implementation Phases and Milestones

### Phase 1: Cyclone AlternateId expansion

**Files**: `reference/cyclone/` — alternate.hpp, document.hpp/cpp, volume.cpp,
ram_cache.hpp, hit_tracker.hpp.

**Deliverable**: Cyclone accepts uint16_t AlternateId. Document header reads/writes
2-byte alternate_id. Version bump rejects old cache files. All Cyclone tests pass.

**Estimated scope**: ~8 files, ~50 lines changed.

### Phase 2: PageSpeed bit layout expansion (cache-breaking, no JXL encoding yet)

**Files**: capability_mask.h/cc, alternate_id.h, alternate_metadata.h/cc,
pagespeed_selector.cc, dimension_iterator.h, pagespeed.h/cc, cache.cc, all tests.

**Deliverable**: Format bits expanded to 3, kJxl=3 exists in enum but is not
generated. kSvg moves to 4. AlternateId is uint16_t. All existing tests updated
and passing. New `FromHeadersJxlDetected` test passes (Accept parsing works).
Cache version bumped to v3. C API version bumped.

**Estimated scope**: ~25 files, ~600 lines changed.

### Phase 3: JXL encoder library

**Files**: WORKSPACE, third_party/libjxl.BUILD, lib/image/jxl_encoder.h/cc,
lib/image/BUILD.

**Deliverable**: `EncodeJxl()` and `RecompressJpegToJxl()` functions working with
unit tests. No integration with worker yet.

**Estimated scope**: ~5 new files, ~400 lines new code.

### Phase 4: Worker + nginx integration

**Files**: image_transcoder.h/cc, worker.cc, main.cc, cache_handlers.cc,
ngx_pagespeed_module.cc, mime_util.cc, worker BUILD.

**Deliverable**: Worker generates JXL variants in proactive loop. Nginx serves
JXL with `Content-Type: image/jxl`. Stats include `jxl_generated` counter.
CLI flags for quality/effort. Full format negotiation works end-to-end.

**Estimated scope**: ~10 files, ~250 lines changed.

### Phase 5: E2E validation

**Files**: test_user_stories.py, test_proactive_variant.py, stress tests.

**Deliverable**: Full E2E test passes: request with JXL Accept header → cold miss →
worker generates JXL variant → subsequent request returns JXL with correct MIME
type and magic bytes.

### Phase 6: Documentation and release

**Files**: CLAUDE.md, MEMORY.md, release notes.

**Deliverable**: All documentation updated. Feature flag `--no-proactive-jxl-variants`
documented. Migration guide for existing deployments.

---

## 13. Risks and Open Questions

### Risks

1. **libjxl cmake build complexity**: libjxl's cmake is more complex than libaom's.
   May require iterating on `third_party/libjxl.BUILD` to get all cmake flags
   right, especially cross-compilation and SIMD detection.

2. **Chrome origin trial timeline**: JXL may stay behind a flag for 6-12 months.
   Early implementation means variants are generated but rarely served. Mitigated
   by `--no-proactive-jxl-variants` flag.

3. **JXL encode speed**: At effort=7, JXL encoding is ~2x slower than AVIF. May
   need effort=3-5 default for production. Needs benchmarking.

4. **Browser Accept header fragmentation**: Safari sends `image/jxl` in Accept,
   Chrome (origin trial) may not. Need to verify actual Accept header behavior in
   Chrome 145+ with the flag enabled.

5. **Cyclone on-disk format change**: The uint16_t alternate_id change affects
   the document header. While the 3-byte reserved padding absorbs the extra byte
   without changing header size, the field layout is different. Thorough testing
   of cache read/write paths is essential.

6. **Endianness constraint**: Cyclone serializes `alternate_id` with
   platform-native byte order (`memcpy`). All current targets (x86-64, ARM64)
   are little-endian, so this is safe. Cache files cannot be shared with
   big-endian architectures. Documented, not fixed (no current need).

7. **libjxl version**: Plan specifies v0.11.1; verify latest stable release at
   implementation time. The API surface is stable post-0.10, but newer versions
   may include performance improvements or bug fixes.

### Open Questions

1. **JXL priority vs AVIF**: Should JXL be preferred over AVIF when both are in
   Accept? Plan assumes yes (JXL > AVIF > WebP) based on compression superiority,
   but this is configurable.

2. **JXL for non-photo content**: JXL excels at photos but may not beat WebP for
   screenshots/illustrations. Should content analysis influence format selection?
   (e.g., only use JXL for photos, prefer WebP for illustrations)

3. **Progressive JXL**: **RESOLVED — yes.** Enable progressive DC
   (`JXL_ENC_FRAME_SETTING_PROGRESSIVE_DC = 1`) by default for images > 100KB.
   This provides a low-resolution preview before full decode completes, improving
   perceived performance on slow connections. No additional variant needed — it's
   a property of the encoded JXL codestream.

4. **Animated JXL**: JXL supports animation (as a JPEG replacement for animated
   GIFs). Should we support animated JXL? (Scope creep risk — defer to future
   work.)

5. **Lossless JXL for PNG**: **RESOLVED — yes.** PNG sources should get lossless
   JXL (modular mode, distance 0). Use `JxlEncoderSetFrameLossless(settings,
   JXL_TRUE)` when the origin content-type is `image/png`. Lossless JXL
   compresses better than PNG in virtually all cases. This requires detecting
   the origin content-type in the transcoder and selecting the appropriate mode.
   The `JxlEncodeConfig` struct should include a `bool lossless` field.

---

## Appendix: Complete File Change Index

### Cyclone library (`reference/cyclone/`)
| File | Change |
|------|--------|
| `include/cyclone/alternate.hpp` | `AlternateId : uint16_t`, AlternateInfo |
| `src/core/document.hpp` | `uint16_t alternate_id`, shrink reserved2 |
| `src/core/document.cpp` | Serialize/deserialize 2-byte alternate_id |
| `src/core/volume.cpp` | Update casts (uint8_t → uint16_t) |
| `src/ram_cache/ram_cache.hpp` | Update hash (uint16_t cast) |
| `src/core/hit_tracker.hpp` | Update hash (uint16_t cast) |

### PageSpeed core (`lib/`)
| File | Change |
|------|--------|
| `lib/classify/capability_mask.h` | 3-bit format enum, shift constants |
| `lib/classify/capability_mask.cc` | Encode/Decode shifts, JXL in FromHeaders |
| `lib/classify/alternate_id.h` | `uint16_t` type, sentinel values, IsSentinel |
| `lib/classify/alternate_metadata.h/cc` | Version bump to v3 |
| `lib/classify/pagespeed_selector.cc` | Shift constants, SVG=4, JXL scoring |
| `lib/classify/dimension_iterator.h` | Add kJxl to format array |
| `lib/cache/cache.cc` | Cast updates (automatic) |
| `lib/pagespeed/pagespeed.h` | C API: uint16_t, version bump |
| `lib/pagespeed/pagespeed.cc` | C API impl: cast updates |

### New files
| File | Purpose |
|------|---------|
| `third_party/libjxl.BUILD` | Bazel build for libjxl (cmake) |
| `lib/image/jxl_encoder.h` | JXL encoding API |
| `lib/image/jxl_encoder.cc` | JXL encoding implementation |

### Worker (`src/worker/`)
| File | Change |
|------|--------|
| `src/worker/image_transcoder.h` | JXL config, MultiTranscodeResult.jxl |
| `src/worker/image_transcoder.cc` | JXL encode cases, ConvertToJxl |
| `src/worker/worker.cc` | all_formats array, write_format, mask extractions |
| `src/worker/main.cc` | CLI flags |
| `src/worker/cache_handlers.cc` | MIME type, MaskToJson, cast updates |
| `src/worker/BUILD` | Add //lib/image:jxl dep |

### Nginx (`src/nginx/`)
| File | Change |
|------|--------|
| `src/nginx/ngx_pagespeed_module.cc` | MIME type, mask extraction, SVG guard |
| `src/nginx/mime_util.cc` | .jxl extension |

### Build files
| File | Change |
|------|--------|
| `WORKSPACE` | Add libjxl http_archive |
| `lib/image/BUILD` | Add jxl cc_library target |

### ASP.NET middleware (`samples/aspnetcore/`)
| File | Change |
|------|--------|
| `NativeWriteParams` struct | `byte` → `ushort` AlternateId + padding |
| `NativeAlternateInfo` struct | `byte` → `ushort` AlternateId + padding |
| `Constants.cs` | Sentinel values recalculated, `byte` → `ushort` |
| P/Invoke signatures | `byte` → `ushort` parameter types |

### Workbench (`tools/workbench/`)
| File | Change |
|------|--------|
| Format types/enums | Add `"jxl"` to format union types |
| Stats display | Add `jxl_generated`, `jxl_served` counters |
| Variant inspector | Handle 16-bit AlternateId values |

### Tests
| File | Change |
|------|--------|
| `test/lib/classify/alternate_id_test.cc` | **7 sentinel values**, IsSentinel shift, 16-bit round-trip |
| `test/lib/classify/capability_mask_test.cc` | Bit positions, JXL tests |
| `test/lib/classify/pagespeed_selector_test.cc` | Shifts, SVG=4, JXL tests |
| `test/lib/classify/dimension_iterator_test.cc` | 48 variants, 4 formats |
| `test/lib/cache/cache_test.cc` | Default mask `0x08`→`0x0010`, JXL variant |
| `test/src/worker/image_transcoder_test.cc` | JXL conversion tests |
| `test/src/worker/cache_handlers_test.cc` | MIME type, cast updates, range validation |
| `test/src/worker/worker_notify_test.cc` | Sentinels, default mask, JXL gen |
| `reference/cyclone/test/` | Document header round-trip, 16-bit IDs, version bump |
| `tools/e2e/test_user_stories.py` | JXL negotiation E2E |
| `tools/stress/test_proactive_variant.py` | JXL in by_format |

### Documentation
| File | Change |
|------|--------|
| `CLAUDE.md` | Bit layout, deps, known issues |
| `docs/jpeg_xl_return.md` | This file (mark phases complete) |

**Note**: Tests contain **8+ hardcoded instances** of the old default mask value
`0x08` across worker, nginx, cache handler, and selector tests. Each must be
updated to `0x0010`. A codebase-wide search for `0x08` in test files is
essential during implementation.

**Total**: ~50 files changed, ~1500 lines modified/added.

---

## Appendix B: Expert Review Log

This plan was reviewed by 6 specialist agents on 2026-02-12. Issues found and
incorporated:

| Expert | Critical | Incorporated |
|--------|----------|--------------|
| Cache/Storage | Endianness not handled | Documented as constraint (Section 2.3), rollback plan added (Section 9) |
| Bit Layout | None (all 5 claims verified) | N/A |
| Build System | skcms missing from cmake deps | Added to `libjxl.BUILD` deps (Section 4.2) |
| API Compatibility | ASP.NET middleware not addressed | New Section 6.3 added; workbench Section 6.4 added |
| Test Coverage | `alternate_id_test.cc` not mentioned, 8+ hardcoded `0x08` values | New Section 7.1, warning note in appendix |
| Image Codec | Quality mapping wrong, missing API calls, no lossless mode | Section 4.4 rewritten with full API sequence; lossless PNG resolved (Q5); progressive DC resolved (Q3); JPEG recompression Section 11 expanded |
