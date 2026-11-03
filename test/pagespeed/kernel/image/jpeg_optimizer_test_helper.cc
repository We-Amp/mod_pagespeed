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

#include "test/pagespeed/kernel/image/jpeg_optimizer_test_helper.h"

#include <csetjmp>
#include <cstdlib>
#include <vector>

#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/jpeg_reader.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

extern "C" {
#ifdef USE_SYSTEM_LIBJPEG
#include "jpeglib.h"  // NOLINT
#else
#include "jpeglib.h"
#endif
}

using net_instaweb::MockMessageHandler;
using net_instaweb::NullMutex;
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::JpegReader;
using pagespeed::image_compression::JpegScanlineReader;

namespace pagespeed_testing {
namespace image_compression {

// Marker for APPN segment can obtained by adding N to JPEG_APP0. There is no
// direct constant to refer them. The offsets here are part of jpeg codex, for
// example JPEG_APP0 + 2 refers to APP2 which should always correspond to color
// profile information.
const int kColorProfileMarker = JPEG_APP0 + 2;
const int kExifDataMarker = JPEG_APP0 + 1;
// APP11 carries C2PA / Content-Credentials provenance (JUMBF boxes).
const int kC2paMarker = JPEG_APP0 + 11;

bool GetJpegNumComponentsAndSamplingFactors(const GoogleString& jpeg,
                                            int* out_num_components,
                                            int* out_h_samp_factor,
                                            int* out_v_samp_factor) {
  MockMessageHandler message_handler_(new NullMutex);
  JpegReader reader(&message_handler_);
  jpeg_decompress_struct* jpeg_decompress = reader.decompress_struct();

  jmp_buf env;
  if (setjmp(env)) {
    return false;
  }

  // Need to install env so that it will be longjmp()ed to on error.
  jpeg_decompress->client_data = static_cast<void*>(&env);

  reader.PrepareForRead(jpeg.data(), jpeg.size());
  jpeg_read_header(jpeg_decompress, TRUE);
  *out_num_components = jpeg_decompress->num_components;
  *out_h_samp_factor = jpeg_decompress->comp_info[0].h_samp_factor;
  *out_v_samp_factor = jpeg_decompress->comp_info[0].v_samp_factor;
  return true;
}

bool IsJpegSegmentPresent(const GoogleString& data, int segment) {
  MockMessageHandler message_handler_(new NullMutex);
  JpegReader reader(&message_handler_);
  jpeg_decompress_struct* jpeg_decompress = reader.decompress_struct();

  jmp_buf env;
  if (setjmp(env)) {
    return false;
  }

  // Need to install env so that it will be longjmp()ed to on error.
  jpeg_decompress->client_data = static_cast<void*>(&env);

  reader.PrepareForRead(data.data(), data.size());
  jpeg_save_markers(jpeg_decompress, segment, 0xFFFF);
  jpeg_read_header(jpeg_decompress, TRUE);

  bool is_marker_present = false;
  for (jpeg_saved_marker_ptr marker = jpeg_decompress->marker_list;
       marker != nullptr; marker = marker->next) {
    if (marker->marker == segment) {
      is_marker_present = true;
      break;
    }
  }

  return is_marker_present;
}

int GetNumScansInJpeg(const GoogleString& data) {
  MockMessageHandler message_handler_(new NullMutex);
  JpegReader reader(&message_handler_);
  jpeg_decompress_struct* jpeg_decompress = reader.decompress_struct();

  jmp_buf env;
  if (setjmp(env)) {
    return false;
  }

  // Need to install env so that it will be longjmp()ed to on error.
  jpeg_decompress->client_data = static_cast<void*>(&env);

  reader.PrepareForRead(data.data(), data.size());
  jpeg_read_header(jpeg_decompress, TRUE);

  jpeg_decompress->buffered_image = true;
  jpeg_start_decompress(jpeg_decompress);

  int num_scans = 0;
  while (!jpeg_input_complete(jpeg_decompress)) {
    if (jpeg_consume_input(jpeg_decompress) == JPEG_SCAN_COMPLETED) {
      num_scans++;
    }
  }

  return num_scans;
}

namespace {

// Error manager for the encoder below: jumps back instead of exiting.
struct EncodeErrorManager {
  jpeg_error_mgr manager;
  jmp_buf env;
};

void EncodeErrorExit(j_common_ptr cinfo) {
  longjmp(reinterpret_cast<EncodeErrorManager*>(cinfo->err)->env, 1);
}

void EncodeOutputMessage(j_common_ptr cinfo) {}

jpeg_scan_info SingleComponentScan(int ss, int se, int ah, int al) {
  jpeg_scan_info scan = {};
  scan.comps_in_scan = 1;
  scan.component_index[0] = 0;
  scan.Ss = ss;
  scan.Se = se;
  scan.Ah = ah;
  scan.Al = al;
  return scan;
}

// Writes `pixels` (width * height grayscale samples) with the given scan
// script. Kept free of C++ objects with destructors, since an encoder error
// longjmps out of it.
bool EncodeGrayWithScript(const unsigned char* pixels, JDIMENSION width,
                          JDIMENSION height, const jpeg_scan_info* script,
                          int script_length, unsigned char** buffer,
                          unsigned long* size) {  // NOLINT(runtime/int)
  jpeg_compress_struct cinfo;
  EncodeErrorManager error;
  cinfo.err = jpeg_std_error(&error.manager);
  error.manager.error_exit = EncodeErrorExit;
  error.manager.output_message = EncodeOutputMessage;
  if (setjmp(error.env)) {
    jpeg_destroy_compress(&cinfo);
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, buffer, size);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 1;
  cinfo.in_color_space = JCS_GRAYSCALE;
  jpeg_set_defaults(&cinfo);
  cinfo.scan_info = script;
  cinfo.num_scans = script_length;
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(pixels) +
                   static_cast<size_t>(cinfo.next_scanline) * width;
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  return true;
}

}  // namespace

bool EncodeGrayJpegWithScans(const GoogleString& source, int num_scans,
                             GoogleString* out) {
  // Two DC scans, then two scans per AC band (first pass at Al=1, refinement
  // to Al=0). For an odd count the first band starts one bit lower (Al=2)
  // and takes one refinement more. There are 63 AC coefficients, so at most
  // 63 bands.
  const int ac_scans = num_scans - 2;
  const int num_bands = ac_scans / 2;
  if (num_bands < 1 || num_bands > 63) {
    return false;
  }

  MockMessageHandler message_handler(new NullMutex);
  JpegScanlineReader reader(&message_handler);
  if (!reader.Initialize(source.data(), source.size()) ||
      reader.GetPixelFormat() != GRAY_8) {
    return false;
  }
  const size_t width = reader.GetImageWidth();
  const size_t height = reader.GetImageHeight();
  std::vector<unsigned char> pixels;
  pixels.reserve(width * height);
  for (size_t y = 0; y < height; ++y) {
    void* row = nullptr;
    if (!reader.ReadNextScanline(&row)) {
      return false;
    }
    const unsigned char* bytes = static_cast<const unsigned char*>(row);
    pixels.insert(pixels.end(), bytes, bytes + width);
  }

  std::vector<jpeg_scan_info> script;
  script.push_back(SingleComponentScan(0, 0, 0, 1));
  script.push_back(SingleComponentScan(0, 0, 1, 0));
  for (int band = 0; band < num_bands; ++band) {
    const int ss = 1 + band * 63 / num_bands;
    const int se = (band + 1) * 63 / num_bands;
    if (band == 0 && ac_scans % 2 == 1) {
      script.push_back(SingleComponentScan(ss, se, 0, 2));
      script.push_back(SingleComponentScan(ss, se, 2, 1));
    } else {
      script.push_back(SingleComponentScan(ss, se, 0, 1));
    }
    script.push_back(SingleComponentScan(ss, se, 1, 0));
  }
  if (static_cast<int>(script.size()) != num_scans) {
    return false;
  }

  unsigned char* buffer = nullptr;
  unsigned long size = 0;  // NOLINT(runtime/int)
  const bool ok =
      EncodeGrayWithScript(pixels.data(), static_cast<JDIMENSION>(width),
                           static_cast<JDIMENSION>(height), script.data(),
                           static_cast<int>(script.size()), &buffer, &size);
  if (ok) {
    out->assign(reinterpret_cast<const char*>(buffer), size);
  }
  free(buffer);
  return ok;
}

int GetColorProfileMarker() { return kColorProfileMarker; }

int GetExifDataMarker() { return kExifDataMarker; }

int GetC2paMarker() { return kC2paMarker; }

}  // namespace image_compression
}  // namespace pagespeed_testing
