// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>CSS validation and minification utilities. Stateless.</summary>
public static class CssProcessor
{
    /// <summary>
    /// Validate CSS for XSS safety (null bytes, script injection).
    /// </summary>
    public static void Validate(ReadOnlySpan<byte> css)
    {
        NativeCheck.ThrowOnError(
            NativePageSpeed.CssValidate(css, (nuint)css.Length));
    }

    /// <summary>
    /// Minify CSS. Returns the minified result as a byte array.
    /// </summary>
    public static byte[] Minify(ReadOnlySpan<byte> css)
    {
        NativeCheck.ThrowOnError(
            NativePageSpeed.CssMinify(
                css, (nuint)css.Length,
                out var outPtr, out var outLen));
        try
        {
            var result = new byte[(int)outLen];
            unsafe
            {
                new ReadOnlySpan<byte>(
                    (void*)outPtr, (int)outLen).CopyTo(result);
            }
            return result;
        }
        finally
        {
            NativePageSpeed.Free(outPtr);
        }
    }
}
