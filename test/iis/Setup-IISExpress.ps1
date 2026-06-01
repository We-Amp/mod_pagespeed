<#
.SYNOPSIS
    Sets up IIS Express for PageSpeed integration tests.

.DESCRIPTION
    This script configures IIS Express to run with the PageSpeed module
    for integration testing. IIS Express is included with Visual Studio
    and can run without full IIS installation.

.PARAMETER SitePath
    Path to the test site content. Default: testsite directory

.PARAMETER Port
    Port for IIS Express. Default: 8080

.PARAMETER ModulePath
    Path to pagespeed_iis.dll. Default: auto-detect from bazel-bin

.PARAMETER ConfigPath
    Path to write IIS Express config. Default: temp directory

.EXAMPLE
    .\Setup-IISExpress.ps1

.EXAMPLE
    .\Setup-IISExpress.ps1 -Port 9000 -SitePath C:\mysite
#>

param(
    [string]$SitePath = "",
    [int]$Port = 8080,
    [string]$ModulePath = "",
    [string]$ConfigPath = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Determine paths
if (-not $SitePath) {
    $SitePath = Join-Path $ScriptDir "testsite"
}

if (-not $ConfigPath) {
    $ConfigPath = Join-Path $env:TEMP "pagespeed_iisexpress"
}

# Find IIS Express
$IISExpressPath = ""
$PossiblePaths = @(
    "${env:ProgramFiles}\IIS Express\iisexpress.exe",
    "${env:ProgramFiles(x86)}\IIS Express\iisexpress.exe",
    "${env:ProgramW6432}\IIS Express\iisexpress.exe"
)

foreach ($path in $PossiblePaths) {
    if (Test-Path $path) {
        $IISExpressPath = $path
        break
    }
}

if (-not $IISExpressPath) {
    Write-Error "IIS Express not found. Please install IIS Express or Visual Studio."
    exit 1
}

Write-Host "Found IIS Express: $IISExpressPath" -ForegroundColor Green

# Find PageSpeed module
if (-not $ModulePath) {
    $BazelBin = Join-Path (Split-Path -Parent (Split-Path -Parent $ScriptDir)) "bazel-bin"
    $ModulePath = Join-Path $BazelBin "pagespeed\iis\pagespeed_iis.dll"

    if (-not (Test-Path $ModulePath)) {
        Write-Warning "PageSpeed module not found at $ModulePath"
        Write-Warning "Tests will run without PageSpeed (to verify test infrastructure)"
        $ModulePath = ""
    }
}

if ($ModulePath -and (Test-Path $ModulePath)) {
    Write-Host "Found PageSpeed module: $ModulePath" -ForegroundColor Green
}

# Create config directory
if (-not (Test-Path $ConfigPath)) {
    New-Item -ItemType Directory -Path $ConfigPath -Force | Out-Null
}

# Create IIS Express configuration
$ConfigFile = Join-Path $ConfigPath "applicationhost.config"

# Build module section
$ModuleSection = ""
if ($ModulePath -and (Test-Path $ModulePath)) {
    $ModuleSection = @"
            <add name="PageSpeedModule" image="$ModulePath" />
"@
}

# Generate applicationhost.config
$Config = @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
    <configSections>
        <sectionGroup name="system.applicationHost">
            <section name="applicationPools" allowDefinition="AppHostOnly" overrideModeDefault="Deny" />
            <section name="sites" allowDefinition="AppHostOnly" overrideModeDefault="Deny" />
        </sectionGroup>
        <sectionGroup name="system.webServer">
            <section name="asp" overrideModeDefault="Deny" />
            <section name="caching" overrideModeDefault="Allow" />
            <section name="cgi" overrideModeDefault="Deny" />
            <section name="defaultDocument" overrideModeDefault="Allow" />
            <section name="directoryBrowse" overrideModeDefault="Allow" />
            <section name="fastCgi" allowDefinition="AppHostOnly" overrideModeDefault="Deny" />
            <section name="globalModules" allowDefinition="AppHostOnly" overrideModeDefault="Deny" />
            <section name="handlers" overrideModeDefault="Deny" />
            <section name="httpCompression" overrideModeDefault="Allow" allowDefinition="Everywhere" />
            <section name="httpErrors" overrideModeDefault="Allow" />
            <section name="httpLogging" overrideModeDefault="Deny" />
            <section name="httpProtocol" overrideModeDefault="Allow" />
            <section name="httpRedirect" overrideModeDefault="Allow" />
            <section name="httpTracing" overrideModeDefault="Deny" />
            <section name="isapiFilters" allowDefinition="MachineToApplication" overrideModeDefault="Deny" />
            <section name="modules" allowDefinition="MachineToApplication" overrideModeDefault="Deny" />
            <section name="applicationInitialization" allowDefinition="MachineToApplication" overrideModeDefault="Allow" />
            <section name="odbcLogging" overrideModeDefault="Deny" />
            <sectionGroup name="security">
                <section name="access" overrideModeDefault="Deny" />
                <section name="applicationDependencies" overrideModeDefault="Deny" />
                <sectionGroup name="authentication">
                    <section name="anonymousAuthentication" overrideModeDefault="Deny" />
                    <section name="basicAuthentication" overrideModeDefault="Deny" />
                    <section name="clientCertificateMappingAuthentication" overrideModeDefault="Deny" />
                    <section name="digestAuthentication" overrideModeDefault="Deny" />
                    <section name="iisClientCertificateMappingAuthentication" overrideModeDefault="Deny" />
                    <section name="windowsAuthentication" overrideModeDefault="Deny" />
                </sectionGroup>
                <section name="authorization" overrideModeDefault="Allow" />
                <section name="ipSecurity" overrideModeDefault="Deny" />
                <section name="dynamicIpSecurity" overrideModeDefault="Deny" />
                <section name="isapiCgiRestriction" allowDefinition="AppHostOnly" overrideModeDefault="Deny" />
                <section name="requestFiltering" overrideModeDefault="Allow" />
            </sectionGroup>
            <section name="serverRuntime" overrideModeDefault="Deny" />
            <section name="serverSideInclude" overrideModeDefault="Deny" />
            <section name="staticContent" overrideModeDefault="Allow" />
            <sectionGroup name="tracing">
                <section name="traceFailedRequests" overrideModeDefault="Allow" />
                <section name="traceProviderDefinitions" overrideModeDefault="Deny" />
            </sectionGroup>
            <section name="urlCompression" overrideModeDefault="Allow" />
            <section name="validation" overrideModeDefault="Allow" />
        </sectionGroup>
        <section name="pagespeed" allowDefinition="Everywhere" overrideModeDefault="Allow" />
    </configSections>

    <system.applicationHost>
        <applicationPools>
            <add name="PageSpeedTestPool" managedRuntimeVersion="" managedPipelineMode="Integrated" />
        </applicationPools>

        <sites>
            <site name="PageSpeedTest" id="1">
                <application path="/" applicationPool="PageSpeedTestPool">
                    <virtualDirectory path="/" physicalPath="$SitePath" />
                </application>
                <bindings>
                    <binding protocol="http" bindingInformation="*:${Port}:localhost" />
                </bindings>
            </site>
        </sites>
    </system.applicationHost>

    <system.webServer>
        <globalModules>
            <add name="StaticFileModule" image="%ProgramFiles%\IIS Express\static.dll" />
            <add name="DefaultDocumentModule" image="%ProgramFiles%\IIS Express\defdoc.dll" />
            <add name="DirectoryListingModule" image="%ProgramFiles%\IIS Express\dirlist.dll" />
            <add name="StaticCompressionModule" image="%ProgramFiles%\IIS Express\compstat.dll" />
            <add name="DynamicCompressionModule" image="%ProgramFiles%\IIS Express\compdyn.dll" />
            <add name="CustomErrorModule" image="%ProgramFiles%\IIS Express\custerr.dll" />
            <add name="AnonymousAuthenticationModule" image="%ProgramFiles%\IIS Express\authanon.dll" />
$ModuleSection
        </globalModules>

        <modules>
            <add name="StaticFileModule" />
            <add name="DefaultDocumentModule" />
            <add name="DirectoryListingModule" />
            <add name="StaticCompressionModule" />
            <add name="DynamicCompressionModule" />
            <add name="CustomErrorModule" />
            <add name="AnonymousAuthenticationModule" />
            <add name="PageSpeedModule" />
        </modules>

        <handlers>
            <add name="StaticFile" path="*" verb="*" modules="StaticFileModule,DefaultDocumentModule,DirectoryListingModule" resourceType="Unspecified" requireAccess="Read" />
        </handlers>

        <staticContent>
            <mimeMap fileExtension=".css" mimeType="text/css" />
            <mimeMap fileExtension=".js" mimeType="application/javascript" />
            <mimeMap fileExtension=".html" mimeType="text/html" />
            <mimeMap fileExtension=".htm" mimeType="text/html" />
            <mimeMap fileExtension=".xhtml" mimeType="application/xhtml+xml" />
            <mimeMap fileExtension=".png" mimeType="image/png" />
            <mimeMap fileExtension=".jpg" mimeType="image/jpeg" />
            <mimeMap fileExtension=".gif" mimeType="image/gif" />
            <mimeMap fileExtension=".webp" mimeType="image/webp" />
            <mimeMap fileExtension=".svg" mimeType="image/svg+xml" />
            <mimeMap fileExtension=".woff" mimeType="font/woff" />
            <mimeMap fileExtension=".woff2" mimeType="font/woff2" />
            <mimeMap fileExtension=".txt" mimeType="text/plain" />
        </staticContent>

        <defaultDocument enabled="true">
            <files>
                <add value="index.html" />
                <add value="default.htm" />
            </files>
        </defaultDocument>

        <httpCompression>
            <dynamicTypes>
                <add mimeType="text/*" enabled="true" />
                <add mimeType="application/javascript" enabled="true" />
            </dynamicTypes>
            <staticTypes>
                <add mimeType="text/*" enabled="true" />
                <add mimeType="application/javascript" enabled="true" />
            </staticTypes>
        </httpCompression>

        <urlCompression doStaticCompression="true" doDynamicCompression="true" />

        <httpErrors errorMode="Detailed" />

        <security>
            <requestFiltering allowDoubleEscaping="true" />
        </security>
    </system.webServer>
</configuration>
"@

$Config | Out-File -FilePath $ConfigFile -Encoding UTF8
Write-Host "Created config: $ConfigFile" -ForegroundColor Green

# Create cache directory for PageSpeed
$CachePath = Join-Path $ConfigPath "cache"
if (-not (Test-Path $CachePath)) {
    New-Item -ItemType Directory -Path $CachePath -Force | Out-Null
}

# Create minimal web.config in site directory
# Note: PageSpeedModule is already registered in applicationhost.config's globalModules
# and modules sections, so we don't need to add it here again.
# Also, we don't include <pagespeed> section because IIS Express doesn't support
# custom configuration sections without schema files.
if ($ModulePath -and (Test-Path $ModulePath)) {
    $WebConfig = @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
  <system.webServer>
    <security>
      <requestFiltering allowDoubleEscaping="true" />
    </security>
  </system.webServer>
</configuration>
"@
    $WebConfigPath = Join-Path $SitePath "web.config"
    $WebConfig | Out-File -FilePath $WebConfigPath -Encoding UTF8
    Write-Host "Created web.config: $WebConfigPath" -ForegroundColor Green
}

# Output connection info
Write-Host ""
Write-Host "IIS Express Configuration Complete" -ForegroundColor Cyan
Write-Host "==================================" -ForegroundColor Cyan
Write-Host "Site Path:   $SitePath"
Write-Host "Port:        $Port"
Write-Host "Config:      $ConfigFile"
Write-Host "Cache:       $CachePath"
Write-Host ""
Write-Host "To start IIS Express:" -ForegroundColor Yellow
Write-Host "  & '$IISExpressPath' /config:'$ConfigFile' /site:PageSpeedTest"
Write-Host ""
Write-Host "Test URL: http://localhost:$Port/" -ForegroundColor Green
Write-Host "Admin URL: http://localhost:$Port/pagespeed_admin" -ForegroundColor Green

# Export variables for use by other scripts
$env:IISEXPRESS_PATH = $IISExpressPath
$env:IISEXPRESS_CONFIG = $ConfigFile
$env:PAGESPEED_HOST = "localhost"
$env:PAGESPEED_PORT = $Port.ToString()
# Set document root for localhost resource fetches (used by CurlUrlAsyncFetcher)
$env:PAGESPEED_DOCUMENT_ROOT = $SitePath

Write-Host "Environment: PAGESPEED_DOCUMENT_ROOT=$SitePath" -ForegroundColor Gray
