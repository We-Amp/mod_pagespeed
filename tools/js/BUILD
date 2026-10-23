licenses(["notice"])  # Apache 2

# The esbuild config and pinned lockfile are hashed into
# net/instaweb/rewriter/generated/.source-hash by the verify_generated_js
# genrule. Bazel builds never run esbuild: the generated assets are checked
# in (see tools/regenerate-rewriter-js.sh).
exports_files([
    "build-rewriter-js.mjs",
    "package-lock.json",
])
