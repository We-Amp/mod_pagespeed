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

// Unit test for image_url_encoder.

#include "net/instaweb/rewriter/public/image_url_encoder.h"

#include <set>

#include "net/instaweb/rewriter/cached_result.pb.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kDimsUrl[] = "17x33x,hencoded.url,_with,_various.stuff";
const char kNoDimsUrl[] = "x,hencoded.url,_with,_various.stuff";
const char kActualUrl[] = "http://encoded.url/with/various.stuff";

}  // namespace

class ImageUrlEncoderTest : public ::testing::Test {
 protected:
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
    bool result = encoder_.Decode(encoded, &urls, &context, &handler_);
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

  bool IsPagespeedWebp(StringPiece url) {
    GoogleUrl gurl(url);
    return ImageUrlEncoder::IsWebpRewrittenUrl(gurl);
  }

  bool IsPagespeedAvif(StringPiece url) {
    GoogleUrl gurl(url);
    return ImageUrlEncoder::IsAvifRewrittenUrl(gurl);
  }

  ImageUrlEncoder encoder_;
  GoogleMessageHandler handler_;
};

TEST_F(ImageUrlEncoderTest, TestEncodingAndDecoding) {
  GoogleString kOriginalUrl = "a.jpg";
  StringVector url_vector;
  url_vector.push_back(kOriginalUrl);

  GoogleString encoded_url;

  ResourceContext context;
  context.set_libwebp_level(ResourceContext::LIBWEBP_LOSSY_ONLY);
  context.set_mobile_user_agent(true);

  ImageDim dim;
  dim.set_width(1024);
  dim.set_height(768);
  *context.mutable_desired_image_dims() = dim;

  encoder_.Encode(url_vector, &context, &encoded_url);
  EXPECT_EQ("1024x768xa.jpg", encoded_url);

  encoder_.Decode(encoded_url, &url_vector, &context, &handler_);

  // Check the resource context returned.
  EXPECT_EQ(context.libwebp_level(), ResourceContext::LIBWEBP_LOSSY_ONLY);
  EXPECT_TRUE(context.mobile_user_agent());

  // Check the decoded url after encoding is the same as original.
  GoogleString decoded_url = url_vector.back();
  EXPECT_EQ(kOriginalUrl, decoded_url);
}

TEST_F(ImageUrlEncoderTest, TestEncodingAndDecodingWithoutWebpAndMobileUA) {
  GoogleString kOriginalUrl = "a.jpg";
  StringVector url_vector;
  url_vector.push_back(kOriginalUrl);

  GoogleString encoded_url;

  ResourceContext context;
  context.set_libwebp_level(ResourceContext::LIBWEBP_NONE);
  context.set_mobile_user_agent(false);

  ImageDim dim;
  dim.set_width(1024);
  dim.set_height(768);
  *context.mutable_desired_image_dims() = dim;

  encoder_.Encode(url_vector, &context, &encoded_url);
  EXPECT_EQ("1024x768xa.jpg", encoded_url);

  encoder_.Decode(encoded_url, &url_vector, &context, &handler_);

  // Check the resource context returned.
  EXPECT_EQ(context.libwebp_level(), ResourceContext::LIBWEBP_NONE);
  EXPECT_FALSE(context.mobile_user_agent());

  // Check the decoded url after encoding is the same as original.
  GoogleString decoded_url = url_vector.back();
  EXPECT_EQ(kOriginalUrl, decoded_url);
}

TEST_F(ImageUrlEncoderTest, TestLegacyMobileWebpDecoding) {
  StringPiece kEncodedUrl = "1024x768mwa.jpg";
  StringVector url_vector;
  ResourceContext context;

  context.set_libwebp_level(ResourceContext::LIBWEBP_LOSSY_ONLY);
  encoder_.Decode(kEncodedUrl, &url_vector, &context, &handler_);
  EXPECT_EQ(context.libwebp_level(), ResourceContext::LIBWEBP_LOSSY_ONLY);
  EXPECT_TRUE(context.mobile_user_agent());

  GoogleString decoded_url = url_vector.back();
  EXPECT_EQ("a.jpg", decoded_url);
}

TEST_F(ImageUrlEncoderTest, TestLegacyMobileDecoding) {
  StringPiece kEncodedUrl = "1024x768mxa.jpg";
  StringVector url_vector;
  ResourceContext context;

  // Set webp lossy similar to the ImageRewriteFilter flow.
  context.set_libwebp_level(ResourceContext::LIBWEBP_LOSSY_ONLY);
  encoder_.Decode(kEncodedUrl, &url_vector, &context, &handler_);
  // While decoding in ImageUrlEncoder, we don't unset libwep_level if
  // already set and no encoding char("w"/"v") is found, we get a false
  // ResourceContext, but its alright since the UA is a webp-capable one.

  EXPECT_EQ(context.libwebp_level(), ResourceContext::LIBWEBP_LOSSY_ONLY);
  EXPECT_TRUE(context.mobile_user_agent());

  GoogleString decoded_url = url_vector.back();
  EXPECT_EQ("a.jpg", decoded_url);
}

TEST_F(ImageUrlEncoderTest, NoDimsWebpOrMobile) {
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kNoDimsUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, NoDimsWebpLa) {
  const char kLegacyWebpLaNoMobileUrl[] = "v,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(
      DecodeUrlAndDimensions(kLegacyWebpLaNoMobileUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, NoDimsWebpLaMobile) {
  const char kLegacyWebpLaMobileUrl[] = "mv,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(
      DecodeUrlAndDimensions(kLegacyWebpLaMobileUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasDims) {
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kDimsUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasDimsWebp) {
  const char kLegacyWebpUrl[] = "17x33w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kLegacyWebpUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasDimsWebpLa) {
  const char kLegacyWebpLaNoMobileUrl[] =
      "17x33v,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(
      DecodeUrlAndDimensions(kLegacyWebpLaNoMobileUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasDimsMobile) {
  const char kLegacyMobileUrl[] = "17x33mx,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kLegacyMobileUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasDimsWebpMobile) {
  const char kLegacyWebpMobileUrl[] =
      "17x33mw,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kLegacyWebpMobileUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasDimsWebpLaMobile) {
  const char kLegacyWebpLaMobileUrl[] =
      "17x33mv,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(
      DecodeUrlAndDimensions(kLegacyWebpLaMobileUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasWidth) {
  const char kWidthUrl[] = "17xNx,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kWidthUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(-1, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kWidthUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasWidthWebp) {
  const char kLegacyWebpWidthUrl[] = "17xNw,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kLegacyWebpWidthUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(-1, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  const char kWidthUrl[] = "17xNx,hencoded.url,_with,_various.stuff";
  EXPECT_EQ(kWidthUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasWidthWebpLa) {
  const char kLegacyWebpLaNoMobileUrl[] =
      "17xNv,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(
      DecodeUrlAndDimensions(kLegacyWebpLaNoMobileUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(-1, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  const char kWidthUrl[] = "17xNx,hencoded.url,_with,_various.stuff";
  EXPECT_EQ(kWidthUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasHeight) {
  const char kHeightUrl[] = "Nx33x,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kHeightUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(-1, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kHeightUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasHeightWebp) {
  const char kLegacyWebpHeightUrl[] = "Nx33w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kLegacyWebpHeightUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(-1, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  const char kHeightUrl[] = "Nx33x,hencoded.url,_with,_various.stuff";
  EXPECT_EQ(kHeightUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, HasHeightWebpLa) {
  const char kLegacyWebpLaNoMobileUrl[] =
      "Nx33v,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(
      DecodeUrlAndDimensions(kLegacyWebpLaNoMobileUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(-1, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  const char kHeightUrl[] = "Nx33x,hencoded.url,_with,_various.stuff";
  EXPECT_EQ(kHeightUrl, EncodeUrlAndDimensions(origin_url, dim));
}

TEST_F(ImageUrlEncoderTest, CacheKey) {
  ResourceContext context;

  EXPECT_EQ(".", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  context.set_may_use_small_screen_quality(true);
  EXPECT_EQ(".ss", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  context.set_may_use_save_data_quality(true);
  context.set_libwebp_level(ResourceContext::LIBWEBP_LOSSY_ONLY);
  EXPECT_EQ("wd", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  context.set_mobile_user_agent(true);
  context.set_libwebp_level(ResourceContext::LIBWEBP_LOSSY_LOSSLESS_ALPHA);
  EXPECT_EQ("vm", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  context.set_may_use_save_data_quality(true);
  context.set_mobile_user_agent(true);
  context.set_libwebp_level(ResourceContext::LIBWEBP_ANIMATED);
  EXPECT_EQ("amd", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  // When both may_use_small_screen_quality and may_use_save_data_quality are
  // set, may_use_save_data_quality takes precedence.
  context.set_may_use_small_screen_quality(true);
  context.set_may_use_save_data_quality(true);
  EXPECT_EQ(".d", ImageUrlEncoder::CacheKeyFromResourceContext(context));
}

// the design record Stream G: the AVIF capability token rides in the metadata cache key
// INDEPENDENTLY of the WebP token, in a disjoint 'A'-prefixed alphabet.
TEST_F(ImageUrlEncoderTest, CacheKeyAvifTokens) {
  ResourceContext context;

  // AVIF_NONE emits no token: pre-AVIF keys are byte-for-byte unchanged.
  context.set_avif_level(ResourceContext::AVIF_NONE);
  EXPECT_EQ(".", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  // Per-level AVIF tokens: 'Af' lossy, 'Ag' lossy+lossless+alpha,
  // 'Ah' animated (appended after the WebP token, here LIBWEBP_NONE = '.').
  context.set_avif_level(ResourceContext::AVIF_LOSSY_ONLY);
  EXPECT_EQ(".Af", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.set_avif_level(ResourceContext::AVIF_LOSSY_LOSSLESS_ALPHA);
  EXPECT_EQ(".Ag", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.set_avif_level(ResourceContext::AVIF_ANIMATED);
  EXPECT_EQ(".Ah", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  // A both-capable request emits BOTH the 'v' and 'Ag' tokens in one key.
  context.set_libwebp_level(ResourceContext::LIBWEBP_LOSSY_LOSSLESS_ALPHA);
  context.set_avif_level(ResourceContext::AVIF_LOSSY_LOSSLESS_ALPHA);
  EXPECT_EQ("vAg", ImageUrlEncoder::CacheKeyFromResourceContext(context));
  context.Clear();

  // A WebP-only request emits no 'A' token at all (the alphabets are
  // disjoint, so this proves no AVIF contribution to the key).
  context.set_libwebp_level(ResourceContext::LIBWEBP_LOSSY_LOSSLESS_ALPHA);
  GoogleString webp_only_key =
      ImageUrlEncoder::CacheKeyFromResourceContext(context);
  EXPECT_EQ("v", webp_only_key);
  EXPECT_EQ(GoogleString::npos, webp_only_key.find('A'));
  context.Clear();

  // An AVIF-only committed context emits the WebP-NONE token plus the AVIF
  // token, and no other WebP token ('w'/'v'/'a').
  context.set_libwebp_level(ResourceContext::LIBWEBP_NONE);
  context.set_avif_level(ResourceContext::AVIF_LOSSY_LOSSLESS_ALPHA);
  GoogleString avif_only_key =
      ImageUrlEncoder::CacheKeyFromResourceContext(context);
  EXPECT_EQ(".Ag", avif_only_key);
  EXPECT_EQ(GoogleString::npos, avif_only_key.find('w'));
  EXPECT_EQ(GoogleString::npos, avif_only_key.find('v'));

  // The quality-modifier keys are unchanged by the AVIF token, which is
  // always appended last (after 'd'/'ss').
  context.set_may_use_save_data_quality(true);
  EXPECT_EQ(".dAg", ImageUrlEncoder::CacheKeyFromResourceContext(context));
}

// Every (libwebp_level, avif_level) combination must produce a distinct
// metadata cache key -- the two token alphabets are disjoint by design, so no
// AVIF-capable key can ever collide with a WebP-only key (the Stream G
// acceptance that guards against serving an AVIF payload under a WebP key).
TEST_F(ImageUrlEncoderTest, DifferentWebpAndAvifLevelCombinations) {
  std::set<GoogleString> seen;
  int valid_combinations = 0;
  for (int webp_level = ResourceContext::LibWebpLevel_MIN;
       webp_level <= ResourceContext::LibWebpLevel_MAX; ++webp_level) {
    if (!ResourceContext::LibWebpLevel_IsValid(webp_level)) {
      continue;
    }
    for (int avif_level = ResourceContext::AvifLevel_MIN;
         avif_level <= ResourceContext::AvifLevel_MAX; ++avif_level) {
      if (!ResourceContext::AvifLevel_IsValid(avif_level)) {
        continue;
      }
      ResourceContext ctx;
      ctx.set_libwebp_level(
          static_cast<ResourceContext::LibWebpLevel>(webp_level));
      ctx.set_avif_level(static_cast<ResourceContext::AvifLevel>(avif_level));
      GoogleString cache_key =
          ImageUrlEncoder::CacheKeyFromResourceContext(ctx);
      EXPECT_EQ(seen.find(cache_key), seen.end())
          << "duplicate key " << cache_key << " at webp_level=" << webp_level
          << " avif_level=" << avif_level;
      seen.insert(cache_key);
      ++valid_combinations;
    }
  }
  EXPECT_EQ(valid_combinations, static_cast<int>(seen.size()));
}

// A legacy 'w'/'v'-terminated URL committed the resource to WebP before AVIF
// existed, so decoding one must CLEAR any avif_level pre-set on the context:
// the stored key for such a URL never carried an A-token, and leaving the
// level set would recompute a different (self-missing) key.
TEST_F(ImageUrlEncoderTest, LegacyWebpTerminatorClearsAvifLevel) {
  const char kLegacyWebpUrl[] = "17x33w,hencoded.url,_with,_various.stuff";
  const char kLegacyWebpLaUrl[] = "17x33v,hencoded.url,_with,_various.stuff";
  const char* kLegacyWebpUrls[] = {kLegacyWebpUrl, kLegacyWebpLaUrl};
  for (const char* url : kLegacyWebpUrls) {
    ResourceContext context;
    context.set_avif_level(ResourceContext::AVIF_LOSSY_LOSSLESS_ALPHA);
    StringVector urls;
    EXPECT_TRUE(encoder_.Decode(url, &urls, &context, &handler_));
    EXPECT_FALSE(context.has_avif_level()) << url;
    EXPECT_EQ(ResourceContext::AVIF_NONE, context.avif_level()) << url;
  }

  // The legacy 'x' terminator does not commit to any format, so (mirroring
  // how libwebp_level is left alone) a pre-set avif_level survives.
  ResourceContext context;
  context.set_avif_level(ResourceContext::AVIF_LOSSY_LOSSLESS_ALPHA);
  StringVector urls;
  EXPECT_TRUE(encoder_.Decode(kDimsUrl, &urls, &context, &handler_));
  EXPECT_TRUE(context.has_avif_level());
  EXPECT_EQ(ResourceContext::AVIF_LOSSY_LOSSLESS_ALPHA, context.avif_level());
}

TEST_F(ImageUrlEncoderTest, DifferentWebpLevels) {
  // Make sure different levels of WebP support get different cache keys.
  std::set<GoogleString> seen;
  for (int webp_level = ResourceContext::LibWebpLevel_MIN;
       webp_level <= ResourceContext::LibWebpLevel_MAX; ++webp_level) {
    if (ResourceContext::LibWebpLevel_IsValid(webp_level)) {
      ResourceContext ctx;
      ctx.set_libwebp_level(
          static_cast<ResourceContext::LibWebpLevel>(webp_level));
      GoogleString cache_key =
          ImageUrlEncoder::CacheKeyFromResourceContext(ctx);
      EXPECT_EQ(seen.find(cache_key), seen.end());
      seen.insert(cache_key);
    }
  }
}

TEST_F(ImageUrlEncoderTest, BadFirst) {
  const char kBadFirst[] = "badx33x,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageUrlEncoderTest, BadFirstWebp) {
  const char kBadFirst[] = "badx33w,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageUrlEncoderTest, BadFirstWebpLa) {
  const char kBadFirst[] = "badx33v,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageUrlEncoderTest, BadFirstMobile) {
  const char kBadFirst[] = "badx33mx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageUrlEncoderTest, BadFirstWebpMobile) {
  const char kBadFirst[] = "badx33mw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageUrlEncoderTest, BadFirstWebpLaMobile) {
  const char kBadFirst[] = "badx33mv,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageUrlEncoderTest, BadSecond) {
  const char kBadSecond[] = "17xbadx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageUrlEncoderTest, BadSecondWebp) {
  const char kBadSecond[] = "17xbadw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageUrlEncoderTest, BadSecondWebpLa) {
  const char kBadSecond[] = "17xbadv,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageUrlEncoderTest, BadSecondMobile) {
  const char kBadSecond[] = "17xbadmx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageUrlEncoderTest, BadSecondWebpMobile) {
  const char kBadSecond[] = "17xbadmw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageUrlEncoderTest, BadSecondWebpLaMobile) {
  const char kBadSecond[] = "17xbadmv,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageUrlEncoderTest, BadLeadingN) {
  const char kBadLeadingN[] = "Nxw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadLeadingN);
}

TEST_F(ImageUrlEncoderTest, BadMiddleN) {
  const char kBadMiddleN[] = "17xN,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadMiddleN);
}

TEST_F(ImageUrlEncoderTest, NoXs) {
  const char kNoXs[] = ",hencoded.url,_with,_various.stuff";
  ExpectBadDim(kNoXs);
}

TEST_F(ImageUrlEncoderTest, NoXsMobile) {
  const char kNoXs[] = "m,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kNoXs);
}

TEST_F(ImageUrlEncoderTest, BlankSecond) {
  const char kBlankSecond[] = "17xx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageUrlEncoderTest, BadSizeCheck) {
  // Catch case where url size check was inverted.
  const char kBadSize[] = "17xx";
  ExpectBadDim(kBadSize);
}

TEST_F(ImageUrlEncoderTest, BlankSecondWebp) {
  const char kBlankSecond[] = "17xw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageUrlEncoderTest, BlankSecondMobile) {
  const char kBlankSecond[] = "17xmx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageUrlEncoderTest, BlankSecondWebpMobile) {
  const char kBlankSecond[] = "17xmw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageUrlEncoderTest, BlankSecondWebpLaMobile) {
  const char kBlankSecond[] = "17xmv,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageUrlEncoderTest, BadTrailChar) {
  const char kBadDimsUrl[] = "17x33u,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadDimsUrl);
}

TEST_F(ImageUrlEncoderTest, BadInitChar) {
  const char kBadNoDimsUrl[] = "u,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadNoDimsUrl);
}

TEST_F(ImageUrlEncoderTest, BadWidthChar) {
  const char kWidthUrl[] = "17t,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kWidthUrl);
}

TEST_F(ImageUrlEncoderTest, BadHeightChar) {
  const char kHeightUrl[] = "Nx33t,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kHeightUrl);
}

TEST_F(ImageUrlEncoderTest, ShortBothDims) {
  const char kShortUrl[] = "17x33";
  ExpectBadDim(kShortUrl);
}

TEST_F(ImageUrlEncoderTest, ShortWidth) {
  const char kShortWidth[] = "Nx33";
  ExpectBadDim(kShortWidth);
}

TEST_F(ImageUrlEncoderTest, ShortHeight) {
  const char kShortHeight[] = "17xN";
  ExpectBadDim(kShortHeight);
}

TEST_F(ImageUrlEncoderTest, BothDimsMissing) {
  const char kNeitherUrl[] = "NxNx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kNeitherUrl);
}

TEST_F(ImageUrlEncoderTest, VeryShortUrl) {
  const char kVeryShortUrl[] = "7x3";
  ExpectBadDim(kVeryShortUrl);
}

TEST_F(ImageUrlEncoderTest, TruncatedAfterFirstDim) {
  const char kTruncatedUrl[] = "175x";
  ExpectBadDim(kTruncatedUrl);
}

TEST_F(ImageUrlEncoderTest, TruncatedBeforeSep) {
  const char kTruncatedUrl[] = "12500";
  ExpectBadDim(kTruncatedUrl);
}

TEST_F(ImageUrlEncoderTest, WebpDetection) {
  // Valid WebP URL.
  EXPECT_TRUE(IsPagespeedWebp("http://example.com/xa.jpg.pagespeed.ic.0.webp"));
  EXPECT_TRUE(IsPagespeedWebp("http://example.com/xa.png.pagespeed.ic.0.webp"));
  EXPECT_TRUE(IsPagespeedWebp("http://example.com/xa.gif.pagespeed.ic.0.webp"));
  EXPECT_TRUE(
      IsPagespeedWebp("http://example.com/xa.webp.pagespeed.ic.0.webp"));
  EXPECT_TRUE(
      IsPagespeedWebp("http://example.com/17x33a.jpg.pagespeed.ic.0.webp"));

  // Invalid WebP URL.
  EXPECT_FALSE(IsPagespeedWebp("http://example.com/xa.jpg.XXXXXXXX.ic.0.webp"));
  EXPECT_FALSE(IsPagespeedWebp("http://example.com/xa.gif.pagespeed.ic.0.png"));
  EXPECT_FALSE(
      IsPagespeedWebp("http://example.com/xa.png.pagespeed.ic.0.jpeg"));
  EXPECT_FALSE(
      IsPagespeedWebp("http://example.com/xa.jpg.pagespeed.ic.0.jpeg"));
  EXPECT_FALSE(IsPagespeedWebp("http://example.com/foo.webp"));
  EXPECT_FALSE(IsPagespeedWebp("http://example.com/foo.jpg"));
  EXPECT_FALSE(IsPagespeedWebp("http://example.com/x.jpg.pagespeed.cd.0.jpeg"));
  EXPECT_FALSE(IsPagespeedWebp("http://example.com/x.jpg.pagespeed.ce.0.webp"));
}

// the design record: IsAvifRewrittenUrl keys on the committed ".avif" output extension
// of an ImageRewriteFilter ("ic") URL, exactly as IsWebpRewrittenUrl keys on
// ".webp". This drives the committed-URL reconcile in SetAvifCapability.
TEST_F(ImageUrlEncoderTest, AvifDetection) {
  // Valid committed-AVIF URLs.
  EXPECT_TRUE(IsPagespeedAvif("http://example.com/xa.jpg.pagespeed.ic.0.avif"));
  EXPECT_TRUE(IsPagespeedAvif("http://example.com/xa.png.pagespeed.ic.0.avif"));
  EXPECT_TRUE(IsPagespeedAvif("http://example.com/xa.gif.pagespeed.ic.0.avif"));
  EXPECT_TRUE(
      IsPagespeedAvif("http://example.com/xa.avif.pagespeed.ic.0.avif"));
  EXPECT_TRUE(
      IsPagespeedAvif("http://example.com/17x33a.jpg.pagespeed.ic.0.avif"));

  // Invalid: wrong extension, wrong filter id, or not a pagespeed URL at all.
  EXPECT_FALSE(IsPagespeedAvif("http://example.com/xa.jpg.XXXXXXXX.ic.0.avif"));
  EXPECT_FALSE(
      IsPagespeedAvif("http://example.com/xa.jpg.pagespeed.ic.0.webp"));
  EXPECT_FALSE(
      IsPagespeedAvif("http://example.com/xa.avif.pagespeed.ic.0.jpeg"));
  EXPECT_FALSE(IsPagespeedAvif("http://example.com/foo.avif"));
  EXPECT_FALSE(IsPagespeedAvif("http://example.com/foo.jpg"));
  EXPECT_FALSE(IsPagespeedAvif("http://example.com/x.jpg.pagespeed.ce.0.avif"));

  // The WebP and AVIF detectors never both claim one URL.
  EXPECT_FALSE(
      IsPagespeedWebp("http://example.com/xa.jpg.pagespeed.ic.0.avif"));
  EXPECT_FALSE(
      IsPagespeedAvif("http://example.com/xa.jpg.pagespeed.ic.0.webp"));
}

}  // namespace net_instaweb
