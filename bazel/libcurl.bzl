# libcurl build rules for PageSpeed
#
# On Linux: Uses system-installed libcurl from /usr
# On Windows: Uses pre-downloaded curl from C:/curl

# Build rule for Linux (system curl at /usr)
libcurl_linux_build_rule = """
cc_library(
    name = "curl",
    hdrs = glob(["include/curl/**/*.h", "include/x86_64-linux-gnu/curl/**/*.h"]),
    includes = ["include", "include/x86_64-linux-gnu"],
    linkopts = ["-lcurl"],
    visibility = ["//visibility:public"],
)
"""

# Build rule for Windows (pre-built curl at C:/curl)
# Expects:
#   - include/curl/*.h  (headers)
#   - lib/libcurl.lib   (import library - created from .def file)
#   - bin/libcurl.dll   (runtime DLL)
libcurl_windows_build_rule = """
cc_import(
    name = "curl_import",
    interface_library = "lib/libcurl.lib",
    shared_library = "bin/libcurl.dll",
    visibility = ["//visibility:private"],
)

cc_library(
    name = "curl",
    hdrs = glob(["include/curl/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [":curl_import"],
)
"""

# Legacy build rule (kept for backward compatibility)
libcurl_build_rule = libcurl_linux_build_rule
