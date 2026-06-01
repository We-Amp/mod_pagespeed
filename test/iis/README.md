# IIS PageSpeed Integration Tests

This directory contains integration tests for the PageSpeed IIS module.

## Prerequisites

- Windows Server 2012 R2+ or Windows 10+ with IIS installed
- Python 3.8+ with pip
- pytest and requests packages
- PageSpeed IIS module installed

## Quick Start

1. **Install the test site:**
   ```powershell
   # Run as Administrator
   .\Setup-TestSite.ps1
   ```

2. **Run the tests:**
   ```powershell
   .\Run-IISTests.ps1
   ```

## Test Categories

Tests are organized by functionality and marked with pytest markers:

| Marker | Description |
|--------|-------------|
| `sanity` | Basic connectivity and functionality tests |
| `ipro` | In-Place Resource Optimization tests |
| `html_rewrite` | HTML rewriting filter tests |
| `admin` | Admin UI and statistics tests |
| `license` | License validation tests |
| `slow` | Tests that take longer to run |

### Running Specific Tests

```powershell
# Run only sanity tests
.\Run-IISTests.ps1 -Filter "sanity"

# Run IPRO tests
.\Run-IISTests.ps1 -Markers "ipro"

# Run all except slow tests
.\Run-IISTests.ps1 -Markers "not slow"

# Verbose output
.\Run-IISTests.ps1 -Verbose
```

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `PAGESPEED_HOST` | IIS server hostname | `localhost` |
| `PAGESPEED_PORT` | IIS server port | `80` (IIS) or `8080` (IIS Express) |
| `PAGESPEED_HTTPS` | Use HTTPS | `false` |
| `PAGESPEED_TEST_ROOT` | Path prefix for test pages | `/mod_pagespeed_test` |
| `PAGESPEED_EXAMPLE_ROOT` | Path prefix for example pages | `/mod_pagespeed_example` |
| `IIS_EXPRESS` | Set to `1` when using IIS Express | `0` |
| `PAGESPEED_LICENSE_KEY` | License key for license tests | (none) |

**Important:** When test files are at the site root (not in subdirectories), set
`PAGESPEED_TEST_ROOT=` and `PAGESPEED_EXAMPLE_ROOT=` to empty strings (not `/`)
to avoid double-slash URL issues.

## Test Files

| File | Description |
|------|-------------|
| `test_sanity.py` | Basic connectivity and PageSpeed detection |
| `test_ipro.py` | CSS, JS, and image optimization tests |
| `test_html_rewrite.py` | HTML rewriting filter tests |
| `test_admin.py` | Admin UI and statistics endpoint tests |
| `test_license.py` | License validation tests |
| `conftest.py` | Pytest fixtures and configuration |

## Test Site

The `testsite/` directory contains sample HTML, CSS, JavaScript, and image
files used by the tests. This site should be deployed to IIS before running
tests.

### Test Site Structure

```
testsite/
├── index.html              # Main test page
├── default.htm             # IIS default document
├── combine_css.html        # CSS combining test
├── combine_javascript.html # JS combining test
├── rewrite_images.html     # Image rewriting test
├── collapse_whitespace.html# Whitespace collapse test
├── styles/
│   ├── main.css           # Main stylesheet
│   ├── colors.css         # Color definitions
│   └── layout.css         # Layout utilities
├── scripts/
│   ├── main.js            # Main JavaScript
│   ├── util.js            # Utility functions
│   └── helper.js          # Helper functions
└── images/
    ├── logo.png           # Test PNG image
    ├── photo.jpg          # Test JPEG image
    └── small.gif          # Test GIF image
```

## Adding New Tests

1. Create a new test file `test_<feature>.py`
2. Import fixtures from `conftest.py`:
   ```python
   from pagespeed_test_framework import PageSpeedClient, assert_http_status
   ```
3. Use pytest markers to categorize:
   ```python
   @pytest.mark.ipro
   class TestNewFeature:
       def test_something(self, client, example_root):
           response = client.get(f"{example_root}/test.html")
           assert_http_status(response, 200)
   ```

## Testing with IIS Express

IIS Express is useful for development testing without full IIS installation.

```powershell
# Setup IIS Express (auto-detects module from bazel-bin/)
.\Setup-IISExpress.ps1 -Port 8080

# Start in background
.\Start-IISExpress.ps1 -Background -Wait -Port 8080

# Run tests (use cmd to avoid Git Bash path issues)
cmd /c "set PAGESPEED_PORT=8080& set IIS_EXPRESS=1& set PAGESPEED_TEST_ROOT=& set PAGESPEED_EXAMPLE_ROOT=& python -m pytest test_sanity.py -v"

# Stop when done
.\Stop-IISExpress.ps1
```

## Troubleshooting

### Run-IISTests.ps1 fails with "Host is read-only"
The `$Host` variable is reserved in PowerShell. Use pytest directly instead:
```powershell
cmd /c "set PAGESPEED_PORT=8080& python -m pytest test_sanity.py -v"
```

### Tests fail with connection refused
- Ensure IIS is running: `iisreset /start`
- Check the site is started: `Get-Website -Name PageSpeedTest`
- Verify port is correct

### PageSpeed not working
- Check module is installed: `Get-WebGlobalModule | Where-Object { $_.Name -match "PageSpeed" }`
- Check web.config has correct settings
- Look for errors in Event Viewer
- Verify the module returns `X-Page-Speed` header (note: IIS uses hyphen, Apache uses `X-PageSpeed`)

### Permission denied
- Run PowerShell as Administrator
- Check IIS_IUSRS has read access to site directory

### Git Bash mangles paths
When running from Git Bash, paths like `/` get converted to Windows paths.
Use `cmd /c` to run Python tests with environment variables:
```bash
cmd //c "set PAGESPEED_PORT=8080& python -m pytest test_sanity.py -v"
```

### Double-slash in URLs (404 errors)
If you see errors like "404 for //combine_css.html", the `PAGESPEED_EXAMPLE_ROOT`
is set to `/`. Set it to empty string instead:
```powershell
cmd /c "set PAGESPEED_EXAMPLE_ROOT=& python -m pytest test_sanity.py -v"
```

## CI Integration

For CI pipelines, use:

```powershell
# Install dependencies silently
pip install -q pytest requests

# Run tests with JUnit output
python -m pytest test/iis --junitxml=test-results.xml
```

GitHub Actions example:
```yaml
- name: Run IIS tests
  run: |
    cd test/iis
    python -m pytest --junitxml=results.xml
  env:
    PAGESPEED_HOST: localhost
    PAGESPEED_PORT: 80
```
