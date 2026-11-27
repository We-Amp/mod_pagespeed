// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - VTracer FFI Layer
//
// Thin C FFI binding around the VTracer crate for raster-to-SVG
// vectorization.  Exposes a C-compatible interface for consumption
// by the C++ image processing pipeline.
//
// Safety invariants:
// - All public functions validate pointers for null before dereference
// - All public functions validate dimensions (width/height < 65536)
// - panic = "abort" in Cargo.toml prevents unwinding across FFI boundary
// - All errors are returned as error codes, never panics

use std::slice;

/// Error codes returned by VTracer FFI functions.
pub const VTRACER_OK: i32 = 0;
pub const VTRACER_INVALID_INPUT: i32 = 1;
pub const VTRACER_TOO_MANY_PATHS: i32 = 2;
pub const VTRACER_INTERNAL_ERROR: i32 = 3;

/// Maximum dimension (width or height) accepted.
const MAX_DIMENSION: u32 = 65535;

/// Maximum number of SVG paths before we consider the output too complex.
const MAX_PATH_COUNT: u32 = 100_000;

/// Configuration for VTracer conversion.
#[repr(C)]
pub struct VTracerConfig {
    /// Color precision: 0-8, number of significant bits per RGB channel.
    pub color_precision: u32,
    /// Minimum area of speckle to filter out (pixels).
    pub filter_speckle: u32,
    /// Corner threshold: 0-180 degrees.
    pub corner_threshold: u32,
    /// Minimum segment length.
    pub segment_length: f64,
    /// Splice threshold: 0-180 degrees.
    pub splice_threshold: u32,
    /// Conversion mode: 0=spline, 1=polygon, 2=pixel.
    pub mode: u32,
}

/// Result from VTracer conversion.  Caller must free via vtracer_free().
#[repr(C)]
pub struct VTracerResult {
    /// Pointer to SVG data (UTF-8 encoded).  Null on error.
    pub svg_data: *mut u8,
    /// Length of SVG data in bytes.
    pub svg_len: usize,
    /// Number of SVG path elements in the output.
    pub path_count: u32,
    /// Error code: 0=success, 1=invalid_input, 2=too_many_paths,
    /// 3=internal_error.
    pub error_code: i32,
}

/// Populate a VTracerConfig with default values.
///
/// # Safety
/// `config` must point to a valid, writable VTracerConfig.
#[no_mangle]
pub unsafe extern "C" fn vtracer_default_config(config: *mut VTracerConfig) {
    if config.is_null() {
        return;
    }
    let config = &mut *config;
    config.color_precision = 6;
    config.filter_speckle = 4;
    config.corner_threshold = 60;
    config.segment_length = 4.0;
    config.splice_threshold = 45;
    config.mode = 0; // spline
}

/// Convert a raster pixel buffer to SVG using VTracer.
///
/// # Parameters
/// - `pixels`: Pointer to raw pixel data (RGB or RGBA).
/// - `width`: Image width in pixels (must be > 0 and < 65536).
/// - `height`: Image height in pixels (must be > 0 and < 65536).
/// - `channels`: Number of channels per pixel (3=RGB, 4=RGBA).
/// - `config`: Pointer to VTracerConfig (uses defaults if null).
/// - `result`: Pointer to VTracerResult to be filled in.
///
/// # Returns
/// 0 on success, non-zero error code on failure.
///
/// # Safety
/// - `pixels` must point to at least `width * height * channels` bytes.
/// - `result` must point to a valid, writable VTracerResult.
#[no_mangle]
pub unsafe extern "C" fn vtracer_convert(
    pixels: *const u8,
    width: u32,
    height: u32,
    channels: u32,
    config: *const VTracerConfig,
    result: *mut VTracerResult,
) -> i32 {
    // Validate result pointer first so we can report errors.
    if result.is_null() {
        return VTRACER_INVALID_INPUT;
    }
    let result = &mut *result;
    result.svg_data = std::ptr::null_mut();
    result.svg_len = 0;
    result.path_count = 0;
    result.error_code = VTRACER_OK;

    // Validate input parameters.
    if pixels.is_null() {
        result.error_code = VTRACER_INVALID_INPUT;
        return VTRACER_INVALID_INPUT;
    }
    if width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION {
        result.error_code = VTRACER_INVALID_INPUT;
        return VTRACER_INVALID_INPUT;
    }
    if channels != 3 && channels != 4 {
        result.error_code = VTRACER_INVALID_INPUT;
        return VTRACER_INVALID_INPUT;
    }

    // Compute total bytes and check for overflow.
    let total_pixels = (width as u64) * (height as u64);
    let total_bytes = total_pixels * (channels as u64);
    if total_bytes > usize::MAX as u64 {
        result.error_code = VTRACER_INVALID_INPUT;
        return VTRACER_INVALID_INPUT;
    }

    // Build the default config if none provided.
    let default_config = VTracerConfig {
        color_precision: 6,
        filter_speckle: 4,
        corner_threshold: 60,
        segment_length: 4.0,
        splice_threshold: 45,
        mode: 0,
    };
    let cfg = if config.is_null() {
        &default_config
    } else {
        &*config
    };

    // Read pixel data into a Rust slice.
    let pixel_slice = slice::from_raw_parts(pixels, total_bytes as usize);

    // Convert to RGBA if input is RGB (VTracer needs RGBA).
    let rgba_pixels: Vec<u8> = if channels == 3 {
        let mut rgba = Vec::with_capacity((total_pixels * 4) as usize);
        for i in 0..total_pixels as usize {
            rgba.push(pixel_slice[i * 3]);
            rgba.push(pixel_slice[i * 3 + 1]);
            rgba.push(pixel_slice[i * 3 + 2]);
            rgba.push(255); // Fully opaque alpha
        }
        rgba
    } else {
        pixel_slice.to_vec()
    };

    // Build VTracer configuration.
    let color_mode = match cfg.mode {
        1 => vtracer::ColorMode::Binary,
        _ => vtracer::ColorMode::Color,
    };
    let hierarchical = match cfg.mode {
        1 => vtracer::Hierarchical::Cutout,
        _ => vtracer::Hierarchical::Stacked,
    };
    let path_precision = match cfg.mode {
        2 => Some(0u32), // pixel mode: integer coordinates
        _ => None,
    };

    // Build VTracer config. The mode field uses PathSimplifyMode from the
    // visioncortex crate (re-exported through vtracer's Config struct).
    // We need to construct it through the Config's Default and then
    // override fields individually.
    let mut vtracer_config = vtracer::Config::default();
    vtracer_config.color_mode = color_mode;
    vtracer_config.hierarchical = hierarchical;
    vtracer_config.filter_speckle = cfg.filter_speckle as usize;
    vtracer_config.color_precision = cfg.color_precision as i32;
    vtracer_config.layer_difference = 16;
    vtracer_config.corner_threshold = cfg.corner_threshold as i32;
    vtracer_config.length_threshold = cfg.segment_length;
    vtracer_config.max_iterations = 10;
    vtracer_config.splice_threshold = cfg.splice_threshold as i32;
    vtracer_config.path_precision = path_precision;

    // Construct a ColorImage for VTracer.
    let color_image = vtracer::ColorImage {
        pixels: rgba_pixels,
        width: width as usize,
        height: height as usize,
    };

    // Run the conversion.
    let svg_result = vtracer::convert(color_image, vtracer_config);

    match svg_result {
        Ok(svg_file) => {
            let svg_string = svg_file.to_string();

            // Count path elements (approximate by counting "<path" occurrences).
            let path_count = svg_string.matches("<path").count() as u32;

            if path_count > MAX_PATH_COUNT {
                result.error_code = VTRACER_TOO_MANY_PATHS;
                result.path_count = path_count;
                return VTRACER_TOO_MANY_PATHS;
            }

            // Allocate output buffer and copy SVG data.
            let svg_bytes = svg_string.into_bytes();
            let len = svg_bytes.len();
            let buf = Box::into_raw(svg_bytes.into_boxed_slice());

            result.svg_data = buf as *mut u8;
            result.svg_len = len;
            result.path_count = path_count;
            result.error_code = VTRACER_OK;
            VTRACER_OK
        }
        Err(_e) => {
            result.error_code = VTRACER_INTERNAL_ERROR;
            VTRACER_INTERNAL_ERROR
        }
    }
}

/// Free memory allocated by vtracer_convert().
///
/// # Safety
/// `result` must point to a VTracerResult previously filled by
/// vtracer_convert(), or be null (no-op).  Must not be called twice
/// on the same result.
#[no_mangle]
pub unsafe extern "C" fn vtracer_free(result: *mut VTracerResult) {
    if result.is_null() {
        return;
    }
    let result = &mut *result;
    if !result.svg_data.is_null() && result.svg_len > 0 {
        // Reconstruct the Box<[u8]> and drop it to free the memory.
        let _ = Box::from_raw(slice::from_raw_parts_mut(
            result.svg_data,
            result.svg_len,
        ));
        result.svg_data = std::ptr::null_mut();
        result.svg_len = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let mut config = VTracerConfig {
            color_precision: 0,
            filter_speckle: 0,
            corner_threshold: 0,
            segment_length: 0.0,
            splice_threshold: 0,
            mode: 0,
        };
        unsafe {
            vtracer_default_config(&mut config);
        }
        assert_eq!(config.color_precision, 6);
        assert_eq!(config.filter_speckle, 4);
        assert_eq!(config.corner_threshold, 60);
        assert!((config.segment_length - 4.0).abs() < f64::EPSILON);
        assert_eq!(config.splice_threshold, 45);
        assert_eq!(config.mode, 0);
    }

    #[test]
    fn test_null_pixels_returns_invalid_input() {
        let mut result = VTracerResult {
            svg_data: std::ptr::null_mut(),
            svg_len: 0,
            path_count: 0,
            error_code: 0,
        };
        let code = unsafe {
            vtracer_convert(
                std::ptr::null(),
                100,
                100,
                4,
                std::ptr::null(),
                &mut result,
            )
        };
        assert_eq!(code, VTRACER_INVALID_INPUT);
        assert_eq!(result.error_code, VTRACER_INVALID_INPUT);
    }

    #[test]
    fn test_null_result_returns_invalid_input() {
        let pixels = vec![0u8; 4];
        let code = unsafe {
            vtracer_convert(
                pixels.as_ptr(),
                1,
                1,
                4,
                std::ptr::null(),
                std::ptr::null_mut(),
            )
        };
        assert_eq!(code, VTRACER_INVALID_INPUT);
    }

    #[test]
    fn test_zero_dimensions_returns_invalid_input() {
        let pixels = vec![0u8; 4];
        let mut result = VTracerResult {
            svg_data: std::ptr::null_mut(),
            svg_len: 0,
            path_count: 0,
            error_code: 0,
        };
        let code = unsafe {
            vtracer_convert(
                pixels.as_ptr(),
                0,
                100,
                4,
                std::ptr::null(),
                &mut result,
            )
        };
        assert_eq!(code, VTRACER_INVALID_INPUT);
    }

    #[test]
    fn test_invalid_channels_returns_invalid_input() {
        let pixels = vec![0u8; 200];
        let mut result = VTracerResult {
            svg_data: std::ptr::null_mut(),
            svg_len: 0,
            path_count: 0,
            error_code: 0,
        };
        let code = unsafe {
            vtracer_convert(
                pixels.as_ptr(),
                10,
                5,
                2, // Invalid: must be 3 or 4
                std::ptr::null(),
                &mut result,
            )
        };
        assert_eq!(code, VTRACER_INVALID_INPUT);
    }

    #[test]
    fn test_oversized_dimensions_returns_invalid_input() {
        let pixels = vec![0u8; 4];
        let mut result = VTracerResult {
            svg_data: std::ptr::null_mut(),
            svg_len: 0,
            path_count: 0,
            error_code: 0,
        };
        let code = unsafe {
            vtracer_convert(
                pixels.as_ptr(),
                65536, // Exceeds MAX_DIMENSION
                1,
                4,
                std::ptr::null(),
                &mut result,
            )
        };
        assert_eq!(code, VTRACER_INVALID_INPUT);
    }

    #[test]
    fn test_small_rgba_image_converts_successfully() {
        // Create a simple 4x4 red RGBA image.
        let width = 4u32;
        let height = 4u32;
        let mut pixels = Vec::with_capacity((width * height * 4) as usize);
        for _ in 0..(width * height) {
            pixels.push(255); // R
            pixels.push(0); // G
            pixels.push(0); // B
            pixels.push(255); // A
        }

        let mut result = VTracerResult {
            svg_data: std::ptr::null_mut(),
            svg_len: 0,
            path_count: 0,
            error_code: 0,
        };

        let code = unsafe {
            vtracer_convert(
                pixels.as_ptr(),
                width,
                height,
                4,
                std::ptr::null(), // Use defaults
                &mut result,
            )
        };

        assert_eq!(code, VTRACER_OK);
        assert_eq!(result.error_code, VTRACER_OK);
        assert!(!result.svg_data.is_null());
        assert!(result.svg_len > 0);

        // Verify it's valid SVG-ish content.
        let svg_str = unsafe {
            std::str::from_utf8(slice::from_raw_parts(result.svg_data, result.svg_len))
                .unwrap()
        };
        assert!(svg_str.contains("<svg"));
        assert!(svg_str.contains("</svg>"));

        // Clean up.
        unsafe {
            vtracer_free(&mut result);
        }
        assert!(result.svg_data.is_null());
        assert_eq!(result.svg_len, 0);
    }

    #[test]
    fn test_small_rgb_image_converts_successfully() {
        // Create a simple 4x4 blue RGB image.
        let width = 4u32;
        let height = 4u32;
        let mut pixels = Vec::with_capacity((width * height * 3) as usize);
        for _ in 0..(width * height) {
            pixels.push(0); // R
            pixels.push(0); // G
            pixels.push(255); // B
        }

        let mut result = VTracerResult {
            svg_data: std::ptr::null_mut(),
            svg_len: 0,
            path_count: 0,
            error_code: 0,
        };

        let code = unsafe {
            vtracer_convert(
                pixels.as_ptr(),
                width,
                height,
                3,
                std::ptr::null(),
                &mut result,
            )
        };

        assert_eq!(code, VTRACER_OK);
        assert_eq!(result.error_code, VTRACER_OK);
        assert!(!result.svg_data.is_null());
        assert!(result.svg_len > 0);

        unsafe {
            vtracer_free(&mut result);
        }
    }

    #[test]
    fn test_free_null_result_is_noop() {
        unsafe {
            vtracer_free(std::ptr::null_mut());
        }
        // Should not crash.
    }

    #[test]
    fn test_free_zeroed_result_is_noop() {
        let mut result = VTracerResult {
            svg_data: std::ptr::null_mut(),
            svg_len: 0,
            path_count: 0,
            error_code: 0,
        };
        unsafe {
            vtracer_free(&mut result);
        }
        // Should not crash.
    }

    #[test]
    fn test_default_config_null_is_noop() {
        unsafe {
            vtracer_default_config(std::ptr::null_mut());
        }
        // Should not crash.
    }
}
