// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers.Binary;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;
using System.Text;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Drains worker notifications via a bounded Channel with a persistent
/// connection to the Worker process. Uses Unix domain sockets on Linux/macOS
/// and Named Pipes on Windows. Reconnects automatically on failure.
/// Deduplicates notifications within a cooldown window.
/// </summary>
public sealed class WorkerNotificationService : BackgroundService
{
    private readonly Channel<WorkerNotification> _channel =
        Channel.CreateBounded<WorkerNotification>(
            new BoundedChannelOptions(1024)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
            });
    private static readonly bool IsWindows =
        RuntimeInformation.IsOSPlatform(OSPlatform.Windows);

    private readonly IOptionsMonitor<PageSpeedOptions> _options;
    private readonly InternalWorkerEndpoint _endpoint;
    private readonly ILogger<WorkerNotificationService> _logger;
    private Socket? _socket;           // Unix domain socket (Linux/macOS)
    private NamedPipeClientStream? _pipe; // Named pipe (Windows)
    private string? _connectedPath;

    // Deduplication: skip re-notifying the same URL+hostname+scheme+type within
    // 5 seconds. AgentRequest is part of the key: an agent-markdown
    // notification asks the worker for DIFFERENT work than a normal one for the
    // same URL, so the two must not collapse into a single dedup entry.
    private readonly Dictionary<(string Url, string Hostname, string Scheme, PageSpeedContentType Type, bool AgentRequest), long> _dedup = new();
    private const long DedupCooldownMs = 5000;

    /// <summary>
    /// Creates a new worker notification service.
    /// </summary>
    public WorkerNotificationService(
        IOptionsMonitor<PageSpeedOptions> options,
        InternalWorkerEndpoint endpoint,
        ILogger<WorkerNotificationService> logger)
    {
        _options = options;
        _endpoint = endpoint;
        _logger = logger;
    }

    /// <summary>
    /// Enqueue a notification (non-blocking, drops oldest on overflow).
    /// <paramref name="hostVouched"/> says that the caller checked
    /// <paramref name="hostname"/> against the names the site serves; only
    /// then does the worker run the work that fetches from that host (the
    /// agent render and the llms.txt build). It defaults to unvouched.
    /// </summary>
    public bool TryNotify(
        string url, string hostname, string scheme,
        PageSpeedContentType contentType, uint mask, bool agentRequest = false,
        bool hostVouched = false)
    {
        if (scheme != "http" && scheme != "https") return false;

        return _channel.Writer.TryWrite(
            new WorkerNotification(
                url, hostname, scheme, contentType, mask, agentRequest,
                hostVouched));
    }

    /// <summary>Notifications queued and not yet sent (tests).</summary>
    internal int PendingCount => _channel.Reader.Count;

    /// <inheritdoc/>
    protected override async Task ExecuteAsync(
        CancellationToken stoppingToken)
    {
        // Resolve the worker's IPC endpoint once. WorkerProcessHost pins the
        // path it actually launched the worker with — we must NOT re-derive it
        // from our own IOptionsMonitor instance, because IOptions (used by the
        // host) and IOptionsMonitor (used here) hand out distinct WorkerOptions
        // instances, so an independently-resolved auto-path would point at a
        // socket nobody is listening on. A null pinned path means worker
        // coordination is disabled.
        string? socketPath;
        try
        {
            socketPath = await _endpoint.SocketPathReady.WaitAsync(stoppingToken);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (socketPath == null)
        {
            _logger.LogInformation(
                "Worker coordination disabled (no socket path); "
                + "notifications will be drained and discarded.");
        }

        await foreach (var n in _channel.Reader.ReadAllAsync(stoppingToken))
        {
            if (socketPath == null) continue;

            // Deduplication: skip if same URL+hostname+scheme+type was notified recently.
            var now = Environment.TickCount64;
            var key = (n.Url, n.Hostname, n.Scheme, n.ContentType, n.AgentRequest);
            if (_dedup.TryGetValue(key, out var lastTick) &&
                (now - lastTick) < DedupCooldownMs)
            {
                continue;
            }
            _dedup[key] = now;

            // Periodic cleanup of dedup dictionary.
            if (_dedup.Count > 4096)
                PruneDedupEntries(now);

            try
            {
                await SendNotificationAsync(socketPath, n, stoppingToken);
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex,
                    "Worker notification failed for {Url}", n.Url);
                Disconnect();
            }
        }
    }

    // Wire protocol version — MUST match the worker's src/proto/worker_ipc.h
    // kIpcVersion. The reader rejects any other version outright (no
    // cross-version tolerance), so this is bumped in lockstep with the C++ side.
    private const byte IpcVersion = 6;

    // v6 flags bit 0, PS_NOTIFY_FLAG_HOST_VOUCHED in pagespeed.h.
    private const byte FlagHostVouched = 0x01;

    /// <summary>
    /// Serializes and sends a notification over the persistent socket.
    /// Wire format (big-endian, v6 — matches worker_ipc.h):
    ///   [4: total_length][1: version=6][4: url_length][url][4: host_length][host][1: ct][4: mask][1: scheme_byte][1: agent_request][4: option_context_length][option_context][1: option_signature_length][option_signature][1: site_length][site][1: flags]
    /// total_length covers everything after itself (version + fields).
    /// scheme_byte: 0x01 = http, 0x02 = https (matches C++ IpcScheme).
    /// agent_request: 0/1 (the markdown-render demand bit).
    ///
    /// The v5 option-context fields are emitted EMPTY. This middleware does not
    /// resolve per-request PageSpeed configuration — there is one configuration
    /// for the process — so it has no context to declare, and an empty context
    /// means exactly that. Both length fields are still written: the frame's
    /// shape is fixed by its version, not by which fields happen to carry
    /// anything, and a v5 reader stops at the length its own header declares.
    ///
    /// Supplying a context would mean supplying the payload AND its signature
    /// together; a payload without a signature is refused by the reader,
    /// because the signature can only be checked against the payload it came
    /// with, and that check is the point of carrying it.
    ///
    /// The v6 site is emitted EMPTY: this middleware has no site id, so its
    /// entries stay in the unsited keyspace. The flags byte carries only
    /// HOST_VOUCHED (0x01), set when the middleware checked the host name
    /// against AllowedHosts; the worker runs the agent render and the
    /// llms.txt build only for such a notification. FROM_MODULE and the
    /// library-only bit are never set here.
    /// </summary>
    private async ValueTask SendNotificationAsync(
        string socketPath, WorkerNotification n,
        CancellationToken ct)
    {
        await EnsureConnectedAsync(socketPath, ct);

        var urlByteCount = Encoding.UTF8.GetByteCount(n.Url);
        var hostByteCount = Encoding.UTF8.GetByteCount(n.Hostname);
        // v6 payload after total_length: version(1) + url_len(4) + url +
        // host_len(4) + host + ct(1) + mask(4) + scheme(1) + agent_request(1)
        // + option_context_len(4) + option_context(0) + option_sig_len(1)
        // + option_signature(0) + site_len(1) + site(0) + flags(1)
        var payloadLen =
            1 + 4 + urlByteCount + 4 + hostByteCount + 1 + 4 + 1 + 1 + 4 + 1
            + 1 + 1;
        // total on wire: total_length(4) + payload
        var totalLen = 4 + payloadLen;
        var buffer = new byte[totalLen];
        var span = buffer.AsSpan();

        // total_length (big-endian, not including this 4-byte field itself).
        BinaryPrimitives.WriteInt32BigEndian(span[0..], payloadLen);
        // version byte.
        span[4] = IpcVersion;
        // url_length + url.
        BinaryPrimitives.WriteInt32BigEndian(span[5..], urlByteCount);
        Encoding.UTF8.GetBytes(n.Url, span[9..]);
        var offset = 9 + urlByteCount;
        // hostname_length + hostname.
        BinaryPrimitives.WriteInt32BigEndian(span[offset..], hostByteCount);
        Encoding.UTF8.GetBytes(n.Hostname, span[(offset + 4)..]);
        offset += 4 + hostByteCount;
        // content_type (1 byte).
        span[offset] = (byte)n.ContentType;
        // capability_mask (big-endian).
        BinaryPrimitives.WriteUInt32BigEndian(span[(offset + 1)..], n.Mask);
        // Scheme byte: 0x01 = http, 0x02 = https (matches C++ IpcScheme).
        span[offset + 5] = n.Scheme == "https" ? (byte)0x02 : (byte)0x01;
        // agent_request byte (v4): the markdown-render demand bit.
        span[offset + 6] = n.AgentRequest ? (byte)1 : (byte)0;
        // option_context_length (v5) = 0, then option_signature_length = 0.
        // Written explicitly rather than left to the zero-initialised buffer,
        // so that the frame layout is visible here and a future non-empty
        // context has an obvious place to go.
        BinaryPrimitives.WriteInt32BigEndian(span[(offset + 7)..], 0);
        span[offset + 11] = 0;
        // site_length (v6) = 0: no site. flags (v6): HOST_VOUCHED (bit 0)
        // when the host was checked, nothing else (not the module).
        span[offset + 12] = 0;
        span[offset + 13] = n.HostVouched ? FlagHostVouched : (byte)0;

        try
        {
            await SendAllAsync(buffer, ct);
        }
        catch (Exception ex) when (ex is SocketException or IOException)
        {
            // Connection broken — reconnect and retry once.
            Disconnect();
            await EnsureConnectedAsync(socketPath, ct);
            await SendAllAsync(buffer, ct);
        }
    }

    private async ValueTask SendAllAsync(byte[] buffer, CancellationToken ct)
    {
        if (IsWindows)
        {
            await _pipe!.WriteAsync(buffer, ct);
            await _pipe.FlushAsync(ct);
        }
        else
        {
            var remaining = buffer.AsMemory();
            while (remaining.Length > 0)
            {
                var sent = await _socket!.SendAsync(
                    remaining, SocketFlags.None, ct);
                if (sent == 0)
                    throw new SocketException(
                        (int)SocketError.ConnectionReset);
                remaining = remaining[sent..];
            }
        }
    }

    /// <summary>
    /// Connects the client end for the worker's pipe. The access is spelled
    /// out (write data and synchronize): <c>PipeDirection.Out</c> would ask
    /// for GENERIC_WRITE, which includes the pipe-instance right the worker
    /// withholds from other accounts, so the connect would be denied for an
    /// application pool identity.
    /// </summary>
    internal static async ValueTask<NamedPipeClientStream> ConnectPipeClientAsync(
        string pipeName, CancellationToken ct)
    {
#if NET9_0_OR_GREATER
        var pipe = new NamedPipeClientStream(
            ".", pipeName,
            PipeAccessRights.WriteData | PipeAccessRights.Synchronize,
            PipeOptions.Asynchronous,
            TokenImpersonationLevel.Identification,
            HandleInheritability.None);
        try
        {
            await pipe.ConnectAsync(ct);
            return pipe;
        }
        catch
        {
            pipe.Dispose();
            throw;
        }
#else
        // The PipeAccessRights constructor needs .NET 9, so open the pipe
        // directly: wait for a free instance, then open with the narrow
        // access, as the stock connect does.
        var fullName = @"\\.\pipe\" + pipeName;
        while (true)
        {
            ct.ThrowIfCancellationRequested();
            var handle = NativePipe.CreateFileW(
                fullName, NativePipe.FileWriteData | NativePipe.Synchronize,
                0, IntPtr.Zero, NativePipe.OpenExisting,
                NativePipe.FileFlagOverlapped | NativePipe.SecurityQosPresent |
                    NativePipe.SecurityIdentification,
                IntPtr.Zero);
            if (!handle.IsInvalid)
            {
                try
                {
                    return new NamedPipeClientStream(
                        PipeDirection.Out, isAsync: true, isConnected: true,
                        handle);
                }
                catch
                {
                    handle.Dispose();
                    throw;
                }
            }

            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            if (error == NativePipe.ErrorPipeBusy)
            {
                // Every instance is taken: wait briefly for one, then retry.
                NativePipe.WaitNamedPipeW(fullName, 50);
            }
            else if (error == NativePipe.ErrorFileNotFound)
            {
                // Not created yet; the stock connect keeps trying as well.
                await Task.Delay(10, ct);
            }
            else
            {
                // Access denied surfaces as UnauthorizedAccessException, as
                // on the stock path.
                throw Marshal.GetExceptionForHR(
                    unchecked((int)0x80070000) | error)!;
            }
        }
#endif
    }

#if !NET9_0_OR_GREATER
    private static class NativePipe
    {
        public const uint FileWriteData = 0x2;
        public const uint Synchronize = 0x100000;
        public const uint OpenExisting = 3;
        public const uint FileFlagOverlapped = 0x40000000;
        public const uint SecurityQosPresent = 0x100000;
        public const uint SecurityIdentification = 0x10000;
        public const int ErrorFileNotFound = 2;
        public const int ErrorPipeBusy = 231;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern SafePipeHandle CreateFileW(
            string fileName, uint desiredAccess, uint shareMode,
            IntPtr securityAttributes, uint creationDisposition,
            uint flagsAndAttributes, IntPtr templateFile);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool WaitNamedPipeW(string name, uint timeoutMs);
    }
#endif

    private async ValueTask EnsureConnectedAsync(
        string socketPath, CancellationToken ct)
    {
        if (_connectedPath == socketPath)
        {
            if (IsWindows && _pipe is { IsConnected: true })
                return;
            if (!IsWindows && _socket is { Connected: true })
                return;
        }

        Disconnect();

        if (IsWindows)
        {
            // Worker prepends \\.\pipe\ to socket_path.  The
            // NamedPipeClientStream ctor adds that prefix internally,
            // so we pass the raw socket_path as the pipe name.  If the
            // config already carries the prefix, strip it.
            var pipeName = socketPath;
            const string pipePrefix = @"\\.\pipe\";
            if (pipeName.StartsWith(pipePrefix, StringComparison.Ordinal))
                pipeName = pipeName[pipePrefix.Length..];

            // Least privilege: identification level only; the worker never
            // acts as its clients.
            _pipe = await ConnectPipeClientAsync(pipeName, ct);
            _connectedPath = socketPath;
        }
        else
        {
            var socket = new Socket(
                AddressFamily.Unix, SocketType.Stream,
                ProtocolType.Unspecified);
            try
            {
                await socket.ConnectAsync(
                    new UnixDomainSocketEndPoint(socketPath), ct);
                _socket = socket;
                _connectedPath = socketPath;
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        }
    }

    private void Disconnect()
    {
        if (_pipe != null)
        {
            try { _pipe.Dispose(); }
            catch { /* best-effort cleanup */ }
            _pipe = null;
        }
        if (_socket != null)
        {
            try { _socket.Dispose(); }
            catch { /* best-effort cleanup */ }
            _socket = null;
        }
        _connectedPath = null;
    }

    private void PruneDedupEntries(long now)
    {
        var threshold = DedupCooldownMs;
        var stale = new List<(string, string, string, PageSpeedContentType, bool)>();
        foreach (var (k, v) in _dedup)
        {
            if ((now - v) >= threshold)
                stale.Add(k);
        }
        foreach (var k in stale)
            _dedup.Remove(k);
    }

    /// <inheritdoc/>
    public override void Dispose()
    {
        _channel.Writer.TryComplete();
        Disconnect();
        base.Dispose();
    }

    // Value type: stored inline in Channel segments, avoids heap allocation.
    private readonly record struct WorkerNotification(
        string Url, string Hostname, string Scheme,
        PageSpeedContentType ContentType, uint Mask, bool AgentRequest,
        bool HostVouched);
}
