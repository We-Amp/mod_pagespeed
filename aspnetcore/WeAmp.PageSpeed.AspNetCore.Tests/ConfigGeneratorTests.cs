using FluentAssertions;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.Options;
using Xunit;
using YamlDotNet.Serialization;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

public class ConfigGeneratorTests
{
    private readonly EnvoyConfigGenerator _generator;

    public ConfigGeneratorTests()
    {
        _generator = new EnvoyConfigGenerator(NullLogger<EnvoyConfigGenerator>.Instance);
    }

    [Fact]
    public void GenerateConfigYaml_WithDefaultOptions_ProducesValidYaml()
    {
        // Arrange
        var options = new PageSpeedOptions();

        // Act
        var (yaml, adminToken) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().NotBeNullOrEmpty();
        adminToken.Should().NotBeNullOrEmpty();
        adminToken.Should().HaveLength(64); // 32 bytes hex = 64 chars

        // Verify YAML parses correctly
        var deserializer = new DeserializerBuilder().Build();
        var config = deserializer.Deserialize<Dictionary<string, object>>(yaml);
        config.Should().ContainKey("static_resources");
        config.Should().ContainKey("admin");
    }

    [Fact]
    public void GenerateConfigYaml_WithCustomPorts_SetsCorrectValues()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions
            {
                ListenPort = 9080,
                OriginPort = 5001,
                AdminPort = 9902
            }
        };

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().Contain("port_value: 9080"); // Listen port
        yaml.Should().Contain("port_value: 5001"); // Origin port
        yaml.Should().Contain("port_value: 9902"); // Admin port
    }

    [Fact]
    public void GenerateConfigYaml_WithRedis_IncludesRedisConfig()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            Redis = new RedisOptions
            {
                Host = "redis.example.com",
                Port = 6380,
                DatabaseIndex = 2,
                TtlSeconds = 3600
            }
        };

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().Contain("redis:");
        yaml.Should().Contain("host: redis.example.com");
        yaml.Should().Contain("port: 6380");
        yaml.Should().Contain("database_index: 2");
        yaml.Should().Contain("ttl_sec: 3600");
    }

    [Fact]
    public void GenerateConfigYaml_WithoutRedis_OmitsRedisConfig()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            Redis = null
        };

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().NotContain("redis:");
    }

    [Fact]
    public void GenerateConfigYaml_WithAdminAuth_IncludesAuthConfig()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            AdminAuth = new AdminAuthOptions
            {
                Enabled = true,
                Token = "my-secret-token",
                AllowedIps = ["10.0.0.0/8", "127.0.0.1/32"],
                RateLimitRpm = 30
            }
        };

        // Act
        var (yaml, adminToken) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().Contain("admin_auth:");
        yaml.Should().Contain("enabled: true");
        yaml.Should().Contain("token: my-secret-token");
        yaml.Should().Contain("10.0.0.0/8");
        yaml.Should().Contain("127.0.0.1/32");
        yaml.Should().Contain("rate_limit_rpm: 30");
        adminToken.Should().Be("my-secret-token");
    }

    [Fact]
    public void GenerateConfigYaml_WithoutExplicitToken_GeneratesRandomToken()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            AdminAuth = new AdminAuthOptions
            {
                Enabled = true,
                Token = null // Auto-generate
            }
        };

        // Act
        var (yaml1, token1) = _generator.GenerateConfigYaml(options);
        var (yaml2, token2) = _generator.GenerateConfigYaml(options);

        // Assert
        token1.Should().NotBeNullOrEmpty();
        token2.Should().NotBeNullOrEmpty();
        token1.Should().NotBe(token2); // Each call generates a new token
    }

    [Fact]
    public void GenerateConfigYaml_WithAuthorizedDomains_IncludesDomains()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            Domains = new DomainOptions
            {
                AuthorizedDomains = ["*.example.com", "cdn.example.com", "localhost"]
            }
        };

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().Contain("authorized_domains:");
        yaml.Should().Contain("*.example.com");
        yaml.Should().Contain("cdn.example.com");
        yaml.Should().Contain("localhost");
    }

    [Fact]
    public void GenerateConfigYaml_WithVirtualHosts_IncludesVirtualHostConfig()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            VirtualHosts =
            [
                new VirtualHostOptions
                {
                    HostPattern = "api.example.com",
                    Priority = 0,
                    Enabled = false
                },
                new VirtualHostOptions
                {
                    HostPattern = "*.example.com",
                    Priority = 10,
                    RewriteLevel = "CoreFilters",
                    EnabledFilters = "rewrite_css,rewrite_javascript"
                }
            ]
        };

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().Contain("virtual_hosts:");
        yaml.Should().Contain("host_pattern: api.example.com");
        yaml.Should().Contain("host_pattern: '*.example.com'");
        yaml.Should().Contain("enabled: false");
        yaml.Should().Contain("rewrite_level: CoreFilters");
        yaml.Should().Contain("enabled_filters: rewrite_css,rewrite_javascript");
    }

    [Fact]
    public void GenerateConfigYaml_WithCacheOptions_SetsCorrectValues()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            Cache = new CacheOptions
            {
                LruCacheSizeKb = 256000,
                FileCacheSizeKb = 5120000,
                FileCachePath = "/custom/cache",
                LogDirectory = "/custom/logs"
            }
        };

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().Contain("lru_cache_kb_per_process: 256000");
        yaml.Should().Contain("file_cache_size_kb: 5120000");
        yaml.Should().Contain("file_cache_path: /custom/cache");
        yaml.Should().Contain("log_dir: /custom/logs");
    }

    [Fact]
    public void GenerateConfigYaml_WithRewriteMappings_IncludesMappings()
    {
        // Arrange
        var options = new PageSpeedOptions
        {
            Domains = new DomainOptions
            {
                RewriteMappings =
                [
                    new RewriteDomainMapping
                    {
                        ToDomain = "cdn.example.com",
                        FromDomains = "www.example.com,static.example.com"
                    }
                ]
            }
        };

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert
        yaml.Should().Contain("rewrite_mappings:");
        yaml.Should().Contain("to_domain: cdn.example.com");
        yaml.Should().Contain("from_domains: www.example.com,static.example.com");
    }

    [Fact]
    public void GenerateConfigYaml_ContainsRequiredEnvoyStructure()
    {
        // Arrange
        var options = new PageSpeedOptions();

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert - Verify required Envoy configuration structure
        yaml.Should().Contain("static_resources:");
        yaml.Should().Contain("listeners:");
        yaml.Should().Contain("clusters:");
        yaml.Should().Contain("envoy.filters.network.http_connection_manager");
        yaml.Should().Contain("envoy.filters.http.router");
        yaml.Should().Contain("type.googleapis.com/pagespeed.Decoder");
        yaml.Should().Contain("admin:");
    }

    [Fact]
    public void GenerateConfigYaml_PageSpeedFilter_HasRequiredLegacyFields()
    {
        // Arrange
        var options = new PageSpeedOptions();

        // Act
        var (yaml, _) = _generator.GenerateConfigYaml(options);

        // Assert - Legacy required fields
        yaml.Should().Contain("key: placeholder");
        yaml.Should().Contain("val: placeholder");
    }
}
