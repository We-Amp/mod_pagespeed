// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;

namespace WeAmp.PageSpeed.Native;

internal static partial class NativePageSpeed
{
    private const string Lib = PageSpeedLibrary.Name;

    // ---------- version ----------

    [LibraryImport(Lib, EntryPoint = "ps_version_major")]
    internal static partial int VersionMajor();

    [LibraryImport(Lib, EntryPoint = "ps_version_minor")]
    internal static partial int VersionMinor();

    [LibraryImport(Lib, EntryPoint = "ps_version_patch")]
    internal static partial int VersionPatch();

    // ---------- error ----------
    // These return const char* to static/thread-local memory.
    // MUST use IntPtr — string return would free the pointer.

    [LibraryImport(Lib, EntryPoint = "ps_error_name")]
    private static partial IntPtr ErrorNamePtr(int err);
    internal static string ErrorName(int err) =>
        Marshal.PtrToStringUTF8(ErrorNamePtr(err)) ?? "";

    [LibraryImport(Lib, EntryPoint = "ps_strerror")]
    private static partial IntPtr StrErrorPtr(int err);
    internal static string StrError(int err) =>
        Marshal.PtrToStringUTF8(StrErrorPtr(err)) ?? "";

    [LibraryImport(Lib, EntryPoint = "ps_last_error_message")]
    private static partial IntPtr LastErrorMessagePtr();
    internal static string? LastErrorMessage()
    {
        var ptr = LastErrorMessagePtr();
        return ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
    }

    // ---------- classification ----------

    [LibraryImport(Lib, EntryPoint = "ps_classify",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint Classify(
        string? accept, string? userAgent,
        string? saveData, string? acceptEncoding);

    [LibraryImport(Lib, EntryPoint = "ps_mask_set_viewport_from_width")]
    internal static partial uint MaskSetViewportFromWidth(
        uint mask, ushort widthPx);

    [LibraryImport(Lib, EntryPoint = "ps_score_alternate")]
    internal static partial int ScoreAlternate(uint clientMask, uint storedMask);

    [LibraryImport(Lib, EntryPoint = "ps_normalize_hostname",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int NormalizeHostname(
        string hostname, Span<byte> outBuf, nuint bufSize);

    [LibraryImport(Lib, EntryPoint = "ps_classify_content_type",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int ClassifyContentType(string contentType);

    // 1 if the Accept header explicitly requests text/markdown
    // (the agent-optimize negotiation signal), else 0. Presence-only,
    // token-bounded, never matched by a wildcard accept.
    [LibraryImport(Lib, EntryPoint = "ps_wants_agent_markdown",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int WantsAgentMarkdown(string? accept);

    // Returns const char* to static string table — use IntPtr.
    [LibraryImport(Lib, EntryPoint = "ps_content_type_mime")]
    private static partial IntPtr ContentTypeMimePtr(int type);
    internal static string ContentTypeMime(int type) =>
        Marshal.PtrToStringUTF8(ContentTypeMimePtr(type)) ?? "";

    // ---------- cache ----------

    [LibraryImport(Lib, EntryPoint = "ps_cache_config_init")]
    internal static partial void CacheConfigInit(ref NativeCacheConfig config);

    [LibraryImport(Lib, EntryPoint = "ps_cache_open")]
    internal static partial int CacheOpen(
        in NativeCacheConfig config, out IntPtr outCache);

    [LibraryImport(Lib, EntryPoint = "ps_cache_close")]
    internal static partial void CacheClose(IntPtr cache);

    // ---------- cache reads ----------

    [LibraryImport(Lib, EntryPoint = "ps_cache_read_best",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheReadBest(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, uint mask, out IntPtr outResult);

    // Agent-aware best read. Routes through the SINGLE audited
    // serve gate (entitlement + content-hash binding); returns PS_ERR_NOT_FOUND
    // for a stale/unbound markdown variant, so the caller falls through to HTML.
    // agentEntitled MUST be derived from ps_read_shared_config_agent_entitled
    // AND ps_wants_agent_markdown — never hard-coded.
    [LibraryImport(Lib, EntryPoint = "ps_cache_read_best_agent",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheReadBestAgent(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, uint mask, int agentEntitled, out IntPtr outResult);

    [LibraryImport(Lib, EntryPoint = "ps_cache_read_alternate",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheReadAlternate(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, byte alternateId, out IntPtr outResult);

    [LibraryImport(Lib, EntryPoint = "ps_cache_read_early_hints",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheReadEarlyHints(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, out IntPtr outResult);

    [LibraryImport(Lib, EntryPoint = "ps_read_content")]
    internal static partial int ReadContent(
        IntPtr result, out IntPtr outData, out nuint outLength);

    [LibraryImport(Lib, EntryPoint = "ps_read_copy")]
    internal static partial int ReadCopy(
        IntPtr result, Span<byte> buf, nuint bufSize,
        out nuint outCopied);

    [LibraryImport(Lib, EntryPoint = "ps_read_mask")]
    internal static partial uint ReadMask(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_read_content_type")]
    internal static partial int ReadContentType(IntPtr result);

    // Returns const char* into result-owned memory — use IntPtr.
    [LibraryImport(Lib, EntryPoint = "ps_read_origin_content_type")]
    private static partial IntPtr ReadOriginContentTypePtr(IntPtr result);
    internal static string? ReadOriginContentType(IntPtr result)
    {
        var ptr = ReadOriginContentTypePtr(result);
        return ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
    }

    [LibraryImport(Lib, EntryPoint = "ps_read_flags")]
    internal static partial byte ReadFlags(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_read_is_ram_hit")]
    internal static partial int ReadIsRamHit(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_read_cache_inserted_at")]
    internal static partial uint ReadCacheInsertedAt(IntPtr result);

    // Origin (unoptimized) content length recorded at worker write time;
    // 0 if NULL or unavailable. Half of the serve-stats gate.
    [LibraryImport(Lib, EntryPoint = "ps_read_origin_content_length")]
    internal static partial uint ReadOriginContentLength(IntPtr result);

    // 1 if the variant was written by the worker (kFlagWorkerProcessed set),
    // else 0; 0 if NULL. The other half of the serve-stats gate.
    [LibraryImport(Lib, EntryPoint = "ps_read_is_worker_processed")]
    internal static partial int ReadIsWorkerProcessed(IntPtr result);

    // Origin Cache-Control directives as a PS_CC_ORIGIN_* bitfield.
    [LibraryImport(Lib, EntryPoint = "ps_read_origin_cc_flags")]
    internal static partial ushort ReadOriginCcFlags(IntPtr result);

    // Origin s-maxage in seconds as recorded with the entry; 0 if absent.
    [LibraryImport(Lib, EntryPoint = "ps_read_origin_s_maxage")]
    internal static partial uint ReadOriginSMaxage(IntPtr result);

    // Re-stamp the read lease pinning this result's mmap borrow
    //. 1 = renewed; 0 = nothing to renew (RAM hit,
    // leases disabled, NULL/invalid). Holders keeping a result alive past
    // ~3.75s must call at a cadence <= 3.75s; renewal stops protecting past
    // the cache's 60s anti-starvation wrap ceiling.
    [LibraryImport(Lib, EntryPoint = "ps_read_renew_lease")]
    internal static partial int ReadRenewLease(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_read_free")]
    internal static partial void ReadFree(IntPtr result);

    // ---------- serve stats ----------
    // The in-process front-end writes the .pagespeed-serve-stats mmap on each
    // worker-processed cache HIT, mirroring nginx. The worker is the sole
    // CREATOR; this only OPENs (returns PS_ERR_NOT_FOUND / NotFound until the
    // worker has created the file). String marshalling matches ps_cache_open's
    // UTF-8 convention used throughout this file.

    [LibraryImport(Lib, EntryPoint = "ps_serve_stats_open",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int ServeStatsOpen(string cachePath, out IntPtr outHandle);

    // ABI: the trailing `mask` (uint32_t in pagespeed.h) lets the native side
    // bump svg.served for SVG image variants (#455). This
    // signature MUST match ps_serve_stats_record_hit exactly or the runtime
    // corrupts the stack.
    [LibraryImport(Lib, EntryPoint = "ps_serve_stats_record_hit")]
    internal static partial void ServeStatsRecordHit(
        IntPtr handle, int contentType, ulong originalBytes, ulong optimizedBytes,
        uint mask);

    [LibraryImport(Lib, EntryPoint = "ps_serve_stats_close")]
    internal static partial void ServeStatsClose(IntPtr handle);

    // ---------- agent_optimize entitlement ----------
    // Worker-written serve-side entitlement under cache_path (the worker is the
    // sole writer). 1 = entitled; 0 = not entitled (incl. a missing/unreadable
    // config — the safe default); -1 = invalid argument. This is the native
    // entitlement source of truth for the in-process .NET front-end.
    [LibraryImport(Lib, EntryPoint = "ps_read_shared_config_agent_entitled",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int ReadSharedConfigAgentEntitled(string cachePath);

    // ---------- cache writes ----------

    [LibraryImport(Lib, EntryPoint = "ps_write_params_init_sized")]
    internal static partial void WriteParamsInitSized(ref NativeWriteParams p, nuint size);

    [LibraryImport(Lib, EntryPoint = "ps_cache_write_begin",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheWriteBegin(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, in NativeWriteParams p, out IntPtr outHandle);

    [LibraryImport(Lib, EntryPoint = "ps_cache_write_sentinel",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheWriteSentinel(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, byte sentinelId, ulong contentLength,
        out IntPtr outHandle);

    [LibraryImport(Lib, EntryPoint = "ps_write_data")]
    internal static partial int WriteData(
        IntPtr handle, ReadOnlySpan<byte> data, nuint length);

    [LibraryImport(Lib, EntryPoint = "ps_write_close")]
    internal static partial int WriteClose(IntPtr handle);

    [LibraryImport(Lib, EntryPoint = "ps_write_abort")]
    internal static partial void WriteAbort(IntPtr handle);

    // ---------- Cache-Control parsing ----------

    // Parse ONE Cache-Control response header line into out, accumulating.
    // Returns 0 (PS_OK) on success, non-zero on error.
    [LibraryImport(Lib, EntryPoint = "ps_parse_cache_control",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int ParseCacheControl(
        string headerValue, ref NativeCacheControl parsed);

    // ---------- response-header block ----------
    // names/values: arrays of `count` NUL-terminated strings (const char* const*).
    // out verdict: ps_sidecar_verdict_t as int; out stored: 0 or 1.
    [LibraryImport(Lib, EntryPoint = "ps_cache_write_headers_sidecar",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheWriteHeadersSidecar(
        SafeCacheHandle cache, string url, string hostname, string scheme,
        IntPtr names, IntPtr values, nuint count,
        out int verdict, out int stored);

    // ---------- cache management ----------

    [LibraryImport(Lib, EntryPoint = "ps_cache_alternate_exists",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheAlternateExists(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, byte alternateId);

    [LibraryImport(Lib, EntryPoint = "ps_cache_remove",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheRemove(
        SafeCacheHandle cache, string url, string hostname,
        string scheme);

    [LibraryImport(Lib, EntryPoint = "ps_cache_list_alternates",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CacheListAlternates(
        SafeCacheHandle cache, string url, string hostname,
        string scheme, out IntPtr outAlternates, out nuint outCount);

    [LibraryImport(Lib, EntryPoint = "ps_alternates_free")]
    internal static partial void AlternatesFree(IntPtr alternates);

    // ref (not out): caller must pre-initialize StructSize for ABI versioning.
    [LibraryImport(Lib, EntryPoint = "ps_cache_stats")]
    internal static partial int CacheStats(
        SafeCacheHandle cache, ref NativeCacheStats stats);

    // ---------- worker notification ----------

    [LibraryImport(Lib, EntryPoint = "ps_notify_worker",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int NotifyWorker(
        string socketPath, string url, string hostname,
        string scheme, int contentType, uint mask);

    // ---------- HTML processing (high-level) ----------

    [LibraryImport(Lib, EntryPoint = "ps_html_config_init")]
    internal static partial void HtmlConfigInit(ref NativeHtmlConfig config);

    [LibraryImport(Lib, EntryPoint = "ps_html_process",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int HtmlProcess(
        ReadOnlySpan<byte> html, nuint htmlLen,
        string url, string hostname,
        in NativeHtmlConfig config, SafeCacheHandle? cache,
        out IntPtr outResult);

    [LibraryImport(Lib, EntryPoint = "ps_html_result_output")]
    internal static partial IntPtr HtmlResultOutput(
        IntPtr result, out nuint outLen);

    [LibraryImport(Lib, EntryPoint = "ps_html_result_modified")]
    internal static partial int HtmlResultModified(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_html_result_has_critical_css")]
    internal static partial int HtmlResultHasCriticalCss(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_html_result_early_hints")]
    internal static partial IntPtr HtmlResultEarlyHints(
        IntPtr result, out nuint outLen);

    [LibraryImport(Lib, EntryPoint = "ps_html_result_needs_revalidation")]
    internal static partial int HtmlResultNeedsRevalidation(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_html_result_free")]
    internal static partial void HtmlResultFree(IntPtr result);

    // ---------- Async CSS loader (same-origin path + JS) ----------
    // const char* to static memory — use IntPtr (string return would free it).
    [LibraryImport(Lib, EntryPoint = "ps_async_css_loader_path")]
    private static partial IntPtr AsyncCssLoaderPathPtr();
    internal static string AsyncCssLoaderPath() =>
        Marshal.PtrToStringUTF8(AsyncCssLoaderPathPtr()) ?? "";

    [LibraryImport(Lib, EntryPoint = "ps_async_css_loader_js")]
    private static partial IntPtr AsyncCssLoaderJsPtr();
    internal static string AsyncCssLoaderJs() =>
        Marshal.PtrToStringUTF8(AsyncCssLoaderJsPtr()) ?? "";

    // ---------- HTML scanner ----------

    [LibraryImport(Lib, EntryPoint = "ps_html_scan",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int HtmlScan(
        ReadOnlySpan<byte> html, nuint htmlLen,
        string url, out IntPtr outResult);

    [LibraryImport(Lib, EntryPoint = "ps_scan_element_count")]
    internal static partial nuint ScanElementCount(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_scan_element")]
    internal static partial int ScanElement(
        IntPtr result, nuint index,
        out IntPtr outTag, out IntPtr outId,
        out int outDepth, out int outElementIndex);

    // out_classes is const char*** (pointer to null-terminated array of strings).
    // Returns count. Consumer must read count IntPtrs from outClasses.
    [LibraryImport(Lib, EntryPoint = "ps_scan_element_classes")]
    internal static partial nuint ScanElementClasses(
        IntPtr result, nuint index, out IntPtr outClasses);

    [LibraryImport(Lib, EntryPoint = "ps_scan_stylesheet_count")]
    internal static partial nuint ScanStylesheetCount(IntPtr result);

    [LibraryImport(Lib, EntryPoint = "ps_scan_stylesheet")]
    internal static partial int ScanStylesheet(
        IntPtr result, nuint index,
        out IntPtr outHref, out IntPtr outMedia);

    [LibraryImport(Lib, EntryPoint = "ps_scan_inline_css")]
    internal static partial IntPtr ScanInlineCss(
        IntPtr result, out nuint outLen);

    [LibraryImport(Lib, EntryPoint = "ps_scan_lcp_candidate")]
    internal static partial IntPtr ScanLcpCandidate(
        IntPtr result, out IntPtr outSrcset,
        out IntPtr outSizes, out int outElementIndex);

    [LibraryImport(Lib, EntryPoint = "ps_scan_origin_count")]
    internal static partial nuint ScanOriginCount(IntPtr result);

    // Returns const char* into result-owned memory — use IntPtr.
    [LibraryImport(Lib, EntryPoint = "ps_scan_origin")]
    private static partial IntPtr ScanOriginPtr(IntPtr result, nuint index);
    internal static string? ScanOrigin(IntPtr result, nuint index)
    {
        var ptr = ScanOriginPtr(result, index);
        return ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
    }

    [LibraryImport(Lib, EntryPoint = "ps_scan_result_free")]
    internal static partial void ScanResultFree(IntPtr result);

    // ---------- critical CSS ----------

    [LibraryImport(Lib, EntryPoint = "ps_critical_css_config_init")]
    internal static partial void CriticalCssConfigInit(
        ref NativeCriticalCssConfig config);

    [LibraryImport(Lib, EntryPoint = "ps_css_extract_critical")]
    internal static partial int CssExtractCritical(
        IntPtr scanResult,
        ReadOnlySpan<byte> css, nuint cssLen,
        in NativeCriticalCssConfig config, out IntPtr outResult);

    [LibraryImport(Lib, EntryPoint = "ps_critical_css_output")]
    internal static partial IntPtr CriticalCssOutput(
        IntPtr result, out nuint outLen);

    [LibraryImport(Lib, EntryPoint = "ps_critical_css_stats")]
    internal static partial void CriticalCssStats(
        IntPtr result, out int outTotalRules, out int outCriticalRules);

    [LibraryImport(Lib, EntryPoint = "ps_critical_css_result_free")]
    internal static partial void CriticalCssResultFree(IntPtr result);

    // ---------- CSS processing ----------

    [LibraryImport(Lib, EntryPoint = "ps_css_validate")]
    internal static partial int CssValidate(
        ReadOnlySpan<byte> css, nuint cssLen);

    // Callback type for CSS import resolution.
    // Returns const char* (caller-owned), sets *out_len.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal unsafe delegate IntPtr PsCssLookupFn(
        IntPtr url, nuint* outLen, IntPtr userData);

    [LibraryImport(Lib, EntryPoint = "ps_css_flatten_imports",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CssFlattenImports(
        ReadOnlySpan<byte> css, nuint cssLen,
        string cssUrl,
        IntPtr lookup, IntPtr userData,
        int maxDepth,
        out IntPtr outCss, out nuint outLen,
        out int outResolved, out int outUnresolved);

    [LibraryImport(Lib, EntryPoint = "ps_css_minify")]
    internal static partial int CssMinify(
        ReadOnlySpan<byte> css, nuint cssLen,
        out IntPtr outCss, out nuint outLen);

    [LibraryImport(Lib, EntryPoint = "ps_free")]
    internal static partial void Free(IntPtr ptr);

    // ---------- HTML transform ----------

    [LibraryImport(Lib, EntryPoint = "ps_html_transform_create",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int HtmlTransformCreate(
        IntPtr scanResult, in NativeHtmlConfig config,
        ReadOnlySpan<byte> criticalCss, nuint criticalCssLen,
        SafeCacheHandle? cache, string hostname,
        ReadOnlySpan<byte> speculationUrls, nuint speculationUrlsLen,
        out IntPtr outTransform);

    // out_html is caller-freed via ps_free().
    [LibraryImport(Lib, EntryPoint = "ps_html_transform_run",
        StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int HtmlTransformRun(
        IntPtr transform,
        ReadOnlySpan<byte> html, nuint htmlLen,
        string url, out IntPtr outHtml, out nuint outLen);

    [LibraryImport(Lib, EntryPoint = "ps_html_transform_modified")]
    internal static partial int HtmlTransformModified(IntPtr transform);

    [LibraryImport(Lib, EntryPoint = "ps_html_transform_free")]
    internal static partial void HtmlTransformFree(IntPtr transform);
}
