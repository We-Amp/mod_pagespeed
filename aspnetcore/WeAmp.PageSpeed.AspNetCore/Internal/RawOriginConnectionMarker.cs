namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Marker connection-feature that identifies a connection accepted on the PRIVATE
/// raw-origin Kestrel listener in <c>SidecarMode.Inverse</c>. This is the PRIMARY,
/// unforgeable loop-break signal: the raw-origin Kestrel listener tags every
/// accepted connection with this feature at bind time (see
/// <see cref="SidecarKestrelConfigureOptions"/> via <c>ListenOptions.Use</c>), so
/// the marker is set by the LISTENER (kernel-accepted on the raw-origin endpoint),
/// never by the peer or any request header.
///
/// The middleware's first check reads <c>context.Features.Get&lt;IRawOriginMarker&gt;()</c>;
/// when present it bypasses itself (serves the raw origin) and never re-forwards.
/// <c>InternalSidecarEndpoint.RawOriginLoopbackPort</c> (the
/// <c>context.Connection.LocalPort</c> compare) is the redundant, also-kernel-set
/// second signal. Together they close the UDS loop-break hole AND remove any
/// dependence on a spoofable header for the authoritative break.
/// </summary>
public interface IRawOriginMarker
{
}

/// <summary>
/// Singleton implementation of <see cref="IRawOriginMarker"/>. There is no
/// per-connection state to carry — presence of the feature on the connection IS
/// the signal — so a single shared instance is set on every raw-origin connection.
/// </summary>
internal sealed class RawOriginMarker : IRawOriginMarker
{
    /// <summary>The shared marker instance set on every raw-origin connection.</summary>
    public static readonly RawOriginMarker Instance = new();

    private RawOriginMarker()
    {
    }
}
