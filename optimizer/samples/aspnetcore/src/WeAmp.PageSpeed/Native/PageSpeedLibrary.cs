// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace WeAmp.PageSpeed.Native;

internal static class PageSpeedLibrary
{
    internal const string Name = "pagespeed";
    private static int _resolverSet;

    [ModuleInitializer]
    internal static void Initialize()
    {
        if (Interlocked.Exchange(ref _resolverSet, 1) == 0)
        {
            NativeLibrary.SetDllImportResolver(
                typeof(PageSpeedLibrary).Assembly,
                ResolveLibrary);
        }
    }

    private static IntPtr ResolveLibrary(
        string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != Name) return IntPtr.Zero;

        // 1. Try runtimes/{rid}/native/ (NuGet package layout).
        //
        // AppContext.BaseDirectory is the app's output directory under both
        // classic (Assembly.Location-equivalent) and single-file/AOT layouts,
        // whereas Assembly.Location returns an empty string when embedded in
        // a single-file bundle (IL3000). MSBuild's NuGet runtime resolution
        // copies WeAmp.PageSpeed.NativeAssets.* binaries to
        //   <appdir>/runtimes/<rid>/native/
        // so this path resolves identically across layouts.
        var rid = RuntimeInformation.RuntimeIdentifier;
        var appDir = AppContext.BaseDirectory;
        var nugetPath = Path.Combine(appDir, "runtimes", rid, "native",
            NativeLibraryName());
        if (NativeLibrary.TryLoad(nugetPath, out var h))
            return h;

        // 2. Fall through to default resolution (LD_LIBRARY_PATH, system paths)
        return IntPtr.Zero;
    }

    private static string NativeLibraryName() =>
        RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
            ? "pagespeed.dll"
            : RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
                ? "libpagespeed.dylib"
                : "libpagespeed.so";
}
