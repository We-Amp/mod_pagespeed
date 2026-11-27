// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for image frame interface (ImageSpec, FrameSpec,
// MultipleFrameReader, MultipleFrameWriter).

#include "lib/image/image_frame_interface.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_status.h"

namespace pagespeed::image_compression {
namespace {

using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::ScanlineStatus;

// =============================================================================
// ImageSpec tests
// =============================================================================

TEST(ImageSpecTest, DefaultConstruction) {
  ImageSpec spec;
  EXPECT_EQ(0u, spec.width);
  EXPECT_EQ(0u, spec.height);
  EXPECT_EQ(0u, spec.num_frames);
  EXPECT_EQ(1u, spec.loop_count);
  EXPECT_TRUE(spec.use_bg_color);
  EXPECT_FALSE(spec.image_size_adjusted);

  // bg_color should be zeroed
  for (unsigned char& i : spec.bg_color) {
    EXPECT_EQ(0, i);
  }
}

TEST(ImageSpecTest, Reset) {
  ImageSpec spec;
  spec.width = 100;
  spec.height = 200;
  spec.num_frames = 5;
  spec.loop_count = 3;
  spec.bg_color[RGBA_RED] = 255;
  spec.bg_color[RGBA_GREEN] = 128;
  spec.bg_color[RGBA_BLUE] = 64;
  spec.bg_color[RGBA_ALPHA] = 200;
  spec.use_bg_color = false;
  spec.image_size_adjusted = true;

  spec.Reset();

  EXPECT_EQ(0u, spec.width);
  EXPECT_EQ(0u, spec.height);
  EXPECT_EQ(0u, spec.num_frames);
  EXPECT_EQ(1u, spec.loop_count);
  EXPECT_TRUE(spec.use_bg_color);
  EXPECT_FALSE(spec.image_size_adjusted);

  for (unsigned char& i : spec.bg_color) {
    EXPECT_EQ(0, i);
  }
}

TEST(ImageSpecTest, TruncateXIndex) {
  ImageSpec spec;
  spec.width = 100;

  EXPECT_EQ(0u, spec.TruncateXIndex(0));
  EXPECT_EQ(50u, spec.TruncateXIndex(50));
  EXPECT_EQ(100u, spec.TruncateXIndex(100));
  EXPECT_EQ(100u, spec.TruncateXIndex(150));
  EXPECT_EQ(100u, spec.TruncateXIndex(1000));
}

TEST(ImageSpecTest, TruncateYIndex) {
  ImageSpec spec;
  spec.height = 200;

  EXPECT_EQ(0u, spec.TruncateYIndex(0));
  EXPECT_EQ(100u, spec.TruncateYIndex(100));
  EXPECT_EQ(200u, spec.TruncateYIndex(200));
  EXPECT_EQ(200u, spec.TruncateYIndex(300));
  EXPECT_EQ(200u, spec.TruncateYIndex(1000));
}

TEST(ImageSpecTest, CanContainFrame) {
  ImageSpec image_spec;
  image_spec.width = 100;
  image_spec.height = 100;

  // Frame that fits exactly
  FrameSpec frame_spec;
  frame_spec.width = 100;
  frame_spec.height = 100;
  frame_spec.left = 0;
  frame_spec.top = 0;
  EXPECT_TRUE(image_spec.CanContainFrame(frame_spec));

  // Frame at offset that still fits
  frame_spec.width = 50;
  frame_spec.height = 50;
  frame_spec.left = 50;
  frame_spec.top = 50;
  EXPECT_TRUE(image_spec.CanContainFrame(frame_spec));

  // Frame that extends beyond right edge
  frame_spec.width = 60;
  frame_spec.left = 50;
  frame_spec.height = 50;
  frame_spec.top = 0;
  EXPECT_FALSE(image_spec.CanContainFrame(frame_spec));

  // Frame that extends beyond bottom edge
  frame_spec.width = 50;
  frame_spec.left = 0;
  frame_spec.height = 60;
  frame_spec.top = 50;
  EXPECT_FALSE(image_spec.CanContainFrame(frame_spec));

  // Zero-size frame at origin
  frame_spec.width = 0;
  frame_spec.height = 0;
  frame_spec.left = 0;
  frame_spec.top = 0;
  EXPECT_TRUE(image_spec.CanContainFrame(frame_spec));

  // Zero-size frame at edge
  frame_spec.left = 100;
  frame_spec.top = 100;
  EXPECT_TRUE(image_spec.CanContainFrame(frame_spec));
}

TEST(ImageSpecTest, ToString) {
  ImageSpec spec;
  spec.width = 320;
  spec.height = 240;
  spec.num_frames = 10;
  spec.loop_count = 2;
  spec.use_bg_color = true;

  std::string str = spec.ToString();
  EXPECT_NE(str.find("320"), std::string::npos);
  EXPECT_NE(str.find("240"), std::string::npos);
  EXPECT_NE(str.find("10"), std::string::npos);
  EXPECT_NE(str.find('2'), std::string::npos);
  EXPECT_NE(str.find("ON"), std::string::npos);
}

TEST(ImageSpecTest, ToStringBgColorOff) {
  ImageSpec spec;
  spec.use_bg_color = false;

  std::string str = spec.ToString();
  EXPECT_NE(str.find("OFF"), std::string::npos);
}

TEST(ImageSpecTest, Equals) {
  ImageSpec spec1;
  spec1.width = 100;
  spec1.height = 200;
  spec1.num_frames = 3;
  spec1.loop_count = 2;
  spec1.bg_color[RGBA_RED] = 255;
  spec1.use_bg_color = true;
  spec1.image_size_adjusted = false;

  ImageSpec spec2 = spec1;
  EXPECT_TRUE(spec1.Equals(spec2));
  EXPECT_TRUE(spec2.Equals(spec1));

  // Modify each field and check inequality
  spec2.width = 101;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.height = 201;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.num_frames = 4;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.loop_count = 3;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.bg_color[RGBA_RED] = 0;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.use_bg_color = false;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.image_size_adjusted = true;
  EXPECT_FALSE(spec1.Equals(spec2));
}

// =============================================================================
// FrameSpec tests
// =============================================================================

TEST(FrameSpecTest, DefaultConstruction) {
  FrameSpec spec;
  EXPECT_EQ(0u, spec.width);
  EXPECT_EQ(0u, spec.height);
  EXPECT_EQ(0u, spec.top);
  EXPECT_EQ(0u, spec.left);
  EXPECT_EQ(UNSUPPORTED, spec.pixel_format);
  EXPECT_EQ(0u, spec.duration_ms);
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, spec.disposal);
  EXPECT_FALSE(spec.hint_progressive);
}

TEST(FrameSpecTest, Reset) {
  FrameSpec spec;
  spec.width = 100;
  spec.height = 200;
  spec.top = 10;
  spec.left = 20;
  spec.pixel_format = RGBA_8888;
  spec.duration_ms = 100;
  spec.disposal = FrameSpec::DISPOSAL_BACKGROUND;
  spec.hint_progressive = true;

  spec.Reset();

  EXPECT_EQ(0u, spec.width);
  EXPECT_EQ(0u, spec.height);
  EXPECT_EQ(0u, spec.top);
  EXPECT_EQ(0u, spec.left);
  EXPECT_EQ(UNSUPPORTED, spec.pixel_format);
  EXPECT_EQ(0u, spec.duration_ms);
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, spec.disposal);
  EXPECT_FALSE(spec.hint_progressive);
}

TEST(FrameSpecTest, DisposalMethods) {
  EXPECT_EQ(0, FrameSpec::DISPOSAL_UNKNOWN);
  EXPECT_EQ(1, FrameSpec::DISPOSAL_NONE);
  EXPECT_EQ(2, FrameSpec::DISPOSAL_BACKGROUND);
  EXPECT_EQ(3, FrameSpec::DISPOSAL_RESTORE);
}

TEST(FrameSpecTest, ToString) {
  FrameSpec spec;
  spec.width = 640;
  spec.height = 480;
  spec.top = 10;
  spec.left = 20;
  spec.pixel_format = RGB_888;
  spec.duration_ms = 100;
  spec.disposal = FrameSpec::DISPOSAL_NONE;
  spec.hint_progressive = true;

  std::string str = spec.ToString();
  EXPECT_NE(str.find("640"), std::string::npos);
  EXPECT_NE(str.find("480"), std::string::npos);
  EXPECT_NE(str.find("RGB_888"), std::string::npos);
  EXPECT_NE(str.find("yes"), std::string::npos);
}

TEST(FrameSpecTest, ToStringNotProgressive) {
  FrameSpec spec;
  spec.hint_progressive = false;

  std::string str = spec.ToString();
  EXPECT_NE(str.find("no"), std::string::npos);
}

TEST(FrameSpecTest, Equals) {
  FrameSpec spec1;
  spec1.width = 100;
  spec1.height = 200;
  spec1.top = 10;
  spec1.left = 20;
  spec1.pixel_format = RGBA_8888;
  spec1.duration_ms = 50;
  spec1.disposal = FrameSpec::DISPOSAL_BACKGROUND;
  spec1.hint_progressive = true;

  FrameSpec spec2 = spec1;
  EXPECT_TRUE(spec1.Equals(spec2));
  EXPECT_TRUE(spec2.Equals(spec1));

  // Modify each field and check inequality
  spec2.width = 101;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.height = 201;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.top = 11;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.left = 21;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.pixel_format = RGB_888;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.duration_ms = 51;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.disposal = FrameSpec::DISPOSAL_RESTORE;
  EXPECT_FALSE(spec1.Equals(spec2));
  spec2 = spec1;

  spec2.hint_progressive = false;
  EXPECT_FALSE(spec1.Equals(spec2));
}

// =============================================================================
// MultipleFrameReader / MultipleFrameWriter tests
//
// Since these are abstract classes, we create minimal concrete
// implementations for testing.
// =============================================================================

class MockMultipleFrameReader : public MultipleFrameReader {
 public:
  explicit MockMultipleFrameReader(MessageHandler* handler)
      : MultipleFrameReader(handler) {}

  // Bring base class overloads into scope to avoid C++ name hiding.
  using MultipleFrameReader::GetFrameSpec;
  using MultipleFrameReader::GetImageSpec;
  using MultipleFrameReader::Initialize;
  using MultipleFrameReader::PrepareNextFrame;
  using MultipleFrameReader::ReadNextScanline;
  using MultipleFrameReader::Reset;
  using MultipleFrameReader::set_quirks_mode;

  ScanlineStatus Reset() override {
    reset_called_ = true;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus Initialize() override {
    initialize_called_ = true;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  [[nodiscard]] bool HasMoreFrames() const override { return has_more_frames_; }
  [[nodiscard]] bool HasMoreScanlines() const override {
    return has_more_scanlines_;
  }

  ScanlineStatus PrepareNextFrame() override {
    prepare_next_frame_called_ = true;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus ReadNextScanline(const void** out_scanline_bytes) override {
    *out_scanline_bytes = scanline_data_;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus GetFrameSpec(FrameSpec* frame_spec) const override {
    *frame_spec = frame_spec_;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus GetImageSpec(ImageSpec* image_spec) const override {
    *image_spec = image_spec_;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  // Test helpers
  void set_has_more_frames(bool val) { has_more_frames_ = val; }
  void set_has_more_scanlines(bool val) { has_more_scanlines_ = val; }
  [[nodiscard]] bool reset_called() const { return reset_called_; }
  [[nodiscard]] bool initialize_called() const { return initialize_called_; }
  [[nodiscard]] bool prepare_next_frame_called() const {
    return prepare_next_frame_called_;
  }
  void set_frame_spec(const FrameSpec& spec) { frame_spec_ = spec; }
  void set_image_spec(const ImageSpec& spec) { image_spec_ = spec; }

 private:
  bool has_more_frames_{false};
  bool has_more_scanlines_{false};
  bool reset_called_{false};
  bool initialize_called_{false};
  bool prepare_next_frame_called_{false};
  FrameSpec frame_spec_;
  ImageSpec image_spec_;
  uint8_t scanline_data_[1024] = {0};
};

class MockMultipleFrameWriter : public MultipleFrameWriter {
 public:
  explicit MockMultipleFrameWriter(MessageHandler* handler)
      : MultipleFrameWriter(handler) {}

  // Bring base class overloads into scope to avoid C++ name hiding.
  using MultipleFrameWriter::FinalizeWrite;
  using MultipleFrameWriter::Initialize;
  using MultipleFrameWriter::PrepareImage;
  using MultipleFrameWriter::PrepareNextFrame;
  using MultipleFrameWriter::WriteNextScanline;

  ScanlineStatus Initialize(const void* /*config*/, std::string* out) override {
    initialize_called_ = true;
    output_ = out;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus PrepareImage(const ImageSpec* image_spec) override {
    prepare_image_called_ = true;
    image_spec_ = *image_spec;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus PrepareNextFrame(const FrameSpec* frame_spec) override {
    prepare_next_frame_called_ = true;
    frame_spec_ = *frame_spec;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus WriteNextScanline(const void* /*scanline_bytes*/) override {
    write_next_scanline_called_ = true;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus FinalizeWrite() override {
    finalize_write_called_ = true;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  // Test helpers
  [[nodiscard]] bool initialize_called() const { return initialize_called_; }
  [[nodiscard]] bool prepare_image_called() const {
    return prepare_image_called_;
  }
  [[nodiscard]] bool prepare_next_frame_called() const {
    return prepare_next_frame_called_;
  }
  [[nodiscard]] bool write_next_scanline_called() const {
    return write_next_scanline_called_;
  }
  [[nodiscard]] bool finalize_write_called() const {
    return finalize_write_called_;
  }

 private:
  bool initialize_called_{false};
  bool prepare_image_called_{false};
  bool prepare_next_frame_called_{false};
  bool write_next_scanline_called_{false};
  bool finalize_write_called_{false};
  std::string* output_ = nullptr;
  ImageSpec image_spec_;
  FrameSpec frame_spec_;
};

TEST(MultipleFrameReaderTest, Construction) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);
  EXPECT_EQ(&handler, reader.message_handler());
  EXPECT_EQ(QUIRKS_NONE, reader.quirks_mode());
}

TEST(MultipleFrameReaderTest, InitializeWithBuffer) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  const char buffer[] = "fake image data";
  ScanlineStatus status = reader.Initialize(buffer, sizeof(buffer));
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(reader.initialize_called());
}

TEST(MultipleFrameReaderTest, Reset) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  ScanlineStatus status = reader.Reset();
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(reader.reset_called());
}

TEST(MultipleFrameReaderTest, PrepareNextFrame) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  reader.set_has_more_frames(true);
  EXPECT_TRUE(reader.HasMoreFrames());

  ScanlineStatus status = reader.PrepareNextFrame();
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(reader.prepare_next_frame_called());
}

TEST(MultipleFrameReaderTest, ReadNextScanline) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  reader.set_has_more_scanlines(true);
  EXPECT_TRUE(reader.HasMoreScanlines());

  const void* scanline = nullptr;
  ScanlineStatus status = reader.ReadNextScanline(&scanline);
  EXPECT_TRUE(status.Success());
  EXPECT_NE(nullptr, scanline);
}

TEST(MultipleFrameReaderTest, GetImageSpec) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  ImageSpec expected;
  expected.width = 320;
  expected.height = 240;
  expected.num_frames = 5;
  reader.set_image_spec(expected);

  ImageSpec actual;
  ScanlineStatus status = reader.GetImageSpec(&actual);
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(expected.Equals(actual));
}

TEST(MultipleFrameReaderTest, GetFrameSpec) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  FrameSpec expected;
  expected.width = 160;
  expected.height = 120;
  expected.pixel_format = RGBA_8888;
  expected.duration_ms = 100;
  reader.set_frame_spec(expected);

  FrameSpec actual;
  ScanlineStatus status = reader.GetFrameSpec(&actual);
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(expected.Equals(actual));
}

TEST(MultipleFrameReaderTest, SetQuirksMode) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  EXPECT_EQ(QUIRKS_NONE, reader.quirks_mode());

  ScanlineStatus status = reader.set_quirks_mode(QUIRKS_CHROME);
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(QUIRKS_CHROME, reader.quirks_mode());

  status = reader.set_quirks_mode(QUIRKS_FIREFOX);
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(QUIRKS_FIREFOX, reader.quirks_mode());
}

TEST(MultipleFrameReaderTest, ConvenienceResetSuccess) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(reader.Reset(&status));
  EXPECT_TRUE(status.Success());
}

TEST(MultipleFrameReaderTest, ConvenienceResetSkipsOnError) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  ScanlineStatus status(net_instaweb::SCANLINE_STATUS_PARSE_ERROR);
  EXPECT_FALSE(reader.Reset(&status));
  // Reset should not have been called since status was already error
  EXPECT_FALSE(reader.reset_called());
}

TEST(MultipleFrameReaderTest, ConvenienceInitializeSuccess) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  const char buffer[] = "data";
  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(reader.Initialize(buffer, sizeof(buffer), &status));
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(reader.initialize_called());
}

TEST(MultipleFrameReaderTest, ConvenienceInitializeSkipsOnError) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  const char buffer[] = "data";
  ScanlineStatus status(net_instaweb::SCANLINE_STATUS_PARSE_ERROR);
  EXPECT_FALSE(reader.Initialize(buffer, sizeof(buffer), &status));
  EXPECT_FALSE(reader.initialize_called());
}

TEST(MultipleFrameReaderTest, ConvenienceSetQuirksMode) {
  NullMessageHandler handler;
  MockMultipleFrameReader reader(&handler);

  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(reader.set_quirks_mode(QUIRKS_CHROME, &status));
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(QUIRKS_CHROME, reader.quirks_mode());
}

// =============================================================================
// MultipleFrameWriter tests
// =============================================================================

TEST(MultipleFrameWriterTest, Construction) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);
  EXPECT_EQ(&handler, writer.message_handler());
}

TEST(MultipleFrameWriterTest, BasicWriteFlow) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  // Initialize
  std::string output;
  ScanlineStatus status = writer.Initialize(nullptr, &output);
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(writer.initialize_called());

  // Prepare image
  ImageSpec image_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  image_spec.num_frames = 1;
  status = writer.PrepareImage(&image_spec);
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(writer.prepare_image_called());

  // Prepare frame
  FrameSpec frame_spec;
  frame_spec.width = 100;
  frame_spec.height = 100;
  frame_spec.pixel_format = RGB_888;
  status = writer.PrepareNextFrame(&frame_spec);
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(writer.prepare_next_frame_called());

  // Write scanline
  uint8_t scanline[300] = {0};
  status = writer.WriteNextScanline(scanline);
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(writer.write_next_scanline_called());

  // Finalize
  status = writer.FinalizeWrite();
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(writer.finalize_write_called());
}

TEST(MultipleFrameWriterTest, ConvenienceInitializeSuccess) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  std::string output;
  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(writer.Initialize(nullptr, &output, &status));
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(writer.initialize_called());
}

TEST(MultipleFrameWriterTest, ConvenienceInitializeSkipsOnError) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  std::string output;
  ScanlineStatus status(net_instaweb::SCANLINE_STATUS_PARSE_ERROR);
  EXPECT_FALSE(writer.Initialize(nullptr, &output, &status));
  EXPECT_FALSE(writer.initialize_called());
}

TEST(MultipleFrameWriterTest, ConveniencePrepareImage) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  ImageSpec image_spec;
  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(writer.PrepareImage(&image_spec, &status));
  EXPECT_TRUE(status.Success());
}

TEST(MultipleFrameWriterTest, ConveniencePrepareNextFrame) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  FrameSpec frame_spec;
  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(writer.PrepareNextFrame(&frame_spec, &status));
  EXPECT_TRUE(status.Success());
}

TEST(MultipleFrameWriterTest, ConvenienceWriteNextScanline) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  uint8_t scanline[100] = {0};
  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(writer.WriteNextScanline(scanline, &status));
  EXPECT_TRUE(status.Success());
}

TEST(MultipleFrameWriterTest, ConvenienceFinalizeWrite) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  EXPECT_TRUE(writer.FinalizeWrite(&status));
  EXPECT_TRUE(status.Success());
}

TEST(MultipleFrameWriterTest, ConvenienceFinalizeWriteSkipsOnError) {
  NullMessageHandler handler;
  MockMultipleFrameWriter writer(&handler);

  ScanlineStatus status(net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR);
  EXPECT_FALSE(writer.FinalizeWrite(&status));
  EXPECT_FALSE(writer.finalize_write_called());
}

}  // namespace
}  // namespace pagespeed::image_compression
