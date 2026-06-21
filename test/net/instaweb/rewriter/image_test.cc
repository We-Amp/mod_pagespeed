/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit tests for Image class used in rewriting.

#include "net/instaweb/rewriter/public/image.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/image_data_lookup.h"
#include "net/instaweb/rewriter/public/image_url_encoder.h"
#include "pagespeed/kernel/base/base64_util.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/statistics_template.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/data_url.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/jpeg_utils.h"
#include "pagespeed/kernel/image/read_image.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/net/instaweb/rewriter/image_test_base.h"
#include "test/net/instaweb/rewriter/image_testing_peer.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/image/jpeg_optimizer_test_helper.h"
#include "test/pagespeed/kernel/image/test_utils.h"

using pagespeed::image_compression::JpegUtils;
using pagespeed::image_compression::kMessagePatternAnimatedGif;
using pagespeed::image_compression::kMessagePatternPixelFormat;
using pagespeed::image_compression::kMessagePatternStats;
using pagespeed::image_compression::kMessagePatternUnexpectedEOF;
using pagespeed::image_compression::kMessagePatternWritingToWebp;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::WEBP_ANIMATED;
using pagespeed::image_compression::WEBP_LOSSLESS;
using pagespeed::image_compression::WEBP_LOSSY;
using pagespeed::image_compression::WEBP_NONE;
using pagespeed_testing::image_compression::GetColorProfileMarker;
using pagespeed_testing::image_compression::GetExifDataMarker;
using pagespeed_testing::image_compression::
    GetJpegNumComponentsAndSamplingFactors;
using pagespeed_testing::image_compression::GetNumScansInJpeg;
using pagespeed_testing::image_compression::IsJpegSegmentPresent;

namespace net_instaweb {
namespace {

const char kProgressiveHeader[] = "\xFF\xC2";
const int kProgressiveHeaderStartIndex = 158;
const char kMessagePatternDataTruncated[] = "*data truncated*";
const char kMessagePatternFailedToCreateWebp[] = "*Failed to create webp*";
const char kMessagePatternFailedToEncodeWebp[] = "*Could not encode webp data*";
const char kMessagePatternNoDimension[] = "*Couldn't find * dimensions*";
const char kMessagePatternTimedOut[] = "*conversion timed out*";
const char kMessagePatternFailedToDecoode[] = "*failed to decode the image*";

class ConversionVarChecker {
 public:
  explicit ConversionVarChecker(Image::CompressionOptions* options)
      : thread_system_(Platform::CreateThreadSystem()),
        simple_stats_(thread_system_.get()) {
    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_GIF)
        ->timeout_count = simple_stats_.AddVariable("gif_webp_timeout");
    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_GIF)
        ->success_ms = simple_stats_.AddHistogram("gif_webp_success");
    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_GIF)
        ->failure_ms = simple_stats_.AddHistogram("gif_webp_failure");

    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_PNG)
        ->timeout_count = simple_stats_.AddVariable("png_webp_timeout");
    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_PNG)
        ->success_ms = simple_stats_.AddHistogram("png_webp_success");
    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_PNG)
        ->failure_ms = simple_stats_.AddHistogram("png_webp_failure");

    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_JPEG)
        ->timeout_count = simple_stats_.AddVariable("jpeg_webp_timeout");
    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_JPEG)
        ->success_ms = simple_stats_.AddHistogram("jpeg_webp_success");
    webp_conversion_variables_.Get(Image::ConversionVariables::FROM_JPEG)
        ->failure_ms = simple_stats_.AddHistogram("jpeg_webp_failure");

    webp_conversion_variables_.Get(Image::ConversionVariables::NONOPAQUE)
        ->timeout_count = simple_stats_.AddVariable("webp_alpha_timeout");
    webp_conversion_variables_.Get(Image::ConversionVariables::NONOPAQUE)
        ->success_ms = simple_stats_.AddHistogram("webp_alpha_success");
    webp_conversion_variables_.Get(Image::ConversionVariables::NONOPAQUE)
        ->failure_ms = simple_stats_.AddHistogram("webp_alpha_failure");

    webp_conversion_variables_.Get(Image::ConversionVariables::OPAQUE)
        ->timeout_count = simple_stats_.AddVariable("webp_opaque_timeout");
    webp_conversion_variables_.Get(Image::ConversionVariables::OPAQUE)
        ->success_ms = simple_stats_.AddHistogram("webp_opaque_success");
    webp_conversion_variables_.Get(Image::ConversionVariables::OPAQUE)
        ->failure_ms = simple_stats_.AddHistogram("webp_opaque_failure");

    webp_conversion_variables_
        .Get(Image::ConversionVariables::FROM_GIF_ANIMATED)
        ->timeout_count =
        simple_stats_.AddVariable("gif_webp_animated_timeout");
    webp_conversion_variables_
        .Get(Image::ConversionVariables::FROM_GIF_ANIMATED)
        ->success_ms = simple_stats_.AddHistogram("gif_webp_animated_success");
    webp_conversion_variables_
        .Get(Image::ConversionVariables::FROM_GIF_ANIMATED)
        ->failure_ms = simple_stats_.AddHistogram("gif_webp_animated_failure");

    options->webp_conversion_variables = &webp_conversion_variables_;
  }

  void Test(int gif_webp_timeout, int gif_webp_success, int gif_webp_failure,

            int png_webp_timeout, int png_webp_success, int png_webp_failure,

            int jpeg_webp_timeout, int jpeg_webp_success, int jpeg_webp_failure,

            int gif_webp_animated_timeout, int gif_webp_animated_success,
            int gif_webp_animated_failure,

            bool opaque) {
    EXPECT_EQ(gif_webp_timeout, webp_conversion_variables_
                                    .Get(Image::ConversionVariables::FROM_GIF)
                                    ->timeout_count->Get());
    EXPECT_EQ(gif_webp_success, webp_conversion_variables_
                                    .Get(Image::ConversionVariables::FROM_GIF)
                                    ->success_ms->Count());
    EXPECT_EQ(gif_webp_failure, webp_conversion_variables_
                                    .Get(Image::ConversionVariables::FROM_GIF)
                                    ->failure_ms->Count());

    EXPECT_EQ(png_webp_timeout, webp_conversion_variables_
                                    .Get(Image::ConversionVariables::FROM_PNG)
                                    ->timeout_count->Get());
    EXPECT_EQ(png_webp_success, webp_conversion_variables_
                                    .Get(Image::ConversionVariables::FROM_PNG)
                                    ->success_ms->Count());
    EXPECT_EQ(png_webp_failure, webp_conversion_variables_
                                    .Get(Image::ConversionVariables::FROM_PNG)
                                    ->failure_ms->Count());

    EXPECT_EQ(jpeg_webp_timeout, webp_conversion_variables_
                                     .Get(Image::ConversionVariables::FROM_JPEG)
                                     ->timeout_count->Get());
    EXPECT_EQ(jpeg_webp_success, webp_conversion_variables_
                                     .Get(Image::ConversionVariables::FROM_JPEG)
                                     ->success_ms->Count());
    EXPECT_EQ(jpeg_webp_failure, webp_conversion_variables_
                                     .Get(Image::ConversionVariables::FROM_JPEG)
                                     ->failure_ms->Count());

    EXPECT_EQ(gif_webp_animated_timeout,
              webp_conversion_variables_
                  .Get(Image::ConversionVariables::FROM_GIF_ANIMATED)
                  ->timeout_count->Get());
    EXPECT_EQ(gif_webp_animated_success,
              webp_conversion_variables_
                  .Get(Image::ConversionVariables::FROM_GIF_ANIMATED)
                  ->success_ms->Count());
    EXPECT_EQ(gif_webp_animated_failure,
              webp_conversion_variables_
                  .Get(Image::ConversionVariables::FROM_GIF_ANIMATED)
                  ->failure_ms->Count());

    int total_timeout = gif_webp_timeout + png_webp_timeout +
                        jpeg_webp_timeout + gif_webp_animated_timeout;
    int total_success = gif_webp_success + png_webp_success +
                        jpeg_webp_success + gif_webp_animated_success;
    int total_failure = gif_webp_failure + png_webp_failure +
                        jpeg_webp_failure + gif_webp_animated_failure;

    Image::ConversionBySourceVariable* webp_transparency =
        webp_conversion_variables_.Get(
            (opaque ? Image::ConversionVariables::OPAQUE
                    : Image::ConversionVariables::NONOPAQUE));

    EXPECT_EQ(total_timeout, webp_transparency->timeout_count->Get());
    EXPECT_EQ(total_success, webp_transparency->success_ms->Count());
    EXPECT_EQ(total_failure, webp_transparency->failure_ms->Count());
  }

 private:
  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats simple_stats_;
  Image::ConversionVariables webp_conversion_variables_;
};

}  // namespace

class ImageTest : public ImageTestBase {
 public:
  ImageTest() : options_(new Image::CompressionOptions()) {}

 protected:
  void SetUp() override {
    message_handler_.AddPatternToSkipPrinting(kMessagePatternAnimatedGif);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternDataTruncated);
    message_handler_.AddPatternToSkipPrinting(
        kMessagePatternFailedToCreateWebp);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternFailedToDecoode);
    message_handler_.AddPatternToSkipPrinting(
        kMessagePatternFailedToEncodeWebp);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternNoDimension);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternPixelFormat);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternStats);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternTimedOut);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternUnexpectedEOF);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternWritingToWebp);
  }

  GoogleString* GetOutputContents(Image* image) {
    return &(image->output_contents_);
  }

  void WriteToBuffer(const char* contents, GoogleString* str) {
    *str = contents;
  }

  void ExpectEmptyOutput(Image* image) {
    EXPECT_FALSE(image->output_valid_);
    EXPECT_TRUE(image->output_contents_.empty());
  }

  void ExpectContentType(ImageType image_type, Image* image) {
    EXPECT_EQ(image_type, image->image_type_);
  }

  void ExpectDimensions(ImageType image_type, int size, int expected_width,
                        int expected_height, Image* image) {
    EXPECT_EQ(size, image->input_size());
    EXPECT_EQ(image_type, image->image_type());
    ImageDim image_dim;
    image_dim.Clear();
    image->Dimensions(&image_dim);
    EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(image_dim));
    EXPECT_EQ(expected_width, image_dim.width());
    EXPECT_EQ(expected_height, image_dim.height());
    EXPECT_EQ(
        absl::StrFormat("%dx%dxZZ", image_dim.width(), image_dim.height()),
        EncodeUrlAndDimensions("ZZ", image_dim));
  }

  void CheckInvalid(const GoogleString& name, const GoogleString& contents,
                    ImageType input_type, ImageType output_type,
                    bool progressive) {
    ImagePtr image(ImageFromString(output_type, name, contents, progressive));
    EXPECT_EQ(contents.size(), image->input_size());
    EXPECT_EQ(input_type, image->image_type());
    ImageDim image_dim;
    image_dim.Clear();
    image->Dimensions(&image_dim);
    EXPECT_FALSE(ImageUrlEncoder::HasValidDimension(image_dim));
    EXPECT_FALSE(image_dim.has_width());
    EXPECT_FALSE(image_dim.has_height());
    EXPECT_EQ(contents.size(), image->output_size());
    EXPECT_EQ("xZZ", EncodeUrlAndDimensions("ZZ", image_dim));
  }

  bool CheckImageFromFile(const char* filename, ImageType input_type,
                          ImageType output_type, int min_bytes_to_type,
                          int min_bytes_to_dimensions, int width, int height,
                          int size, bool optimizable) {
    return CheckImageFromFile(filename, input_type, output_type, output_type,
                              input_type, min_bytes_to_type,
                              min_bytes_to_dimensions, width, height, size,
                              optimizable);
  }

  bool CheckImageFromFile(const char* filename, ImageType input_type,
                          ImageType intended_output_type,
                          ImageType actual_output_type,
                          ImageType type_for_truncated_image,
                          int min_bytes_to_type, int min_bytes_to_dimensions,
                          int width, int height, int size, bool optimizable) {
    // Set options to convert to intended_output_type, but to allow for
    // negative tests, don't clear any other options.
    if (intended_output_type == IMAGE_WEBP) {
      options_->preferred_webp = WEBP_LOSSY;
    } else if (intended_output_type == IMAGE_WEBP_LOSSLESS_OR_ALPHA) {
      options_->preferred_webp = WEBP_LOSSLESS;
    }
    switch (intended_output_type) {
      case IMAGE_WEBP:
      case IMAGE_WEBP_LOSSLESS_OR_ALPHA:
        options_->convert_jpeg_to_webp = true;
        FALLTHROUGH_INTENDED;
      case IMAGE_JPEG:
        options_->convert_png_to_jpeg = true;
        FALLTHROUGH_INTENDED;
      case IMAGE_PNG:
        options_->convert_gif_to_png = true;
        break;
      default:
        break;
    }

    bool progressive = options_->progressive_jpeg;
    int jpeg_quality = options_->jpeg_quality;
    GoogleString contents;
    ImagePtr image(
        ReadFromFileWithOptions(filename, &contents, options_.release()));
    ExpectDimensions(input_type, size, width, height, image.get());
    if (optimizable) {
      EXPECT_GT(size, image->output_size());
      ExpectDimensions(actual_output_type, size, width, height, image.get());
    } else {
      EXPECT_EQ(size, image->output_size());
      ExpectDimensions(input_type, size, width, height, image.get());
    }

    // Construct data url, then decode it and check for match.
    CachedResult cached;
    GoogleString data_url;
    EXPECT_NE(IMAGE_UNKNOWN, image->image_type());
    StringPiece image_contents = image->Contents();

    progressive &=
        ImageTestingPeer::ShouldConvertToProgressive(jpeg_quality, image.get());
    if (progressive) {
      EXPECT_STREQ(kProgressiveHeader,
                   image_contents.substr(kProgressiveHeaderStartIndex,
                                         strlen(kProgressiveHeader)));
    }

    cached.set_inlined_data(image_contents.data(), image_contents.size());
    cached.set_inlined_image_type(static_cast<int>(image->image_type()));
    DataUrl(*Image::TypeToContentType(
                static_cast<ImageType>(cached.inlined_image_type())),
            BASE64, cached.inlined_data(), &data_url);
    GoogleString data_header("data:");
    data_header.append(image->content_type()->mime_type());
    data_header.append(";base64,");
    EXPECT_EQ(data_header, data_url.substr(0, data_header.size()));
    StringPiece encoded_contents(data_url.data() + data_header.size(),
                                 data_url.size() - data_header.size());
    GoogleString decoded_contents;
    EXPECT_TRUE(Mime64Decode(encoded_contents, &decoded_contents));
    EXPECT_EQ(image->Contents(), decoded_contents);

    // Now truncate the file in various ways and make sure we still
    // get partial data.
    GoogleString dim_data(contents, 0, min_bytes_to_dimensions);
    ImagePtr dim_image(
        ImageFromString(intended_output_type, filename, dim_data, progressive));
    ExpectDimensions(input_type, min_bytes_to_dimensions, width, height,
                     dim_image.get());
    EXPECT_EQ(min_bytes_to_dimensions, dim_image->output_size());

    GoogleString no_dim_data(contents, 0, min_bytes_to_dimensions - 1);
    CheckInvalid(filename, no_dim_data, type_for_truncated_image,
                 intended_output_type, progressive);
    GoogleString type_data(contents, 0, min_bytes_to_type);
    CheckInvalid(filename, type_data, type_for_truncated_image,
                 intended_output_type, progressive);
    GoogleString junk(contents, 0, min_bytes_to_type - 1);
    CheckInvalid(filename, junk, IMAGE_UNKNOWN, IMAGE_UNKNOWN, progressive);
    return progressive;
  }

  GoogleString EncodeUrlAndDimensions(const StringPiece& origin_url,
                                      const ImageDim& dim) {
    StringVector v;
    v.push_back(origin_url.as_string());
    GoogleString out;
    ResourceContext data;
    *data.mutable_desired_image_dims() = dim;
    encoder_.Encode(v, &data, &out);
    return out;
  }

  bool DecodeUrlAndDimensions(const StringPiece& encoded, ImageDim* dim,
                              GoogleString* url) {
    ResourceContext context;
    StringVector urls;
    bool result = encoder_.Decode(encoded, &urls, &context, &message_handler_);
    if (result) {
      EXPECT_EQ(1, urls.size());
      url->assign(urls.back());
      *dim = context.desired_image_dims();
    }
    return result;
  }

  void ExpectBadDim(const StringPiece& url) {
    GoogleString origin_url;
    ImageDim dim;
    EXPECT_FALSE(DecodeUrlAndDimensions(url, &dim, &origin_url));
    EXPECT_FALSE(ImageUrlEncoder::HasValidDimension(dim));
  }

  void SetJpegRecompressionAndQuality(Image::CompressionOptions* options) {
    options->jpeg_quality = 85;
    options->recompress_jpeg = true;
  }

  ImageUrlEncoder encoder_;
  std::unique_ptr<Image::CompressionOptions> options_;

 private:
  ImageTest(const ImageTest&) = delete;
  ImageTest& operator=(const ImageTest&) = delete;
};

namespace {

TEST_F(ImageTest, EmptyImageUnidentified) {
  CheckInvalid("Empty string", "", IMAGE_UNKNOWN, IMAGE_UNKNOWN, false);
}

TEST_F(ImageTest, InputWebpTest) {
  CheckImageFromFile(kScenery, IMAGE_WEBP, IMAGE_WEBP, IMAGE_WEBP,
                     IMAGE_UNKNOWN,
                     29,  // Min bytes to bother checking file type at all.
                     30, 550, 368, 30320, false);
}

TEST_F(ImageTest, WebpLowResTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_webp = true;
  options->preferred_webp = WEBP_LOSSY;
  GoogleString contents;
  ImagePtr image(ReadFromFileWithOptions(kScenery, &contents, options));
  int filesize = 30320;
  image->SetTransformToLowRes();
  EXPECT_GT(filesize, image->output_size());
}

TEST_F(ImageTest, WebpLaLowResTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_webp = true;
  options->preferred_webp = WEBP_LOSSLESS;
  GoogleString contents;
  ImagePtr image(ReadFromFileWithOptions(kScenery, &contents, options));
  int filesize = 30320;
  image->SetTransformToLowRes();
  EXPECT_GT(filesize, image->output_size());
}

TEST_F(ImageTest, PngTest) {
  options_->recompress_png = true;
  CheckImageFromFile(
      kBikeCrash, IMAGE_PNG, IMAGE_PNG, ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2, 100, 100,
      26548, true);
}

TEST_F(ImageTest, PngToWebpTest) {
  ConversionVarChecker conversion_var_checker(options_.get());
  options_->webp_quality = 75;
  CheckImageFromFile(
      kBikeCrash, IMAGE_PNG, IMAGE_WEBP, ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2, 100, 100,
      26548, true);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 1, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, PngToWebpFailToJpegDueToPreferredTest) {
  ConversionVarChecker conversion_var_checker(options_.get());
  options_->preferred_webp = WEBP_NONE;
  options_->webp_quality = 75;
  options_->jpeg_quality = 85;
  options_->convert_jpeg_to_webp = true;
  CheckImageFromFile(
      kBikeCrash, IMAGE_PNG, IMAGE_JPEG, ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2, 100, 100,
      26548, true);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, PngToWebpLaTest) {
  ConversionVarChecker conversion_var_checker(options_.get());
  options_->webp_quality = 75;
  CheckImageFromFile(
      kCuppa, IMAGE_PNG, IMAGE_WEBP_LOSSLESS_OR_ALPHA,
      ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2, 65, 70,
      1763, true);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 1, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, PngAlphaFailToWebpLossyTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->preferred_webp = WEBP_LOSSY;
  options->allow_webp_alpha = false;
  options->webp_quality = 75;
  options->jpeg_quality = 85;
  options->convert_png_to_jpeg = true;
  options->convert_jpeg_to_webp = true;
  EXPECT_EQ(0, options->conversions_attempted);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kCuppaTransparent, &buffer, options));
  image->output_size();
  EXPECT_EQ(ContentType::kPng, image->content_type()->type());

  // "kCuppaTransparent" is a graphic. It should be compressed losslessly,
  // but the configuration only allows lossy compression, so no compression
  // will be attempted.
  EXPECT_EQ(0, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, PngAlphaToWebpLaTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->preferred_webp = WEBP_LOSSLESS;
  options->allow_webp_alpha = true;
  options->convert_png_to_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->webp_quality = 75;
  options->jpeg_quality = 85;
  EXPECT_EQ(0, options->conversions_attempted);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kCuppaTransparent, &buffer, options));
  image->output_size();
  EXPECT_EQ(ContentType::kWebp, image->content_type()->type());
  EXPECT_EQ(1, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 1, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);
}

TEST_F(ImageTest, PngAlphaToWebpTestFailsBecauseTooManyTries) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->preferred_webp = WEBP_LOSSLESS;
  options->allow_webp_alpha = true;
  options->convert_png_to_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->webp_quality = 75;
  options->jpeg_quality = 85;
  options->conversions_attempted = 2;

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kCuppaTransparent, &buffer, options));
  image->output_size();
  EXPECT_EQ(ContentType::kPng, image->content_type()->type());
  EXPECT_EQ(2, options->conversions_attempted);
  // There were already enough (2) attempts, so we shouldn't try any
  // more conversions.
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);
}

// This tests that we compress the alpha channel on the webp. If we
// don't on this image, it becomes larger than the original.
TEST_F(ImageTest, PngLargeAlphaToWebpLaTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->preferred_webp = WEBP_LOSSLESS;
  options->allow_webp_alpha = true;
  options->convert_png_to_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->webp_quality = 75;
  options->jpeg_quality = 85;
  EXPECT_EQ(0, options->conversions_attempted);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kRedbrush, &buffer, options));
  EXPECT_GT(image->input_size(), image->output_size());
  EXPECT_EQ(ContentType::kWebp, image->content_type()->type());
  EXPECT_EQ(1, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 1, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);
  // TODO(vchudnov): Check that the pixels match.
}

// Same image and settings that succeed in PngLargeAlphaToWebpTest,
// should fail when using a very short timeout.
TEST_F(ImageTest, PngLargeAlphaToWebpTimesOutToPngTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->preferred_webp = WEBP_LOSSLESS;
  options->allow_webp_alpha = true;
  options->convert_png_to_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->webp_quality = 75;
  options->jpeg_quality = 85;
  options->webp_conversion_timeout_ms = 1;
  EXPECT_EQ(0, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kRedbrush, &buffer, options));
  timer_.SetTimeDeltaUs(1);  // When setting deadline
  timer_.SetTimeDeltaUs(1);  // Before attempting webp lossless
  timer_.SetTimeDeltaUs(     // During conversion
      1000 * options->webp_conversion_timeout_ms + 1);
  image->output_size();
  EXPECT_EQ(ContentType::kPng, image->content_type()->type());
  conversion_var_checker.Test(0, 0, 0,  // gif
                              1, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);

  // One attempt for WebpP conversion, one attempt for the fall-back
  // to PNG/JPEG.
  EXPECT_EQ(2, options->conversions_attempted);
}

// Same image and settings that succeed in PngLargeAlphaToWebpTest,
// should succeed if processing is really fast.
TEST_F(ImageTest, PngLargeAlphaToWebpDoesNotTimeOutTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->preferred_webp = WEBP_LOSSLESS;
  options->allow_webp_alpha = true;
  options->convert_png_to_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->webp_quality = 75;
  options->jpeg_quality = 85;
  options->webp_conversion_timeout_ms = 1;
  EXPECT_EQ(0, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kRedbrush, &buffer, options));
  timer_.SetTimeDeltaUs(1);  // When setting deadline
  timer_.SetTimeDeltaUs(1);  // Before attempting webp lossless
  timer_.SetTimeDeltaUs(     // During conversion
      1000 * options->webp_conversion_timeout_ms - 2);
  image->output_size();
  EXPECT_EQ(ContentType::kWebp, image->content_type()->type());
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 1, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);

  // One attempt for WebpP conversion.
  EXPECT_EQ(1, options->conversions_attempted);
}

TEST_F(ImageTest, PngToJpegTest) {
  options_->jpeg_quality = 85;
  CheckImageFromFile(
      kBikeCrash, IMAGE_PNG, IMAGE_JPEG, ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2, 100, 100,
      26548, true);
}

TEST_F(ImageTest, TooSmallToConvertPngToProgressiveJpegTest) {
  options_->progressive_jpeg = true;
  options_->jpeg_quality = 85;
  bool progressive = CheckImageFromFile(
      kBikeCrash, IMAGE_PNG, IMAGE_JPEG, ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2, 100, 100,
      26548, true);
  EXPECT_FALSE(progressive);
}

TEST_F(ImageTest, PngToProgressiveJpegTest) {
  options_->progressive_jpeg = true;
  options_->jpeg_quality = 85;
  options_->progressive_jpeg_min_bytes = 100;  // default is 10k.
  bool progressive = CheckImageFromFile(
      kBikeCrash, IMAGE_PNG, IMAGE_JPEG, ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2, 100, 100,
      26548, true);
  EXPECT_TRUE(progressive);
}

TEST_F(ImageTest, GifToPngTest) {
  CheckImageFromFile(kIronChef, IMAGE_GIF, IMAGE_PNG,
                     8,  // Min bytes to bother checking file type at all.
                     ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
                     192, 256, 24941, true);
}

TEST_F(ImageTest, GifToPngDisabledTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  options->convert_gif_to_png = false;
  EXPECT_EQ(0, options->conversions_attempted);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kIronChef, &buffer, options));
  image->output_size();
  EXPECT_EQ(ContentType::kGif, image->content_type()->type());
  EXPECT_EQ(0, options->conversions_attempted);
}

TEST_F(ImageTest, GifToJpegTest) {
  options_->jpeg_quality = 85;
  CheckImageFromFile(kIronChef, IMAGE_GIF, IMAGE_JPEG,
                     8,  // Min bytes to bother checking file type at all.
                     ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
                     192, 256, 24941, true);
}

TEST_F(ImageTest, GifToWebpTest) {
  ConversionVarChecker conversion_var_checker(options_.get());
  options_->webp_quality = 25;
  CheckImageFromFile(kIronChef, IMAGE_GIF, IMAGE_WEBP,
                     8,  // Min bytes to bother checking file type at all.
                     ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
                     192, 256, 24941, true);
  conversion_var_checker.Test(0, 1, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, GifToWebpLaTest) {
  ConversionVarChecker conversion_var_checker(options_.get());
  options_->webp_quality = 75;
  CheckImageFromFile(kTransparent, IMAGE_GIF, IMAGE_WEBP_LOSSLESS_OR_ALPHA,
                     8,  // Min bytes to bother checking file type at all.
                     ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
                     320, 320, 55800, true);
  conversion_var_checker.Test(0, 1, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              false);
}

TEST_F(ImageTest, AnimatedFilterNotEnabledTest) {
  CheckImageFromFile(kCradle, IMAGE_GIF, IMAGE_PNG,
                     8,  // Min bytes to bother checking file type at all.
                     ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
                     200, 150, 583374, false);
}

TEST_F(ImageTest, JpegTest) {
  options_->recompress_jpeg = true;
  CheckImageFromFile(kPuzzle, IMAGE_JPEG, IMAGE_JPEG,
                     8,     // Min bytes to bother checking file type at all.
                     6468,  // Specific to this test
                     1023, 766, 241260, true);
}

TEST_F(ImageTest, ProgressiveJpegTest) {
  options_->recompress_jpeg = true;
  options_->progressive_jpeg = true;
  CheckImageFromFile(kPuzzle, IMAGE_JPEG, IMAGE_JPEG,
                     8,     // Min bytes to bother checking file type at all.
                     6468,  // Specific to this test
                     1023, 766, 241260, true);
}

TEST_F(ImageTest, NumProgressiveScansTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->progressive_jpeg = true;
  options->jpeg_num_progressive_scans = 3;

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kPuzzle, &buffer, options));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_EQ(3, GetNumScansInJpeg(image->Contents().as_string()));
}

TEST_F(ImageTest, UseJpegLossyIfInputQualityIsLowTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->progressive_jpeg = true;

  GoogleString buffer;
  // Input image quality is 50.
  ImagePtr image(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_EQ(50, JpegUtils::GetImageQualityFromImage(image->Contents().data(),
                                                    image->Contents().size(),
                                                    &message_handler_));

  // When num progressive scans is set, we use lossy path. The compression
  // quality is the minimum of the input and the configuration, i.e., 50.
  options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->progressive_jpeg = true;
  buffer.clear();
  options->jpeg_num_progressive_scans = 1;
  image.reset(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_EQ(50, JpegUtils::GetImageQualityFromImage(image->Contents().data(),
                                                    image->Contents().size(),
                                                    &message_handler_));

  // Empty image will return -1 when we try to determine its quality.
  options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->progressive_jpeg = true;
  image.reset(
      NewImage("", "", GTestTempDir(), options, &timer_, &message_handler_));
  EXPECT_EQ(-1, JpegUtils::GetImageQualityFromImage(image->Contents().data(),
                                                    image->Contents().size(),
                                                    &message_handler_));
}

TEST_F(ImageTest, JpegRetainColorProfileTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->retain_color_profile = true;

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_TRUE(IsJpegSegmentPresent(buffer, GetColorProfileMarker()));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_TRUE(IsJpegSegmentPresent(image->Contents().as_string(),
                                   GetColorProfileMarker()));
  // Try stripping the color profile information.
  options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->retain_color_profile = false;
  buffer.clear();
  image.reset(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_TRUE(IsJpegSegmentPresent(buffer, GetColorProfileMarker()));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_FALSE(IsJpegSegmentPresent(image->Contents().as_string(),
                                    GetColorProfileMarker()));
}

TEST_F(ImageTest, JpegRetainColorSamplingTest) {
  int num_components, h_sampling_factor, v_sampling_factor;
  Image::CompressionOptions* options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->retain_color_profile = false;

  GoogleString buffer;
  // Input image color sampling is YUV 422. By defult we force YUV420.
  ImagePtr image(ReadFromFileWithOptions(kPuzzle, &buffer, options));
  GetJpegNumComponentsAndSamplingFactors(
      buffer, &num_components, &h_sampling_factor, &v_sampling_factor);
  EXPECT_EQ(3, num_components);
  EXPECT_EQ(2, h_sampling_factor);
  EXPECT_EQ(1, v_sampling_factor);
  EXPECT_GT(buffer.size(), image->output_size());
  GetJpegNumComponentsAndSamplingFactors(image->Contents().as_string(),
                                         &num_components, &h_sampling_factor,
                                         &v_sampling_factor);
  EXPECT_EQ(3, num_components);
  EXPECT_EQ(2, h_sampling_factor);
  EXPECT_EQ(2, v_sampling_factor);

  // Try retaining the color sampling.
  options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->retain_color_sampling = true;
  buffer.clear();
  image.reset(ReadFromFileWithOptions(kPuzzle, &buffer, options));
  EXPECT_GT(buffer.size(), image->output_size());
  GetJpegNumComponentsAndSamplingFactors(image->Contents().as_string(),
                                         &num_components, &h_sampling_factor,
                                         &v_sampling_factor);
  EXPECT_EQ(3, num_components);
  EXPECT_EQ(2, h_sampling_factor);
  EXPECT_EQ(1, v_sampling_factor);
}

TEST_F(ImageTest, JpegRetainExifDataTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->retain_exif_data = true;

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_TRUE(IsJpegSegmentPresent(buffer, GetExifDataMarker()));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_TRUE(
      IsJpegSegmentPresent(image->Contents().as_string(), GetExifDataMarker()));
  // Try stripping the color profile information.
  options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->retain_exif_data = false;
  buffer.clear();
  image.reset(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_TRUE(IsJpegSegmentPresent(buffer, GetExifDataMarker()));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_FALSE(
      IsJpegSegmentPresent(image->Contents().as_string(), GetExifDataMarker()));
}

TEST_F(ImageTest, WebpTest) {
  options_->webp_quality = 75;
  CheckImageFromFile(kPuzzle, IMAGE_JPEG, IMAGE_WEBP,
                     8,     // Min bytes to bother checking file type at all.
                     6468,  // Specific to this test
                     1023, 766, 241260, true);
}

TEST_F(ImageTest, JpegToWebpTimesOutTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->recompress_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->preferred_webp = WEBP_LOSSY;
  options->webp_quality = 75;
  options->webp_conversion_timeout_ms = 1;
  timer_.SetTimeDeltaUs(1);  // 1st increment of time, used for setting deadline
  timer_.SetTimeDeltaUs(1);  // 2nd increment of time, used for setting deadline
  timer_.SetTimeDeltaUs(     // During conversion
      1000 * options->webp_conversion_timeout_ms + 1);

  EXPECT_EQ(0, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kPuzzle, &buffer, options));
  image->output_size();
  EXPECT_EQ(ContentType::kJpeg, image->content_type()->type());

  EXPECT_EQ(2, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              1, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, JpegToWebpDoesNotTimeOutTest) {
  Image::CompressionOptions* options = new Image::CompressionOptions;
  ConversionVarChecker conversion_var_checker(options);
  options->recompress_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->preferred_webp = WEBP_LOSSY;
  options->webp_quality = 75;
  options->webp_conversion_timeout_ms = 1;
  timer_.SetTimeDeltaUs(1);  // When setting deadline
  timer_.SetTimeDeltaUs(     // During conversion
      1000 * options->webp_conversion_timeout_ms - 1);

  EXPECT_EQ(0, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);

  GoogleString buffer;
  ImagePtr image(ReadFromFileWithOptions(kPuzzle, &buffer, options));
  image->output_size();
  EXPECT_EQ(ContentType::kWebp, image->content_type()->type());

  EXPECT_EQ(1, options->conversions_attempted);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 1, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, WebpNonLaFromJpgTest) {
  ConversionVarChecker conversion_var_checker(options_.get());
  options_->webp_quality = 75;
  // Note that jpeg->webp cannot return a lossless webp.
  CheckImageFromFile(kPuzzle, IMAGE_JPEG, IMAGE_WEBP_LOSSLESS_OR_ALPHA,
                     IMAGE_WEBP, IMAGE_JPEG,
                     8,     // Min bytes to bother checking file type at all.
                     6468,  // Specific to this test
                     1023, 766, 241260, true);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 1, 0,  // jpeg
                              0, 0, 0,  // gif animated
                              true);
}

TEST_F(ImageTest, DrawImage) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_png = true;
  GoogleString buf1;
  ImagePtr image1(ReadFromFileWithOptions(kBikeCrash, &buf1, options));
  ImageDim image_dim1;
  image1->Dimensions(&image_dim1);

  options = new Image::CompressionOptions();
  options->recompress_png = true;
  GoogleString buf2;
  ImagePtr image2(ReadFromFileWithOptions(kCuppa, &buf2, options));
  ImageDim image_dim2;
  image2->Dimensions(&image_dim2);

  int width = std::max(image_dim1.width(), image_dim2.width());
  int height = image_dim1.height() + image_dim2.height();
  ASSERT_GT(width, 0);
  ASSERT_GT(height, 0);
  options = new Image::CompressionOptions();
  options->recompress_png = true;
  ImagePtr canvas(BlankImageWithOptions(width, height, IMAGE_PNG,
                                        GTestTempDir(), &timer_,
                                        &message_handler_, options));
  EXPECT_TRUE(canvas->DrawImage(image1.get(), 0, 0));
  EXPECT_TRUE(canvas->DrawImage(image2.get(), 0, image_dim1.height()));
  // The combined image should be bigger than either of the components, but
  // smaller than their unoptimized sum.
  EXPECT_GT(canvas->output_size(), image1->output_size());
  EXPECT_GT(canvas->output_size(), image2->output_size());
  EXPECT_GT(image1->input_size() + image2->input_size(), canvas->output_size());
}

// Make sure that the image produced by 'DrawImage()' is accurate for every
// pixel.
TEST_F(ImageTest, DrawImageDetails) {
  GoogleString buf1, buf2;
  uint8_t* image1_pixels = nullptr;
  uint8_t* image2_pixels = nullptr;
  uint8_t* canvas_pixels = nullptr;
  PixelFormat image1_format, image2_format, canvas_format;
  size_t image1_width, image2_width, canvas_width;
  size_t image1_height, image2_height, canvas_height;
  size_t image1_stride, image2_stride, canvas_stride;
  Image::CompressionOptions* image1_options = new Image::CompressionOptions();
  Image::CompressionOptions* image2_options = new Image::CompressionOptions();
  Image::CompressionOptions* canvas_options = new Image::CompressionOptions();
  canvas_options->recompress_png = true;

  // 'kIronChef' is an RGB GIF image while 'kCuppaTransparent' is a grayscale
  // transparent PNG image.
  ImagePtr image1(ReadFromFileWithOptions(kIronChef, &buf1, image1_options));
  ImagePtr image2(
      ReadFromFileWithOptions(kCuppaTransparent, &buf2, image2_options));

  ASSERT_TRUE(ReadImage(pagespeed::image_compression::IMAGE_GIF, buf1.data(),
                        buf1.length(), reinterpret_cast<void**>(&image1_pixels),
                        &image1_format, &image1_width, &image1_height,
                        &image1_stride, &message_handler_));

  ASSERT_TRUE(ReadImage(pagespeed::image_compression::IMAGE_PNG, buf2.data(),
                        buf2.length(), reinterpret_cast<void**>(&image2_pixels),
                        &image2_format, &image2_width, &image2_height,
                        &image2_stride, &message_handler_));

  int width = std::max(image1_width, image2_width);
  int height = image1_height + image2_height;
  ImagePtr canvas(BlankImageWithOptions(width, height, IMAGE_PNG,
                                        GTestTempDir(), &timer_,
                                        &message_handler_, canvas_options));
  EXPECT_TRUE(canvas->DrawImage(image1.get(), 0, 0));
  EXPECT_TRUE(canvas->DrawImage(image2.get(), 0, image1_height));

  ASSERT_TRUE(ReadImage(pagespeed::image_compression::IMAGE_PNG,
                        canvas->Contents().data(), canvas->Contents().length(),
                        reinterpret_cast<void**>(&canvas_pixels),
                        &canvas_format, &canvas_width, &canvas_height,
                        &canvas_stride, &message_handler_));

  CompareImageRegions(image1_pixels, image1_format, image1_stride, 0, 0,
                      canvas_pixels, canvas_format, canvas_stride, 0, 0,
                      image1_width, image1_height, &message_handler_);

  CompareImageRegions(image2_pixels, image2_format, image2_stride, 0, 0,
                      canvas_pixels, canvas_format, canvas_stride, 0,
                      image1_height, image2_width, image2_height,
                      &message_handler_);

  free(image1_pixels);
  free(image2_pixels);
  free(canvas_pixels);
}

TEST_F(ImageTest, BlankTransparentImage) {
  int width = 1000, height = 1000;
  Image::CompressionOptions* options = new Image::CompressionOptions();

  options->use_transparent_for_blank_image = true;
  ImagePtr blank(BlankImageWithOptions(width, height, IMAGE_PNG, GTestTempDir(),
                                       &timer_, &message_handler_, options));
  bool loaded = blank->EnsureLoaded(false);
  EXPECT_EQ(loaded, true);
  EXPECT_GT(blank->Contents().size(), 0);

  ImageDim blank_dim;
  blank->Dimensions(&blank_dim);
  EXPECT_EQ(blank_dim.width(), width);
  EXPECT_EQ(blank_dim.height(), height);
}

TEST_F(ImageTest, ResizeTo) {
  GoogleString buf;
  ImagePtr image(ReadImageFromFile(IMAGE_JPEG, kPuzzle, &buf, false));

  ImageDim new_dim;
  new_dim.set_width(10);
  new_dim.set_height(10);
  ASSERT_TRUE(image->ResizeTo(new_dim));

  ExpectEmptyOutput(image.get());
  ExpectContentType(IMAGE_JPEG, image.get());
}

TEST_F(ImageTest, CompressJpegUsingLossyOrLossless) {
  Image::CompressionOptions* options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  GoogleString buffer;

  // Input image quality is 50. When jpeg_quality is set to -1, lossless
  // will be used and the quality of the input image will be preserved.
  options->jpeg_quality = -1;
  ImagePtr image(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_EQ(50, JpegUtils::GetImageQualityFromImage(image->Contents().data(),
                                                    image->Contents().size(),
                                                    &message_handler_));

  // When jpeg_num_progressive_scans > 0, lossy will be used and the quality
  // will be set to the minimum of input quality and jpeg_quality.
  options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->jpeg_num_progressive_scans = 1;
  options->jpeg_quality = 51;
  buffer.clear();
  image.reset(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_EQ(50, JpegUtils::GetImageQualityFromImage(image->Contents().data(),
                                                    image->Contents().size(),
                                                    &message_handler_));

  // When jpeg_quality is less than input quality, lossy will be used and the
  // output quality is the minimum of them.
  options = new Image::CompressionOptions();
  SetJpegRecompressionAndQuality(options);
  options->jpeg_quality = 49;
  buffer.clear();
  image.reset(ReadFromFileWithOptions(kAppSegments, &buffer, options));
  EXPECT_GT(buffer.size(), image->output_size());
  EXPECT_EQ(49, JpegUtils::GetImageQualityFromImage(image->Contents().data(),
                                                    image->Contents().size(),
                                                    &message_handler_));
}

void SetBaseJpegOptions(Image::CompressionOptions* options) {
  options->preferred_webp = WEBP_LOSSY;
  options->allow_webp_alpha = true;
  options->convert_gif_to_png = true;
  options->convert_png_to_jpeg = true;
  options->webp_quality = 75;
  options->webp_animated_quality = 75;
  options->jpeg_quality = 85;
}

TEST_F(ImageTest, IgnoreTimeoutWhenFinishingWebp) {
  // Get the jpeg reference image
  Image::CompressionOptions* jpeg_options = new Image::CompressionOptions;
  SetBaseJpegOptions(jpeg_options);

  GoogleString jpeg_buffer;
  ImagePtr jpeg_image(
      ReadFromFileWithOptions(kBikeCrash, &jpeg_buffer, jpeg_options));

  jpeg_image->output_size();
  EXPECT_EQ(ContentType::kJpeg, jpeg_image->content_type()->type());

  // Get the webp reference image
  Image::CompressionOptions* webp_options = new Image::CompressionOptions;
  SetBaseJpegOptions(webp_options);
  webp_options->convert_jpeg_to_webp = true;
  webp_options->webp_conversion_timeout_ms = 1;
  GoogleString webp_buffer;
  ImagePtr webp_image(
      ReadFromFileWithOptions(kBikeCrash, &webp_buffer, webp_options));

  webp_image->output_size();
  EXPECT_EQ(ContentType::kWebp, webp_image->content_type()->type());

  // Make sure that if the timeout occurs before the first byte is
  // written, we do indeed time out.
  Image::CompressionOptions* timed_out_webp_options =
      new Image::CompressionOptions;
  SetBaseJpegOptions(timed_out_webp_options);
  timed_out_webp_options->convert_jpeg_to_webp = true;
  timed_out_webp_options->webp_conversion_timeout_ms = 1;

  GoogleString timed_out_webp_buffer;
  ImagePtr timed_out_webp_image(ReadFromFileWithOptions(
      kBikeCrash, &timed_out_webp_buffer, timed_out_webp_options));
  timer_.SetTimeMs(10);
  timer_.SetTimeDeltaUs(1);  // When setting deadline
  timer_.SetTimeDeltaUs(1);  // Before attempting webp lossless
  timer_.SetTimeDeltaUs(1);
  timer_.SetTimeDeltaUs(2000);

  timed_out_webp_image->output_size();
  EXPECT_EQ(ContentType::kJpeg, timed_out_webp_image->content_type()->type());
  EXPECT_EQ(jpeg_image->Contents(), timed_out_webp_image->Contents());

  // Test that if we time out after the first output byte is emitted, we keep
  // going with the webp output.
  Image::CompressionOptions* almost_done_webp_options =
      new Image::CompressionOptions;
  SetBaseJpegOptions(almost_done_webp_options);
  almost_done_webp_options->convert_jpeg_to_webp = true;
  almost_done_webp_options->webp_conversion_timeout_ms = 1;

  const char* kSomeData = "some data";
  GoogleString almost_done_webp_buffer;
  ImagePtr almost_done_webp_image(ReadFromFileWithOptions(
      kBikeCrash, &almost_done_webp_buffer, almost_done_webp_options));
  timer_.SetTimeMs(20);
  timer_.SetTimeDeltaUs(1);  // When setting deadline
  timer_.SetTimeDeltaUs(1);  // Before attempting webp lossless
  timer_.SetTimeDeltaUs(1);
  timer_.SetTimeDeltaUs(1);
  timer_.SetTimeDeltaUs(1);
  timer_.SetTimeDeltaUs(1);
  // We need to specify the template typenames explicitly below
  // because the compiler can't decide whether to use this test class
  // or its base class, ImageTest.
  timer_.SetTimeDeltaUsWithCallback(
      2000,
      MakeFunction<ImageTest_IgnoreTimeoutWhenFinishingWebp_Test, const char*,
                   GoogleString*>(
          this, &ImageTest_IgnoreTimeoutWhenFinishingWebp_Test::WriteToBuffer,
          kSomeData, GetOutputContents(almost_done_webp_image.get())));

  almost_done_webp_image->output_size();
  EXPECT_EQ(ContentType::kWebp, almost_done_webp_image->content_type()->type());
  GoogleString expected = kSomeData;
  expected.append(webp_image->Contents().as_string());
  EXPECT_EQ(expected, almost_done_webp_image->Contents());
}

TEST_F(ImageTest, AnimatedGifToWebpTest) {
  ConversionVarChecker conversion_var_checker(options_.get());
  options_->webp_animated_quality = 25;
  options_->allow_webp_animated = true;
  options_->preferred_webp = WEBP_ANIMATED;
  CheckImageFromFile(kCradle, IMAGE_GIF, IMAGE_WEBP_ANIMATED,
                     8,  // Min bytes to bother checking file type at all.
                     ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
                     200, 150, 583374, true);
  conversion_var_checker.Test(0, 0, 0,  // gif
                              0, 0, 0,  // png
                              0, 0, 0,  // jpeg
                              0, 1, 0,  // gif animated
                              true);
}

}  // namespace

// ----------------------------------------------------------------------------
// the design record: C2PA / Content-Credentials preserve-by-default (best-of consolidation).
//
// Two mechanisms, exercised through ImageImpl::ComputeOutputContents():
//  * Codec carry (jpeg_optimizer.cc): a JPEG that is recompressed but NOT resized
//    and NOT format-converted keeps its APP11/JUMBF manifest THROUGH the re-encode
//    (optimize AND preserve).
//  * Detect-and-skip fallback (the gate): for paths the codec cannot carry -- resize
//    (any format), JPEG->WebP, PNG/GIF -- a manifest-bearing image is served from the
//    ORIGINAL bytes byte-for-byte (skip-not-strip). Detection is a conservative byte
//    scan, so a spliced marker suffices and the tests stay hermetic (no real signing).
// ----------------------------------------------------------------------------

namespace {

// Splices a minimal JUMBF/C2PA stub into an APP11 (0xFF 0xEB) JPEG segment right
// after the SOI marker (0xFF 0xD8). The JPEG decoder ignores the unknown APP11
// segment, so the recompress path still yields a valid JPEG (without the
// segment) when preservation is off.
GoogleString SpliceC2paApp11IntoJpeg(const GoogleString& jpeg) {
  EXPECT_GE(jpeg.size(), static_cast<size_t>(2));
  EXPECT_EQ('\xFF', jpeg[0]);
  EXPECT_EQ('\xD8', jpeg[1]);
  const GoogleString payload =
      "JP"
      "jumb"
      "jumd"
      "c2pa";
  const size_t seg_len = payload.size() + 2;  // +2 for the length field itself.
  GoogleString out;
  out.append(jpeg.data(), 2);  // SOI.
  out.push_back('\xFF');       // APP11 marker...
  out.push_back('\xEB');       // ...0xFF 0xEB.
  out.push_back(static_cast<char>((seg_len >> 8) & 0xFF));
  out.push_back(static_cast<char>(seg_len & 0xFF));
  out.append(payload);
  out.append(jpeg.data() + 2, jpeg.size() - 2);  // Rest of the original JPEG.
  return out;
}

// ---- the design record Level A PNG carry-through fixtures ----
// The detector + ExtractPngC2paChunks key off a "jumb"/"jumd"/"c2pa" JUMBF byte
// signature inside a caBX chunk, so a spliced stub trips them without any real
// signing -- the tests stay hermetic.

// CRC-32 (ISO 3309 / PNG) over a byte range. PNG chunk CRCs cover the 4-byte type
// plus the data; a real CRC keeps the spliced caBX chunk valid so libpng decodes
// the image cleanly (a bad ancillary-chunk CRC would warn under the strict
// handler).
uint32_t PngCrc32(const char* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<unsigned char>(data[i]);
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void AppendBE32(uint32_t v, GoogleString* out) {
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>(v & 0xFF));
}

// Builds a valid caBX (C2PA box) PNG chunk carrying the JUMBF signature, framed
// as length(BE32) + "caBX" + data + crc(BE32). `extra_data_bytes` pads the chunk
// data so a large (>64KB) single-chunk manifest can be exercised. The exact bytes
// are what ExtractPngC2paChunks captures and the carry path must re-insert.
GoogleString MakeCaBxChunk(size_t extra_data_bytes) {
  GoogleString type_and_data;
  type_and_data.append("caBX");                    // PNG C2PA chunk type.
  type_and_data.append("JP");                      // JUMBF superbox tag.
  type_and_data.append("jumb");                    // detector token.
  type_and_data.append("jumd");                    // detector token.
  type_and_data.append("c2pa");                    // detector token.
  type_and_data.append(extra_data_bytes, '\x7E');  // opaque manifest padding.
  const uint32_t data_len = static_cast<uint32_t>(type_and_data.size() - 4);
  GoogleString chunk;
  AppendBE32(data_len, &chunk);
  chunk.append(type_and_data);
  AppendBE32(PngCrc32(type_and_data.data(), type_and_data.size()), &chunk);
  return chunk;
}

// Splices a caBX C2PA chunk into a PNG immediately after the IHDR chunk (the first
// chunk after the 8-byte signature), where ancillary chunks legitimately live.
// The unknown caBX chunk is ignored by the decoder, so the image still
// recompresses to a valid (smaller) PNG that has dropped the chunk when carry is
// off.
GoogleString SpliceC2paCaBxIntoPng(const GoogleString& png,
                                   size_t extra_bytes) {
  static const char kSig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1A', '\n'};
  EXPECT_GE(png.size(), static_cast<size_t>(8 + 25));
  EXPECT_EQ(0, memcmp(png.data(), kSig, 8));
  // IHDR is the first chunk: 4 (len) + 4 (type) + 13 (data) + 4 (crc) = 25.
  const size_t ihdr_end = 8 + 25;
  EXPECT_EQ(0, memcmp(png.data() + 12, "IHDR", 4));  // type at sig+8+4.
  GoogleString out;
  out.append(png.data(), ihdr_end);        // signature + IHDR.
  out.append(MakeCaBxChunk(extra_bytes));  // spliced caBX.
  out.append(png.data() + ihdr_end,
             png.size() - ihdr_end);  // rest (IDAT..IEND).
  return out;
}

// Builds a valid iTXt PNG chunk carrying a Content-Credentials XMP packet
// (keyword "XML:com.adobe.xmp", an xpacket marker, and the "cr:" namespace) --
// the XMP form of a C2PA manifest, with NO caBX box. Trips ImageHasXmpC2pa.
GoogleString MakeXmpItxtChunk() {
  GoogleString type_and_data;
  type_and_data.append("iTXt");
  type_and_data.append("XML:com.adobe.xmp");  // standard XMP iTXt keyword.
  type_and_data.append(
      " <?xpacket begin?> cr:provenance contentauth </xpacket>");
  const uint32_t data_len = static_cast<uint32_t>(type_and_data.size() - 4);
  GoogleString chunk;
  AppendBE32(data_len, &chunk);
  chunk.append(type_and_data);
  AppendBE32(PngCrc32(type_and_data.data(), type_and_data.size()), &chunk);
  return chunk;
}

GoogleString SpliceXmpItxtIntoPng(const GoogleString& png) {
  static const char kSig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1A', '\n'};
  EXPECT_GE(png.size(), static_cast<size_t>(8 + 25));
  EXPECT_EQ(0, memcmp(png.data(), kSig, 8));
  const size_t ihdr_end = 8 + 25;
  EXPECT_EQ(0, memcmp(png.data() + 12, "IHDR", 4));
  GoogleString out;
  out.append(png.data(), ihdr_end);  // signature + IHDR.
  out.append(MakeXmpItxtChunk());    // spliced XMP iTXt.
  out.append(png.data() + ihdr_end, png.size() - ihdr_end);  // rest.
  return out;
}

}  // namespace

TEST_F(ImageTest, PreserveC2paJpegRecompressKeepsManifest) {
  // The optimize-AND-preserve fast path: a non-resized JPEG is genuinely
  // recompressed, and the codec carries the APP11/JUMBF manifest into the output.
  GoogleString original;
  ASSERT_TRUE(
      file_system_.ReadFile(StrCat(GTestSrcDir(), kTestData, kPuzzle).c_str(),
                            &original, &message_handler_));
  const GoogleString with_c2pa = SpliceC2paApp11IntoJpeg(original);

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_jpeg = true;
  EXPECT_TRUE(options->preserve_c2pa);  // Default-on.
  ImagePtr image(NewImage(with_c2pa, "c2pa-recompress", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());

  // Recompressed (not a byte-identical skip) AND the manifest survives.
  EXPECT_NE(with_c2pa, out);
  EXPECT_NE(GoogleString::npos, out.find("c2pa"));
  ExpectContentType(IMAGE_JPEG, image.get());  // Stayed JPEG.
}

TEST_F(ImageTest, PreserveC2paOffStripsOnRecompress) {
  // The opt-out: with preserve OFF the codec carry is disabled, so recompression
  // strips the manifest (legacy behavior). Proves the ON path is non-tautological.
  GoogleString original;
  ASSERT_TRUE(
      file_system_.ReadFile(StrCat(GTestSrcDir(), kTestData, kPuzzle).c_str(),
                            &original, &message_handler_));
  const GoogleString with_c2pa = SpliceC2paApp11IntoJpeg(original);
  ASSERT_TRUE(pagespeed::image_compression::ImageHasC2paManifest(with_c2pa));

  Image::CompressionOptions* on_options = new Image::CompressionOptions();
  on_options->recompress_jpeg = true;
  EXPECT_TRUE(on_options->preserve_c2pa);  // Default-on.
  ImagePtr on(NewImage(with_c2pa, "c2pa-on", GTestTempDir(), on_options,
                       &timer_, &message_handler_));
  const GoogleString on_out(on->Contents().data(), on->Contents().size());

  Image::CompressionOptions* off_options = new Image::CompressionOptions();
  off_options->recompress_jpeg = true;
  off_options->preserve_c2pa = false;
  ImagePtr off(NewImage(with_c2pa, "c2pa-off", GTestTempDir(), off_options,
                        &timer_, &message_handler_));
  const GoogleString off_out(off->Contents().data(), off->Contents().size());

  EXPECT_NE(GoogleString::npos, on_out.find("c2pa"));   // ON preserves.
  EXPECT_EQ(GoogleString::npos, off_out.find("c2pa"));  // OFF strips.
  EXPECT_NE(on_out, off_out);
}

TEST_F(ImageTest, PreserveC2paNoOpOnCleanImage) {
  // On a manifest-free image the gate and the codec carry must both be pure no-ops:
  // preserve ON and preserve OFF must produce byte-identical output.
  GoogleString original;
  ASSERT_TRUE(
      file_system_.ReadFile(StrCat(GTestSrcDir(), kTestData, kPuzzle).c_str(),
                            &original, &message_handler_));

  Image::CompressionOptions* on_options = new Image::CompressionOptions();
  EXPECT_TRUE(on_options->preserve_c2pa);  // Default-on.
  on_options->recompress_jpeg = true;
  ImagePtr on(NewImage(original, "clean-on", GTestTempDir(), on_options,
                       &timer_, &message_handler_));
  const GoogleString on_out(on->Contents().data(), on->Contents().size());

  Image::CompressionOptions* off_options = new Image::CompressionOptions();
  off_options->recompress_jpeg = true;
  off_options->preserve_c2pa = false;
  ImagePtr off(NewImage(original, "clean-off", GTestTempDir(), off_options,
                        &timer_, &message_handler_));
  const GoogleString off_out(off->Contents().data(), off->Contents().size());

  EXPECT_EQ(off_out, on_out);
  EXPECT_EQ(GoogleString::npos, on_out.find("c2pa"));
}

TEST_F(ImageTest, PreserveC2paXmpJpegSkipsWhenExifStripped) {
  // Regression (review blocker): an XMP-carried Content-Credentials manifest in a JPEG
  // lives in APP1 (shared with EXIF). The codec carry only handles APP11/JUMBF, so when
  // EXIF stripping is on (the DEFAULT CoreFilters posture, retain_exif_data=false) a
  // plain recompress would drop the APP1/XMP manifest. The gate must skip-not-strip.
  GoogleString original;
  ASSERT_TRUE(
      file_system_.ReadFile(StrCat(GTestSrcDir(), kTestData, kPuzzle).c_str(),
                            &original, &message_handler_));
  ASSERT_GE(original.size(), static_cast<size_t>(2));
  // Splice an APP1 (0xFF 0xE1) XMP segment carrying the Content-Credentials namespace
  // right after the SOI. No JUMBF FourCC, so it is detected ONLY via the XMP path. The
  // decoder skips the unknown APP1 during decode. (No "c2pa"/"jumb" tokens on purpose.)
  const GoogleString xmp_payload =
      "http://ns.adobe.com/xap/1.0/ "
      "<?xpacket begin=\"\"?><x:xmpmeta><rdf:Description "
      "cr:provenance=\"urn:uuid:test\"/></x:xmpmeta><?xpacket end=\"w\"?>";
  const size_t seg_len = xmp_payload.size() + 2;  // +2 for the length field.
  GoogleString with_xmp;
  with_xmp.append(original.data(), 2);  // SOI.
  with_xmp.push_back('\xFF');
  with_xmp.push_back('\xE1');  // APP1.
  with_xmp.push_back(static_cast<char>((seg_len >> 8) & 0xFF));
  with_xmp.push_back(static_cast<char>(seg_len & 0xFF));
  with_xmp.append(xmp_payload);
  with_xmp.append(original.data() + 2, original.size() - 2);
  ASSERT_TRUE(pagespeed::image_compression::ImageHasXmpC2pa(with_xmp));

  // EXIF stripping ON (retain_exif_data=false) + preserve ON: gate skip-not-strips.
  Image::CompressionOptions* on_options = new Image::CompressionOptions();
  on_options->recompress_jpeg = true;
  on_options->retain_exif_data = false;    // StripImageMetaData posture.
  EXPECT_TRUE(on_options->preserve_c2pa);  // Default-on.
  ImagePtr on(NewImage(with_xmp, "xmp-on", GTestTempDir(), on_options, &timer_,
                       &message_handler_));
  const GoogleString on_out(on->Contents().data(), on->Contents().size());
  EXPECT_EQ(with_xmp, on_out);  // Served original byte-identical (gate fired).
  EXPECT_NE(GoogleString::npos, on_out.find("cr:provenance"));

  // preserve OFF + EXIF stripping: recompress drops the APP1/XMP manifest (the legacy
  // strip). Confirms the ON path is non-tautological.
  Image::CompressionOptions* off_options = new Image::CompressionOptions();
  off_options->recompress_jpeg = true;
  off_options->retain_exif_data = false;
  off_options->preserve_c2pa = false;
  ImagePtr off(NewImage(with_xmp, "xmp-off", GTestTempDir(), off_options,
                        &timer_, &message_handler_));
  const GoogleString off_out(off->Contents().data(), off->Contents().size());
  EXPECT_NE(with_xmp, off_out);  // Recompressed.
  EXPECT_EQ(GoogleString::npos,
            off_out.find("cr:provenance"));  // XMP stripped.
}

TEST_F(ImageTest, PreserveC2paResizedJpegSkipsToOriginal) {
  // The codec cannot carry the manifest through a resize (the ScanlineWriter copies
  // no markers), so the fallback serves the ORIGINAL bytes un-resized.
  GoogleString original;
  ASSERT_TRUE(
      file_system_.ReadFile(StrCat(GTestSrcDir(), kTestData, kPuzzle).c_str(),
                            &original, &message_handler_));
  const GoogleString with_c2pa = SpliceC2paApp11IntoJpeg(original);

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_jpeg = true;
  EXPECT_TRUE(options->preserve_c2pa);  // Default-on.
  ImagePtr image(NewImage(with_c2pa, "c2pa-resize", GTestTempDir(), options,
                          &timer_, &message_handler_));
  ImageDim new_dim;
  new_dim.set_width(10);
  new_dim.set_height(10);
  ASSERT_TRUE(image->ResizeTo(new_dim));
  const GoogleString out(image->Contents().data(), image->Contents().size());

  // Gate fired: byte-identical to the manifest-bearing original, manifest intact.
  EXPECT_EQ(with_c2pa, out);
  EXPECT_NE(GoogleString::npos, out.find("c2pa"));
}

TEST_F(ImageTest, PreserveC2paResizedOffAllowsResize) {
  // With preservation OFF the operator opts into legacy behavior: the image is
  // resized (and the manifest stripped) -- the gate must NOT fire.
  GoogleString original;
  ASSERT_TRUE(
      file_system_.ReadFile(StrCat(GTestSrcDir(), kTestData, kPuzzle).c_str(),
                            &original, &message_handler_));
  const GoogleString with_c2pa = SpliceC2paApp11IntoJpeg(original);

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_jpeg = true;
  options->preserve_c2pa = false;
  ImagePtr image(NewImage(with_c2pa, "c2pa-resize-off", GTestTempDir(), options,
                          &timer_, &message_handler_));
  ImageDim new_dim;
  new_dim.set_width(10);
  new_dim.set_height(10);
  ASSERT_TRUE(image->ResizeTo(new_dim));
  const GoogleString out(image->Contents().data(), image->Contents().size());

  // Gate bypassed: output differs from the original and the manifest is gone.
  EXPECT_NE(with_c2pa, out);
  EXPECT_EQ(GoogleString::npos, out.find("c2pa"));
}

TEST_F(ImageTest, PreserveC2paJpegToWebpStaysJpeg) {
  // WebP carries no APP11/JUMBF, so a manifest-bearing JPEG must NOT be converted to
  // WebP under the default -- it stays a (recompressed) JPEG that keeps the manifest.
  GoogleString original;
  ASSERT_TRUE(
      file_system_.ReadFile(StrCat(GTestSrcDir(), kTestData, kPuzzle).c_str(),
                            &original, &message_handler_));
  const GoogleString with_c2pa = SpliceC2paApp11IntoJpeg(original);

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_jpeg = true;
  options->convert_jpeg_to_webp = true;
  options->preferred_webp = WEBP_LOSSY;
  EXPECT_TRUE(options->preserve_c2pa);  // Default-on.
  ImagePtr image(NewImage(with_c2pa, "c2pa-webp", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());

  // Stayed JPEG (not WebP), manifest intact.
  EXPECT_EQ(ContentType::kJpeg, image->content_type()->type());
  EXPECT_NE(GoogleString::npos, out.find("c2pa"));

  // Contrast: preservation OFF does not keep the manifest (whether it converts to
  // WebP or strips on recompress), proving the ON path's skip is real.
  Image::CompressionOptions* off_options = new Image::CompressionOptions();
  off_options->recompress_jpeg = true;
  off_options->convert_jpeg_to_webp = true;
  off_options->preferred_webp = WEBP_LOSSY;
  off_options->preserve_c2pa = false;
  ImagePtr off(NewImage(with_c2pa, "c2pa-webp-off", GTestTempDir(), off_options,
                        &timer_, &message_handler_));
  const GoogleString off_out(off->Contents().data(), off->Contents().size());
  // The OFF arm actually converts to WebP, proving the ON arm's kJpeg result is the
  // !has_c2pa guard suppressing a conversion that would otherwise have succeeded.
  EXPECT_EQ(ContentType::kWebp, off->content_type()->type());
  EXPECT_EQ(GoogleString::npos, off_out.find("c2pa"));
  EXPECT_NE(out, off_out);
}

// ---- the design record Level A: PNG carry-through (ImageProvenanceCarry) ----
// JPEG carry is already covered by the PreserveC2pa* tests above (jpeg_optimizer
// carries APP11/JUMBF through a recompress via libjpeg's marker API). These tests
// cover the PNG path the carry flag adds: recompress AND re-splice the original
// caBX/iTXt chunks (the PNG optimizer strips ancillary chunks, so it cannot carry
// the manifest on its own).

TEST_F(ImageTest, CarryC2paPngRecompressesKeepsManifestAndDecodes) {
  GoogleString original;
  ASSERT_TRUE(file_system_.ReadFile(
      StrCat(GTestSrcDir(), kTestData, kBikeCrash).c_str(), &original,
      &message_handler_));
  const GoogleString with_c2pa = SpliceC2paCaBxIntoPng(original, /*extra=*/0);
  ASSERT_TRUE(pagespeed::image_compression::ImageHasC2paManifest(with_c2pa));
  // The exact original caBX chunk bytes the carry path must re-insert verbatim.
  const StringPieceVector chunks =
      pagespeed::image_compression::ExtractPngC2paChunks(with_c2pa);
  ASSERT_EQ(static_cast<size_t>(1), chunks.size());
  const GoogleString carrier(chunks[0].data(), chunks[0].size());

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_png = true;
  EXPECT_TRUE(options->preserve_c2pa);  // Level B floor, default-on.
  options->c2pa_carry = true;           // Level A opt-in.
  ImagePtr image(NewImage(with_c2pa, "carry-png", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());

  // Recompressed (NOT the Level-B byte-identical skip) and stayed PNG ...
  EXPECT_NE(with_c2pa, out);
  EXPECT_EQ(IMAGE_PNG, image->image_type());
  // ... and the ORIGINAL caBX chunk (incl. its CRC) is carried verbatim (D3).
  EXPECT_NE(GoogleString::npos, out.find(carrier));

  // Decodability: the spliced output re-reads as a valid PNG with the original
  // dimensions -- a corrupt chunk stream would not.
  ImagePtr reread(NewImage(out, "carry-png-reread", GTestTempDir(),
                           new Image::CompressionOptions(), &timer_,
                           &message_handler_));
  EXPECT_EQ(IMAGE_PNG, reread->image_type());
  ImageDim dim;
  dim.Clear();
  reread->Dimensions(&dim);
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(100, dim.width());
  EXPECT_EQ(100, dim.height());

  // Stronger decodability: fully re-optimize the carried output with BOTH carry
  // and the preserve floor OFF, which forces a complete decode and strips all
  // ancillary chunks. The PngReader must walk past the spliced caBX to the IDAT
  // pixels and re-emit; on success the now-unknown caBX is dropped. If the chunk
  // stream were malformed the recompress would fail and the original
  // (manifest-bearing) bytes would come back -- so a manifest-free result proves
  // the spliced PNG decoded cleanly end to end.
  Image::CompressionOptions* reopt_opts = new Image::CompressionOptions();
  reopt_opts->recompress_png = true;
  reopt_opts->preserve_c2pa = false;  // allow the strip (force a real decode).
  ImagePtr reopt(NewImage(out, "carry-png-reopt", GTestTempDir(), reopt_opts,
                          &timer_, &message_handler_));
  const GoogleString reopt_out(reopt->Contents().data(),
                               reopt->Contents().size());
  EXPECT_EQ(IMAGE_PNG, reopt->image_type());
  EXPECT_FALSE(pagespeed::image_compression::ImageHasC2paManifest(reopt_out));
}

TEST_F(ImageTest, CarryC2paXmpItxtPngCarriesAndDecodes) {
  // XMP-form manifest (iTXt with cr:, NO caBX box): has_c2pa fires via the XMP
  // path, png_carry triggers, and the iTXt is carried verbatim into the
  // recompressed output.
  GoogleString original;
  ASSERT_TRUE(file_system_.ReadFile(
      StrCat(GTestSrcDir(), kTestData, kBikeCrash).c_str(), &original,
      &message_handler_));
  const GoogleString with_xmp = SpliceXmpItxtIntoPng(original);
  ASSERT_TRUE(pagespeed::image_compression::ImageHasC2paManifest(with_xmp));
  const StringPieceVector chunks =
      pagespeed::image_compression::ExtractPngC2paChunks(with_xmp);
  ASSERT_EQ(static_cast<size_t>(1), chunks.size());
  const GoogleString carrier(chunks[0].data(), chunks[0].size());

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_png = true;
  options->c2pa_carry = true;
  ImagePtr image(NewImage(with_xmp, "carry-png-xmp", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());

  EXPECT_NE(with_xmp, out);  // recompressed, not a skip.
  EXPECT_EQ(IMAGE_PNG, image->image_type());
  EXPECT_NE(GoogleString::npos,
            out.find(carrier));  // XMP iTXt carried verbatim.
}

TEST_F(ImageTest, CarryC2paPngOffSkipsToOriginal) {
  // Carry OFF (default) but the preserve floor ON: a manifest-bearing PNG is
  // served byte-for-byte (Level-B skip), NOT recompressed -- and NOT stripped.
  GoogleString original;
  ASSERT_TRUE(file_system_.ReadFile(
      StrCat(GTestSrcDir(), kTestData, kBikeCrash).c_str(), &original,
      &message_handler_));
  const GoogleString with_c2pa = SpliceC2paCaBxIntoPng(original, 0);

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_png = true;
  EXPECT_TRUE(options->preserve_c2pa);
  EXPECT_FALSE(options->c2pa_carry);  // default off.
  ImagePtr image(NewImage(with_c2pa, "carry-png-off", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());
  EXPECT_EQ(with_c2pa, out);  // byte-identical skip.
}

TEST_F(ImageTest, CarryC2paPngToJpegFallsBackToSkip) {
  // Carry ON but the PNG converts to JPEG: the PNG caBX carrier cannot be spliced
  // into a JPEG, so fail-safe to Level-B (serve the ORIGINAL PNG byte-for-byte,
  // manifest intact) rather than emit a JPEG with corrupt PNG chunks or a
  // stripped manifest. Regression guard for the cross-format splice bug.
  GoogleString original;
  ASSERT_TRUE(file_system_.ReadFile(
      StrCat(GTestSrcDir(), kTestData, kBikeCrash).c_str(), &original,
      &message_handler_));
  const GoogleString with_c2pa = SpliceC2paCaBxIntoPng(original, 0);

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_png = true;
  options->convert_png_to_jpeg = true;
  options->jpeg_quality = 85;
  EXPECT_TRUE(options->preserve_c2pa);
  options->c2pa_carry = true;
  ImagePtr image(NewImage(with_c2pa, "carry-png2jpeg", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());

  // Served the ORIGINAL PNG (Level-B skip): stayed PNG, byte-identical.
  EXPECT_EQ(IMAGE_PNG, image->image_type());
  EXPECT_EQ(with_c2pa, out);
}

TEST_F(ImageTest, CarryC2paPngCleanImageRecompresses) {
  // Carry ON but NO manifest: ordinary optimization (the carry gate is a no-op).
  GoogleString original;
  ASSERT_TRUE(file_system_.ReadFile(
      StrCat(GTestSrcDir(), kTestData, kBikeCrash).c_str(), &original,
      &message_handler_));
  ASSERT_FALSE(pagespeed::image_compression::ImageHasC2paManifest(original));

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_png = true;
  options->c2pa_carry = true;
  ImagePtr image(NewImage(original, "carry-png-clean", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());
  EXPECT_EQ(IMAGE_PNG, image->image_type());
  // No manifest appeared, and the output is a valid (re-readable) PNG.
  EXPECT_FALSE(pagespeed::image_compression::ImageHasC2paManifest(out));
  ImagePtr reread(NewImage(out, "carry-png-clean-reread", GTestTempDir(),
                           new Image::CompressionOptions(), &timer_,
                           &message_handler_));
  EXPECT_EQ(IMAGE_PNG, reread->image_type());
  ImageDim dim;
  dim.Clear();
  reread->Dimensions(&dim);
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
}

TEST_F(ImageTest, CarryC2paLargePngManifestCarriesAndDecodes) {
  // A large (>64KB) single caBX chunk carries verbatim -- PNG chunk lengths are
  // 32-bit, so there is no 64KB-per-segment limit like JPEG APP markers -- and
  // the output still decodes.
  GoogleString original;
  ASSERT_TRUE(file_system_.ReadFile(
      StrCat(GTestSrcDir(), kTestData, kBikeCrash).c_str(), &original,
      &message_handler_));
  const size_t kBig = 80 * 1024;  // > 64KB of manifest payload.
  const GoogleString with_c2pa = SpliceC2paCaBxIntoPng(original, kBig);
  const StringPieceVector chunks =
      pagespeed::image_compression::ExtractPngC2paChunks(with_c2pa);
  ASSERT_EQ(static_cast<size_t>(1), chunks.size());
  EXPECT_GT(chunks[0].size(), kBig);
  const GoogleString carrier(chunks[0].data(), chunks[0].size());

  Image::CompressionOptions* options = new Image::CompressionOptions();
  options->recompress_png = true;
  options->c2pa_carry = true;
  ImagePtr image(NewImage(with_c2pa, "carry-png-big", GTestTempDir(), options,
                          &timer_, &message_handler_));
  const GoogleString out(image->Contents().data(), image->Contents().size());
  EXPECT_NE(GoogleString::npos, out.find(carrier));  // carried verbatim.

  ImagePtr reread(NewImage(out, "carry-png-big-reread", GTestTempDir(),
                           new Image::CompressionOptions(), &timer_,
                           &message_handler_));
  EXPECT_EQ(IMAGE_PNG, reread->image_type());
  ImageDim dim;
  dim.Clear();
  reread->Dimensions(&dim);
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
}

}  // namespace net_instaweb
