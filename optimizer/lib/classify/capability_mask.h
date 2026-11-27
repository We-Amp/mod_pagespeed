// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CLASSIFY_CAPABILITY_MASK_H_
#define PAGESPEED_LIB_CLASSIFY_CAPABILITY_MASK_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace pagespeed {

// 32-bit capability mask for cache variant selection.
//
// Bit layout (from CLAUDE.md):
// Bits 0-1: Image Format (00: Original, 01: WebP, 10: AVIF, 11: SVG)
// Bits 2-3: Viewport Class (00: Mobile, 01: Tablet, 10: Desktop)
// Bit 4: Pixel Density (0: 1x, 1: 2x+)
// Bit 5: Save-Data (0: off, 1: on)
// Bits 6-7: Transfer-Encoding (00: Identity, 01: Gzip, 10: Brotli, 11: Reserved)
//
// Variants stored as Cyclone alternates. AlternateId = low 8 bits of Encode().

// The request headers image variant selection reads, by selection dimension
// (PageSpeedSelector scores every dimension of the mask above):
//
//   format     <- Accept
//   viewport   <- User-Agent (the image's size class)
//   density    <- User-Agent, Sec-CH-DPR
//   save-data  <- Save-Data
//   encoding   <- Accept-Encoding: never changes an image selection, since
//                 image variants are stored identity only
//
// A response served from a selected image variant must name every one of
// them in `Vary`, or a shared cache downstream can hand one client's variant
// (a phone-size image, a Save-Data image, a format it cannot decode) to a
// client that would have been selected another one.  This is the one list
// every front end matches: the optimizer's own response builder emits it
// (lib/cache/vary_emission.cc), and a front end that builds its responses
// elsewhere declares the same value.  A dimension added to the selection is
// added here, in the same change.
inline constexpr std::string_view kImageSelectionVary =
    "Accept, Save-Data, User-Agent, Sec-CH-DPR";
// Sentinel mask values for special purposes.
// kWarmupSentinel: sent by nginx when a URL crosses the hot threshold,
// triggering the worker to generate the full format variant matrix.
// Decodes to Viewport=3 which is invalid, so FromHeaders() never produces it.
inline constexpr uint32_t kWarmupSentinel = 0xFFFFFFFE;

// kLlmsTxtSentinel: sent by nginx to request a lazy /llms.txt
// (re)build. Decodes to Viewport=3 (invalid), like the other sentinels.
inline constexpr uint32_t kLlmsTxtSentinel = 0xFFFFFFFD;

// kOriginRefreshedSentinel: sent by nginx when a freshness-driven re-fetch
// (stale verdict, issue #652) obtained refreshed origin content while stale
// optimized variants may still exist in cache.  The worker responds by
// purging the URL's variant set and clearing its dedup entries so the next
// MISS rebuilds everything from the fresh origin response.  Decodes to
// Viewport=3 (invalid), like the other sentinels.
inline constexpr uint32_t kOriginRefreshedSentinel = 0xFFFFFFFC;

// kEarlyHintsSentinel: used to store preload hints for 103 Early Hints.
inline constexpr uint32_t kEarlyHintsSentinel = 0xFFFFFFFF;

class CapabilityMask {
 public:
  // Image format capabilities
  enum class ImageFormat : uint8_t {
    kOriginal = 0,
    kWebP = 1,
    kAvif = 2,
    kSvg = 3  // Was: kJxl. Safe to repurpose: kSvg is worker-only
              // (never set from Accept headers), so no wire compatibility
              // issue with clients that advertised image/jxl.
  };

  // Viewport size classes
  enum class Viewport : uint8_t { kMobile = 0, kTablet = 1, kDesktop = 2 };

  // Pixel density
  enum class PixelDensity : uint8_t { k1x = 0, k2xPlus = 1 };

  // Save-Data preference
  enum class SaveData : uint8_t { kOff = 0, kOn = 1 };

  // Transfer encoding for pre-compressed cache alternates
  enum class TransferEncoding : uint8_t {
    kIdentity = 0,
    kGzip = 1,
    kBrotli = 2,
    kReserved = 3
  };

  // Default constructor - creates mask with default values
  CapabilityMask();

  // Construct from individual capabilities
  CapabilityMask(ImageFormat image_format, Viewport viewport,
                 PixelDensity density, SaveData save_data,
                 TransferEncoding encoding);

  // Parse capabilities from HTTP headers
  // - accept: Accept header (for image format detection)
  // - user_agent: User-Agent header (for viewport detection)
  // - save_data: Save-Data header value
  // - accept_encoding: Accept-Encoding header value (br > gzip > identity)
  // - sec_ch_dpr: Sec-CH-DPR header value (explicit device-pixel-ratio
  //   client hint; when parseable it wins over UA-based density detection)
  static CapabilityMask FromHeaders(std::string_view accept,
                                    std::string_view user_agent,
                                    std::string_view save_data,
                                    std::string_view accept_encoding,
                                    std::string_view sec_ch_dpr = {});

  // Encode mask as 32-bit value (big-endian for cache key prefix)
  uint32_t Encode() const;

  // Decode from 32-bit encoded value
  static CapabilityMask Decode(uint32_t encoded);

  // Accessors
  ImageFormat image_format() const { return image_format_; }
  Viewport viewport() const { return viewport_; }
  PixelDensity pixel_density() const { return pixel_density_; }
  SaveData save_data() const { return save_data_; }
  TransferEncoding transfer_encoding() const { return transfer_encoding_; }

  // Setters for building masks programmatically
  void set_image_format(ImageFormat format) { image_format_ = format; }
  void set_viewport(Viewport viewport) { viewport_ = viewport; }
  void set_pixel_density(PixelDensity density) { pixel_density_ = density; }
  void set_save_data(SaveData save_data) { save_data_ = save_data; }
  void set_transfer_encoding(TransferEncoding enc) { transfer_encoding_ = enc; }

  // Viewport width range in CSS pixels for media query evaluation.
  struct ViewportRange {
    uint32_t min_px;
    uint32_t max_px;
  };

  // Returns the CSS pixel width range for a viewport class.
  // Mobile: 0-479, Tablet: 480-1279, Desktop: 1280-65535.
  static ViewportRange ViewportWidthRange(Viewport vp);

  bool operator==(const CapabilityMask& other) const;
  bool operator!=(const CapabilityMask& other) const {
    return !(*this == other);
  }

 private:
  // Bit field positions
  static constexpr int kImageFormatShift = 0;
  static constexpr int kViewportShift = 2;
  static constexpr int kPixelDensityShift = 4;
  static constexpr int kSaveDataShift = 5;
  static constexpr int kEncodingShift = 6;

  // Bit field masks
  static constexpr uint32_t kImageFormatMask = 0x03;
  static constexpr uint32_t kViewportMask = 0x03;
  static constexpr uint32_t kPixelDensityMask = 0x01;
  static constexpr uint32_t kSaveDataMask = 0x01;
  static constexpr uint32_t kEncodingMask = 0x03;

  ImageFormat image_format_;
  Viewport viewport_;
  PixelDensity pixel_density_;
  SaveData save_data_;
  TransferEncoding transfer_encoding_;
};

// Decides whether a request's Accept header selects the Markdown copy (the
// agent_optimize same-URL negotiation) over HTML, by RFC 9110 q-values:
//
//   - `text/markdown` must be named explicitly; `*/*` and `text/*` never
//     select it, at any q;
//   - its q must be above 0 and at least the q `text/html` resolves to
//     (the most specific of `text/html`, `text/*`, `*/*`, else 0);
//   - equal q selects Markdown, whatever the order;
//   - a malformed header selects HTML: a q outside [0, 1] or with more than
//     three decimals, an unparseable media range, or `text/markdown` listed
//     twice with different q-values.
//
// Type, subtype and parameter names are case-insensitive; media-type
// parameters other than q do not change the match. Several `text/html` (or
// wildcard) ranges resolve to their highest q, which can only favour HTML.
// The vectors in test/lib/classify/testdata/accept_markdown_vectors.txt are
// the specification.
//
// Feed every Accept header line of the request, in order, to AddLine(); the
// lines combine as one comma-separated list (RFC 9110 section 5.3). No
// allocation.
class AcceptMarkdownPreference {
 public:
  void AddLine(std::string_view line);
  bool PrefersMarkdown() const;

 private:
  bool ParseElement(std::string_view line, size_t* pos);

  bool malformed_ = false;
  // q-values in thousandths; -1 = the range is not listed.
  int markdown_q_ = -1;
  int html_q_ = -1;
  int text_any_q_ = -1;
  int any_q_ = -1;
};

// AcceptMarkdownPreference over the given Accept header lines.
bool AcceptPrefersMarkdown(std::span<const std::string_view> accept_lines);

// True iff the (single, already combined) Accept value selects the Markdown
// copy under AcceptMarkdownPreference. Kept separate from
// CapabilityMask::FromHeaders because the 32-bit mask has no free bit; the
// caller ANDs this with the SharedConfig agent_optimize flag to gate the
// kAgentMarkdown variant.
bool WantsAgentMarkdown(std::string_view accept);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_CAPABILITY_MASK_H_
