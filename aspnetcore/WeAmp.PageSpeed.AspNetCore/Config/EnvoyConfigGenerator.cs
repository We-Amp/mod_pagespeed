using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Logging;
using WeAmp.PageSpeed.AspNetCore.Options;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace WeAmp.PageSpeed.AspNetCore.Config;

/// <summary>
/// Generates Envoy configuration YAML from PageSpeedOptions.
/// </summary>
public class EnvoyConfigGenerator
{
    private readonly ILogger<EnvoyConfigGenerator> _logger;

    public EnvoyConfigGenerator(ILogger<EnvoyConfigGenerator> logger)
    {
        _logger = logger;
    }

    /// <summary>
    /// Generates the complete Envoy configuration file and writes it to disk.
    /// </summary>
    /// <param name="options">PageSpeed configuration options.</param>
    /// <param name="configPath">Path to write the configuration file.</param>
    /// <returns>Generated admin token (if one was auto-generated).</returns>
    public string GenerateConfigFile(PageSpeedOptions options, string configPath)
    {
        var configDir = Path.GetDirectoryName(configPath);
        if (!string.IsNullOrEmpty(configDir))
        {
            Directory.CreateDirectory(configDir);
        }

        // Ensure cache and log directories exist
        var cacheDir = GetFileCachePath(options);
        var logDir = GetLogDirectory(options);
        Directory.CreateDirectory(cacheDir);
        Directory.CreateDirectory(logDir);

        _logger.LogDebug("Creating cache directory: {CacheDir}", cacheDir);
        _logger.LogDebug("Creating log directory: {LogDir}", logDir);

        var (config, adminToken) = GenerateConfig(options);

        var serializer = new SerializerBuilder()
            .WithNamingConvention(UnderscoredNamingConvention.Instance)
            .ConfigureDefaultValuesHandling(DefaultValuesHandling.OmitNull)
            .Build();

        var yaml = serializer.Serialize(config);
        File.WriteAllText(configPath, yaml);

        _logger.LogInformation("Generated Envoy configuration at {ConfigPath}", configPath);

        return adminToken;
    }

    /// <summary>
    /// Generates the Envoy configuration as a YAML string.
    /// </summary>
    /// <param name="options">PageSpeed configuration options.</param>
    /// <returns>Tuple of (YAML string, admin token).</returns>
    public (string Yaml, string AdminToken) GenerateConfigYaml(PageSpeedOptions options)
    {
        var (config, adminToken) = GenerateConfig(options);

        var serializer = new SerializerBuilder()
            .WithNamingConvention(UnderscoredNamingConvention.Instance)
            .ConfigureDefaultValuesHandling(DefaultValuesHandling.OmitNull)
            .Build();

        return (serializer.Serialize(config), adminToken);
    }

    /// <summary>
    /// Generates the configuration object.
    /// </summary>
    private (Dictionary<string, object> Config, string AdminToken) GenerateConfig(PageSpeedOptions options)
    {
        var adminToken = options.AdminAuth.Token ?? GenerateRandomToken();

        var config = new Dictionary<string, object>
        {
            ["static_resources"] = GenerateStaticResources(options, adminToken),
            ["admin"] = GenerateAdminConfig(options)
        };

        return (config, adminToken);
    }

    private Dictionary<string, object> GenerateStaticResources(PageSpeedOptions options, string adminToken)
    {
        return new Dictionary<string, object>
        {
            ["listeners"] = new List<object>
            {
                GenerateListener(options, adminToken)
            },
            ["clusters"] = new List<object>
            {
                GenerateOriginCluster(options)
            }
        };
    }

    private Dictionary<string, object> GenerateListener(PageSpeedOptions options, string adminToken)
    {
        return new Dictionary<string, object>
        {
            ["address"] = new Dictionary<string, object>
            {
                ["socket_address"] = new Dictionary<string, object>
                {
                    ["address"] = "0.0.0.0",
                    ["port_value"] = options.Sidecar.ListenPort
                }
            },
            ["filter_chains"] = new List<object>
            {
                new Dictionary<string, object>
                {
                    ["filters"] = new List<object>
                    {
                        GenerateHttpConnectionManager(options, adminToken)
                    }
                }
            }
        };
    }

    private Dictionary<string, object> GenerateHttpConnectionManager(PageSpeedOptions options, string adminToken)
    {
        return new Dictionary<string, object>
        {
            ["name"] = "envoy.filters.network.http_connection_manager",
            ["typed_config"] = new Dictionary<string, object>
            {
                ["@type"] = "type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager",
                ["generate_request_id"] = true,
                ["codec_type"] = "AUTO",
                ["stat_prefix"] = "ingress_http",
                ["route_config"] = GenerateRouteConfig(),
                ["http_filters"] = new List<object>
                {
                    GeneratePageSpeedFilter(options, adminToken),
                    new Dictionary<string, object>
                    {
                        ["name"] = "envoy.filters.http.router",
                        ["typed_config"] = new Dictionary<string, object>
                        {
                            ["@type"] = "type.googleapis.com/envoy.extensions.filters.http.router.v3.Router"
                        }
                    }
                }
            }
        };
    }

    private Dictionary<string, object> GenerateRouteConfig()
    {
        return new Dictionary<string, object>
        {
            ["name"] = "local_route",
            ["virtual_hosts"] = new List<object>
            {
                new Dictionary<string, object>
                {
                    ["name"] = "backend",
                    ["domains"] = new List<string> { "*" },
                    ["routes"] = new List<object>
                    {
                        new Dictionary<string, object>
                        {
                            ["match"] = new Dictionary<string, object>
                            {
                                ["prefix"] = "/"
                            },
                            ["route"] = new Dictionary<string, object>
                            {
                                ["cluster"] = "origin"
                            }
                        }
                    }
                }
            }
        };
    }

    private Dictionary<string, object> GeneratePageSpeedFilter(PageSpeedOptions options, string adminToken)
    {
        var decoder = new Dictionary<string, object>
        {
            // Legacy required fields
            ["key"] = "placeholder",
            ["val"] = "placeholder",

            // Cache settings
            ["file_cache_path"] = GetFileCachePath(options),
            ["log_dir"] = GetLogDirectory(options),
            ["lru_cache_kb_per_process"] = options.Cache.LruCacheSizeKb,
            ["file_cache_size_kb"] = options.Cache.FileCacheSizeKb
        };

        // Redis configuration
        if (options.Redis != null)
        {
            decoder["redis"] = new Dictionary<string, object>
            {
                ["server"] = new Dictionary<string, object>
                {
                    ["host"] = options.Redis.Host,
                    ["port"] = options.Redis.Port
                },
                ["timeout_us"] = options.Redis.TimeoutUs,
                ["reconnection_delay_ms"] = options.Redis.ReconnectionDelayMs,
                ["database_index"] = options.Redis.DatabaseIndex,
                ["ttl_sec"] = options.Redis.TtlSeconds
            };
        }

        // Domain configuration
        decoder["domains"] = GenerateDomainConfig(options.Domains);

        // Admin authentication
        if (options.AdminAuth.Enabled)
        {
            decoder["admin_auth"] = new Dictionary<string, object>
            {
                ["enabled"] = true,
                ["token"] = adminToken,
                ["allowed_ips"] = options.AdminAuth.AllowedIps,
                ["rate_limit_rpm"] = options.AdminAuth.RateLimitRpm
            };
        }

        // Virtual hosts
        if (options.VirtualHosts.Count > 0)
        {
            decoder["virtual_hosts"] = options.VirtualHosts.Select(vh => GenerateVirtualHostConfig(vh)).ToList();
        }

        return new Dictionary<string, object>
        {
            ["name"] = "pagespeed",
            ["typed_config"] = new Dictionary<string, object>
            {
                ["@type"] = "type.googleapis.com/pagespeed.Decoder"
            }.Concat(decoder).ToDictionary(x => x.Key, x => x.Value)
        };
    }

    private Dictionary<string, object> GenerateDomainConfig(DomainOptions domains)
    {
        var config = new Dictionary<string, object>
        {
            ["authorized_domains"] = domains.AuthorizedDomains
        };

        if (domains.RewriteMappings.Count > 0)
        {
            config["rewrite_mappings"] = domains.RewriteMappings.Select(m => new Dictionary<string, object>
            {
                ["to_domain"] = m.ToDomain,
                ["from_domains"] = m.FromDomains
            }).ToList();
        }

        if (domains.OriginMappings.Count > 0)
        {
            config["origin_mappings"] = domains.OriginMappings.Select(m =>
            {
                var mapping = new Dictionary<string, object>
                {
                    ["to_domain"] = m.ToDomain,
                    ["from_domains"] = m.FromDomains
                };
                if (!string.IsNullOrEmpty(m.HostHeader))
                {
                    mapping["host_header"] = m.HostHeader;
                }
                return mapping;
            }).ToList();
        }

        if (domains.Shards.Count > 0)
        {
            config["shards"] = domains.Shards.Select(s => new Dictionary<string, object>
            {
                ["domain"] = s.Domain,
                ["shards"] = s.Shards
            }).ToList();
        }

        return config;
    }

    private Dictionary<string, object> GenerateVirtualHostConfig(VirtualHostOptions vh)
    {
        var config = new Dictionary<string, object>
        {
            ["host_pattern"] = vh.HostPattern,
            ["priority"] = vh.Priority,
            ["options"] = GenerateVirtualHostOptionsConfig(vh)
        };

        return config;
    }

    private Dictionary<string, object> GenerateVirtualHostOptionsConfig(VirtualHostOptions vh)
    {
        var options = new Dictionary<string, object>
        {
            ["enabled"] = vh.Enabled
        };

        if (!string.IsNullOrEmpty(vh.RewriteLevel))
        {
            options["rewrite_level"] = vh.RewriteLevel;
        }

        if (!string.IsNullOrEmpty(vh.EnabledFilters))
        {
            options["enabled_filters"] = vh.EnabledFilters;
        }

        if (!string.IsNullOrEmpty(vh.DisabledFilters))
        {
            options["disabled_filters"] = vh.DisabledFilters;
        }

        if (vh.CustomOptions.Count > 0)
        {
            options["custom_options"] = vh.CustomOptions;
        }

        if (vh.Domains != null)
        {
            options["domains"] = GenerateDomainConfig(vh.Domains);
        }

        return options;
    }

    private Dictionary<string, object> GenerateOriginCluster(PageSpeedOptions options)
    {
        return new Dictionary<string, object>
        {
            ["name"] = "origin",
            ["connect_timeout"] = "5s",
            ["type"] = "STATIC",
            ["lb_policy"] = "ROUND_ROBIN",
            ["load_assignment"] = new Dictionary<string, object>
            {
                ["cluster_name"] = "origin",
                ["endpoints"] = new List<object>
                {
                    new Dictionary<string, object>
                    {
                        ["lb_endpoints"] = new List<object>
                        {
                            new Dictionary<string, object>
                            {
                                ["endpoint"] = new Dictionary<string, object>
                                {
                                    ["address"] = new Dictionary<string, object>
                                    {
                                        ["socket_address"] = new Dictionary<string, object>
                                        {
                                            ["address"] = "127.0.0.1",
                                            ["port_value"] = options.Sidecar.OriginPort
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            },
            ["circuit_breakers"] = new Dictionary<string, object>
            {
                ["thresholds"] = new List<object>
                {
                    new Dictionary<string, object>
                    {
                        ["priority"] = "DEFAULT",
                        ["max_connections"] = 1024,
                        ["max_pending_requests"] = 1024,
                        ["max_requests"] = 1024,
                        ["max_retries"] = 3
                    }
                }
            }
        };
    }

    private Dictionary<string, object> GenerateAdminConfig(PageSpeedOptions options)
    {
        return new Dictionary<string, object>
        {
            ["address"] = new Dictionary<string, object>
            {
                ["socket_address"] = new Dictionary<string, object>
                {
                    ["address"] = options.Sidecar.AdminBindAddress,
                    ["port_value"] = options.Sidecar.AdminPort
                }
            }
        };
    }

    private string GetFileCachePath(PageSpeedOptions options)
    {
        return options.Cache.FileCachePath
            ?? Path.Combine(Path.GetTempPath(), "pagespeed_cache", "files");
    }

    private string GetLogDirectory(PageSpeedOptions options)
    {
        return options.Cache.LogDirectory
            ?? Path.Combine(Path.GetTempPath(), "pagespeed_cache", "logs");
    }

    private static string GenerateRandomToken()
    {
        var bytes = new byte[32];
        RandomNumberGenerator.Fill(bytes);
        return Convert.ToHexString(bytes).ToLowerInvariant();
    }
}
