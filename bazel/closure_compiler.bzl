closure_library_rules = """
sh_library(
    name = "runfiles",
    srcs = [],
    visibility = ["//visibility:public"],
)

# Filegroup containing all closure library JS files (excluding tests)
filegroup(
    name = "all_js",
    srcs = glob(
        ["closure/**/*.js", "third_party/**/*.js"],
        exclude = ["**/*_test.js", "**/*_perf.js"],
    ),
    visibility = ["//visibility:public"],
)
"""

def _to_label(path):
    """Convert a path like 'pagespeed/system/foo.js' to a label like '//pagespeed/system:foo.js'."""
    if path.startswith("//") or path.startswith("@"):
        return path
    parts = path.rsplit("/", 1)
    if len(parts) == 2:
        return "//" + parts[0] + ":" + parts[1]
    return ":" + path

def closure_compiler_gen(name, js_src, js_includes = [], js_dir = [], entry_points = [], externs = [], opt = True):
    """Generate optimized JavaScript using closure compiler with dependency mode."""

    # Convert paths to labels
    js_src_label = _to_label(js_src)
    js_includes_labels = [_to_label(f) for f in js_includes]
    externs_labels = [_to_label(f) for f in externs]

    # Collect all source files for srcs
    all_srcs = [js_src_label] + js_includes_labels + externs_labels

    js_include_str = ""
    for f in all_srcs[1:len(js_includes)+1]:  # Skip first (js_src) and externs
        js_include_str += " --js $(execpath " + f + ")"

    js_entry_points = ""
    for ep in entry_points:
        js_entry_points += " --entry_point " + ep

    js_externs = ""
    for f in externs_labels:
        js_externs += " --externs $(execpath " + f + ")"

    if opt == True:
        BUILD_FLAGS = " --compilation_level=ADVANCED"
    else:
        BUILD_FLAGS = "  --compilation_level=SIMPLE --formatting=PRETTY_PRINT "

    # Use a flagfile to avoid command line length limits on Windows
    native.genrule(
        name = name,
        srcs = all_srcs + ["@closure_library//:all_js"],
        outs = [name + ".js"],
        cmd = select({
            "@platforms//os:windows": (
                # On Windows, write JS file list to a flagfile to avoid command line length limits.
                # Prepend the real Node.js directory to PATH so that npx resolves to the
                # Node.js installation's script (which can find npm modules) instead of
                # Git Bash's /usr/bin/npx (which uses a node.exe lacking npm modules).
                "for f in $(locations @closure_library//:all_js); do echo --js $$f; done > $@.flagfile && " +
                "echo '--output_wrapper=(function(){%output%})();' >> $@.flagfile && " +
                "\"$$PROGRAMFILES\"/nodejs/node.exe \"$$PROGRAMFILES\"/nodejs/node_modules/npm/bin/npx-cli.js google-closure-compiler" +
                " --js $(execpath " + js_src_label + ")" +
                " --js_output_file $@" +
                js_include_str +
                BUILD_FLAGS +
                js_entry_points +
                js_externs +
                " --dependency_mode PRUNE" +
                " --jscomp_off=checkVars" +
                " --generate_exports" +
                " --flagfile=$@.flagfile && rm -f $@.flagfile"
            ),
            "//conditions:default": (
                "npx google-closure-compiler" +
                " --js $(execpath " + js_src_label + ")" +
                " --js_output_file $@" +
                js_include_str +
                BUILD_FLAGS +
                js_entry_points +
                js_externs +
                " --dependency_mode PRUNE" +
                " --jscomp_off=checkVars" +
                " --generate_exports" +
                " '--output_wrapper=(function(){%output%})();'" +
                " $$(echo '$(locations @closure_library//:all_js)' | tr ' ' '\\n' | sort | sed 's/^/--js /')"
            ),
        }),
    )

def closure_compiler_without_dependency_mode(name, js_src, js_includes = [], js_dir = [], externs = [], opt = True):
    """Generate optimized JavaScript using closure compiler without dependency mode."""

    for js_file in js_src:
        # Convert paths to labels
        js_file_label = _to_label(js_file)
        js_includes_labels = [_to_label(f) for f in js_includes]
        externs_labels = [_to_label(f) for f in externs]

        # Collect all source files for srcs
        all_srcs = [js_file_label] + js_includes_labels + externs_labels

        js_include_str = ""
        for f in js_includes_labels:
            js_include_str += " --js $(execpath " + f + ")"

        js_externs = ""
        for f in externs_labels:
            js_externs += " --externs $(execpath " + f + ")"

        if opt == True:
            BUILD_FLAGS = " --compilation_level=ADVANCED "
            rule_name = js_file.split("/")[len(js_file.split("/")) - 1].split(".js")[0] + "_opt"
        else:
            BUILD_FLAGS = "  --compilation_level=SIMPLE --formatting=PRETTY_PRINT "
            rule_name = js_file.split("/")[len(js_file.split("/")) - 1].split(".js")[0] + "_dbg"

        # Build the common closure compiler flags (without output_wrapper)
        cc_flags_base = (" --js $(execpath " + js_file_label + ")" +
                    " --js_output_file $@" +
                    js_include_str +
                    BUILD_FLAGS +
                    js_externs +
                    " --jscomp_off=checkVars" +
                    " --generate_exports")

        native.genrule(
            name = rule_name,
            srcs = all_srcs + ["@closure_library//:runfiles"],
            outs = [rule_name + ".js"],
            cmd = select({
                # On Windows, call node.exe directly to bypass Git Bash's
                # npx shebang re-parsing that breaks (function(){}) syntax.
                # Write output_wrapper to a flagfile to avoid shell interpretation.
                "@platforms//os:windows": (
                    "echo '--output_wrapper=(function(){%output%})();' > $@.flagfile && " +
                    "\"$$PROGRAMFILES\"/nodejs/node.exe \"$$PROGRAMFILES\"/nodejs/node_modules/npm/bin/npx-cli.js google-closure-compiler" +
                    cc_flags_base +
                    " --flagfile=$@.flagfile && rm -f $@.flagfile"
                ),
                "//conditions:default": "npx google-closure-compiler" + cc_flags_base + " '--output_wrapper=(function(){%output%})();'",
            }),
        )
