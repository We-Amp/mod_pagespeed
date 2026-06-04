using System.Text;
using FluentAssertions;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tests for the BYOL license-file plumbing: the
/// subscription-aware never-clobber write, the parent-of-cache path derivation,
/// the decode-only token validation (730-day cap / expiry / product scope), and
/// the PAGESPEED_LICENSE_SERVICE_URL child-environment wiring. All mirror the C++
/// license_v2 semantics on origin/master and require no signing key.
/// </summary>
public class LicenseFileTests
{
    // kMaxTokenLifetimeSec (license_verifier.cc) — 730 days.
    private const long MaxLifetimeSec = 63072000;

    // ---------- never-clobber 3-state (+ worker-renewal preservation) ----------

    [Fact]
    public void WriteLicenseFileIfNeeded_WhenAbsent_WritesOwnerOnly()
    {
        using var tmp = new TempDir();
        var path = Path.Combine(tmp.Path, "pagespeed.license");
        var token = MakeToken(iat: 1000, exp: 0, sid: "sub_A");

        var wrote = ProcessSidecarManager.WriteLicenseFileIfNeeded(path, token);

        wrote.Should().BeTrue("an absent license file must be created");
        File.Exists(path).Should().BeTrue();
        File.ReadAllText(path).Should().Be(token);
        if (!OperatingSystem.IsWindows())
        {
            var mode = File.GetUnixFileMode(path);
            const UnixFileMode groupOther =
                UnixFileMode.GroupRead | UnixFileMode.GroupWrite | UnixFileMode.GroupExecute |
                UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute;
            (mode & groupOther).Should().Be(UnixFileMode.None,
                "the license token must not be group/other readable (0600)");
            (mode & UnixFileMode.UserRead).Should().Be(UnixFileMode.UserRead);
        }
    }

    [Fact]
    public void WriteLicenseFileIfNeeded_WhenIdentical_IsNoOp()
    {
        using var tmp = new TempDir();
        var path = Path.Combine(tmp.Path, "pagespeed.license");
        var token = MakeToken(iat: 1000, exp: 0, sid: "sub_A");

        ProcessSidecarManager.WriteLicenseFileIfNeeded(path, token).Should().BeTrue();
        var second = ProcessSidecarManager.WriteLicenseFileIfNeeded(path, token);

        second.Should().BeFalse("an identical token is a never-clobber no-op");
        File.ReadAllText(path).Should().Be(token);
    }

    [Fact]
    public void WriteLicenseFileIfNeeded_WhenDifferentSubscription_Overwrites()
    {
        using var tmp = new TempDir();
        var path = Path.Combine(tmp.Path, "pagespeed.license");
        var original = MakeToken(iat: 1000, exp: 0, sid: "sub_A");
        var rotated = MakeToken(iat: 2000, exp: 0, sid: "sub_B");

        ProcessSidecarManager.WriteLicenseFileIfNeeded(path, original).Should().BeTrue();
        var rewrote = ProcessSidecarManager.WriteLicenseFileIfNeeded(path, rotated);

        rewrote.Should().BeTrue("a different subscription is an operator rotation — write it");
        File.ReadAllText(path).Should().Be(rotated);
    }

    [Fact]
    public void WriteLicenseFileIfNeeded_WhenWorkerRenewedSameSubscription_PreservesOnDisk()
    {
        using var tmp = new TempDir();
        var path = Path.Combine(tmp.Path, "pagespeed.license");
        // The worker renewed the token in place: same subscription (sid), later exp.
        var renewed = MakeToken(iat: 1_700_000_000, exp: 1_900_000_000, sid: "sub_S");
        // The operator's originally-configured key for the SAME subscription, older.
        var configured = MakeToken(iat: 1_600_000_000, exp: 1_700_000_000, sid: "sub_S");

        ProcessSidecarManager.WriteLicenseFileIfNeeded(path, renewed).Should().BeTrue();
        var rewrote = ProcessSidecarManager.WriteLicenseFileIfNeeded(path, configured);

        rewrote.Should().BeFalse(
            "a worker-renewed token of the same subscription must never be clobbered (could revert to an expired token)");
        File.ReadAllText(path).Should().Be(renewed, "the on-disk renewal is preserved");
    }

    [Theory]
    [InlineData(false)] // null -> write
    [InlineData(true)]  // identical -> skip
    public void ShouldWriteLicense_AbsentVsIdentical(bool identical)
    {
        var token = MakeToken(iat: 1000, exp: 0, sid: "sub_A");
        if (identical)
        {
            ProcessSidecarManager.ShouldWriteLicense(token, token).Should().BeFalse();
        }
        else
        {
            ProcessSidecarManager.ShouldWriteLicense(null, token).Should().BeTrue();
        }
    }

    [Fact]
    public void ShouldWriteLicense_SameSubjectNoSid_IsPreservedAsRenewal()
    {
        var onDisk = MakeToken(iat: 2000, exp: 1_900_000_000, sub: "cust@x.test", sid: "");
        var configured = MakeToken(iat: 1000, exp: 1_700_000_000, sub: "cust@x.test", sid: "");

        ProcessSidecarManager.ShouldWriteLicense(onDisk, configured)
            .Should().BeFalse("same subject + no sid ⇒ treat as a renewal of the same customer");
    }

    [Fact]
    public void ShouldWriteLicense_UndecodableOnDisk_OperatorKeyWins()
    {
        var configured = MakeToken(iat: 1000, exp: 0, sid: "sub_A");

        ProcessSidecarManager.ShouldWriteLicense("!!!not-a-token!!!", configured)
            .Should().BeTrue("an undecodable on-disk blob can't be proven a renewal — the operator key wins");
    }

    // ---------------------- parent-of-cache path derivation ----------------------

    [Fact]
    public void ResolveLicenseFilePath_IsParentOfCache_TrailingSeparatorInsensitive()
    {
        using var tmp = new TempDir();
        var cache = Path.Combine(tmp.Path, "cache");
        var expected = Path.Combine(tmp.Path, "pagespeed.license");

        ProcessSidecarManager.ResolveLicenseFilePath("/srv/app", cache)
            .Should().Be(expected);
        ProcessSidecarManager.ResolveLicenseFilePath("/srv/app", cache + Path.DirectorySeparatorChar)
            .Should().Be(expected, "a trailing separator must be stripped before taking the parent");
    }

    [Fact]
    public void ResolveLicenseFilePath_DefaultsCacheUnderConfigPrefix()
    {
        using var tmp = new TempDir();
        // No FileCachePath -> cache defaults to <prefix>/cache -> license sits in <prefix>.
        ProcessSidecarManager.ResolveLicenseFilePath(tmp.Path, null)
            .Should().Be(Path.Combine(tmp.Path, "pagespeed.license"));
    }

    // ----------------------- decode-only token inspection -----------------------

    [Fact]
    public void TryDecodeLicenseToken_RoundTripsClaims()
    {
        var token = MakeToken(iat: 111, exp: 222, sub: "u@test", sid: "S1", products: ["mps1", "the 2.0 optimizer line"]);

        var claims = ProcessSidecarManager.TryDecodeLicenseToken(token);

        claims.Should().NotBeNull();
        claims!.Iat.Should().Be(111);
        claims.Exp.Should().Be(222);
        claims.Sub.Should().Be("u@test");
        claims.Sid.Should().Be("S1");
        claims.Products.Should().Equal("mps1", "the 2.0 optimizer line");
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("!!!not-base64url!!!")]
    public void TryDecodeLicenseToken_Garbage_ReturnsNull(string garbage)
    {
        ProcessSidecarManager.TryDecodeLicenseToken(garbage).Should().BeNull();
    }

    [Fact]
    public void TryDecodeLicenseToken_TooShortForSignature_ReturnsNull()
    {
        // A valid base64url blob of fewer than 64 bytes can't carry a signature+payload.
        var shortBlob = Base64UrlEncode(new byte[10]);
        ProcessSidecarManager.TryDecodeLicenseToken(shortBlob).Should().BeNull();
    }

    // --------------- decode-only validation (730d / expiry / scope) ---------------

    private const long Now = 1_700_000_000;

    // These isolate the DURATION/expiry logic, so they carry a valid mps1 scope —
    // otherwise the (now-flagged) empty-products scope check would add a second
    // problem and break the ContainSingle/BeEmpty assertions below.

    [Fact]
    public void Describe_ExactlyAtCap_NotFlagged()
    {
        var claims = Claims(iat: Now, exp: Now + MaxLifetimeSec, products: ["mps1"]);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now).Should().BeEmpty();
    }

    [Fact]
    public void Describe_OverCap_IsFlagged()
    {
        var claims = Claims(iat: Now, exp: Now + MaxLifetimeSec + 1, products: ["mps1"]);
        var problems = ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now);
        problems.Should().ContainSingle().Which.Should().Contain("730-day");
    }

    [Fact]
    public void Describe_Perpetual_NoChecks()
    {
        // exp == 0 means perpetual — no duration/expiry enforcement (even with odd iat).
        var claims = Claims(iat: -999, exp: 0, products: ["mps1"]);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now).Should().BeEmpty();
    }

    [Fact]
    public void Describe_Expired_IsFlagged()
    {
        var claims = Claims(iat: Now - 1000, exp: Now - 10, products: ["mps1"]);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now)
            .Should().ContainSingle().Which.Should().Contain("expired");
    }

    [Fact]
    public void Describe_ExpiryPrecedesIssuance_IsFlagged()
    {
        var claims = Claims(iat: Now, exp: Now - 100, products: ["mps1"]);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now)
            .Should().ContainSingle().Which.Should().Contain("precedes");
    }

    [Fact]
    public void Describe_NegativeTimestamp_IsFlagged()
    {
        var claims = Claims(iat: -1, exp: Now, products: ["mps1"]);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now)
            .Should().ContainSingle().Which.Should().Contain("negative");
    }

    [Fact]
    public void Describe_MissingMps1Scope_IsFlagged()
    {
        var claims = Claims(iat: Now, exp: Now + 100, products: ["the 2.0 optimizer line"]);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now)
            .Should().ContainSingle().Which.Should().Contain("mps1");
    }

    [Fact]
    public void Describe_HasMps1Scope_NotFlagged()
    {
        var claims = Claims(iat: Now, exp: Now + 100, products: ["mps1", "the 2.0 optimizer line"]);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now).Should().BeEmpty();
    }

    [Fact]
    public void Describe_NoProductsClaim_IsFlaggedAsUnauthorized()
    {
        // CheckProductAuthorization (license_verifier.cc:205) treats an empty products
        // array as unauthorized, so a token with no products claim is flagged for scope.
        var claims = Claims(iat: Now, exp: Now + 100);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now)
            .Should().ContainSingle().Which.Should().Contain("mps1");
    }

    [Fact]
    public void Describe_EmptyProductsArray_IsFlaggedAsUnauthorized()
    {
        var claims = Claims(iat: Now, exp: Now + 100, products: []);
        ProcessSidecarManager.DescribeLicenseTokenProblems(claims, Now)
            .Should().ContainSingle().Which.Should().Contain("mps1");
    }

    // ------------------- PAGESPEED_LICENSE_SERVICE_URL wiring -------------------

    [Fact]
    public void BuildChildEnvironment_ValidLicenseServiceUrl_IsPassedThrough()
    {
        var opts = new PageSpeedOptions { LicenseServiceUrl = "https://lic.example.com/api" };
        var env = ProcessSidecarManager.BuildChildEnvironment(opts);
        env.Should().ContainKey("PAGESPEED_LICENSE_SERVICE_URL")
            .WhoseValue.Should().Be("https://lic.example.com/api");
    }

    [Fact]
    public void BuildChildEnvironment_NoLicenseServiceUrl_KeyAbsent()
    {
        var env = ProcessSidecarManager.BuildChildEnvironment(new PageSpeedOptions());
        env.Should().NotContainKey("PAGESPEED_LICENSE_SERVICE_URL");
    }

    [Theory]
    [InlineData("not a url")]
    [InlineData("ftp://example.com")]
    [InlineData("/relative/path")]
    public void BuildChildEnvironment_InvalidLicenseServiceUrl_IsDroppedWithWarning(string bad)
    {
        var warnings = new List<string>();
        var opts = new PageSpeedOptions { LicenseServiceUrl = bad };

        var env = ProcessSidecarManager.BuildChildEnvironment(opts, warnings.Add);

        env.Should().NotContainKey("PAGESPEED_LICENSE_SERVICE_URL",
            "a malformed URL must not reach the child — the worker falls back to its default");
        warnings.Should().ContainSingle().Which.Should().Contain("LicenseServiceUrl");
    }

    [Fact]
    public void BuildChildEnvironment_EnvironmentVariables_ArePassedThrough()
    {
        var opts = new PageSpeedOptions();
        opts.Sidecar.EnvironmentVariables["FOO"] = "bar";
        ProcessSidecarManager.BuildChildEnvironment(opts).Should().ContainKey("FOO").WhoseValue.Should().Be("bar");
    }

    [Fact]
    public void BuildChildEnvironment_LicenseServiceUrl_OverridesEnvDictEntry()
    {
        var opts = new PageSpeedOptions { LicenseServiceUrl = "https://new.example.com" };
        opts.Sidecar.EnvironmentVariables["PAGESPEED_LICENSE_SERVICE_URL"] = "http://old.example.com";

        ProcessSidecarManager.BuildChildEnvironment(opts)["PAGESPEED_LICENSE_SERVICE_URL"]
            .Should().Be("https://new.example.com", "the typed option wins over the escape-hatch entry");
    }

    [Fact]
    public void BuildStartInfo_AppliesLicenseServiceUrlToChildEnvironment()
    {
        var opts = new PageSpeedOptions { LicenseServiceUrl = "https://lic.example.com/api" };
        var psi = ProcessSidecarManager.BuildStartInfo("/opt/ps/nginx", "/opt/ps/run/nginx.conf", opts);
        psi.Environment.Should().ContainKey("PAGESPEED_LICENSE_SERVICE_URL")
            .WhoseValue.Should().Be("https://lic.example.com/api");
    }

    // ------------------------------- helpers -------------------------------

    private static ProcessSidecarManager.LicenseTokenClaims Claims(
        long iat, long exp, string sub = "u@test", string sid = "", string[]? products = null)
        => new(sub, sid, iat, exp, products ?? []);

    /// <summary>
    /// Builds a token in the exact wire format the worker reads
    /// (license_token.cc BuildToken): <c>base64url(64-byte-signature ‖ json-payload)</c>.
    /// The signature is left as 64 zero bytes — the sidecar decodes claims WITHOUT
    /// verifying the signature, so a real signature is not needed for these tests.
    /// </summary>
    private static string MakeToken(
        long iat, long exp, string sub = "user@test", string sid = "", string[]? products = null)
    {
        var sb = new StringBuilder();
        sb.Append("{\"sub\":\"").Append(sub).Append("\",\"iss\":\"modpagespeed.com\",\"iat\":").Append(iat);
        if (exp != 0)
        {
            sb.Append(",\"exp\":").Append(exp);
        }
        if (!string.IsNullOrEmpty(sid))
        {
            sb.Append(",\"sid\":\"").Append(sid).Append('"');
        }
        if (products is { Length: > 0 })
        {
            sb.Append(",\"products\":[");
            for (var i = 0; i < products.Length; i++)
            {
                if (i > 0)
                {
                    sb.Append(',');
                }
                sb.Append('"').Append(products[i]).Append('"');
            }
            sb.Append(']');
        }
        sb.Append('}');

        var json = Encoding.UTF8.GetBytes(sb.ToString());
        var raw = new byte[64 + json.Length];
        Array.Copy(json, 0, raw, 64, json.Length);
        return Base64UrlEncode(raw);
    }

    private static string Base64UrlEncode(byte[] bytes) =>
        Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    private sealed class TempDir : IDisposable
    {
        public string Path { get; } =
            System.IO.Path.Combine(System.IO.Path.GetTempPath(), "pslic_" + Guid.NewGuid().ToString("N"));

        public TempDir() => Directory.CreateDirectory(Path);

        public void Dispose()
        {
            try { Directory.Delete(Path, recursive: true); } catch { /* best effort */ }
        }
    }
}
