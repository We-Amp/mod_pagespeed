#!/usr/bin/env python3

# Except for the targets (search for @@@) this file is a 1:1 copy of Envoy's

import argparse
import glob
import json
import logging
import os
import shlex
import subprocess
from pathlib import Path


# This method is equivalent to https://github.com/grailbio/bazel-compilation-database/blob/master/generate.sh
def generateCompilationDatabase(args):
  # We need to download all remote outputs for generated source code. This option lives here to override those
  # specified in bazelrc.
  bazel_options = shlex.split(os.environ.get("BAZEL_BUILD_OPTIONS", "")) + [
      "--config=compdb",
      "--remote_download_outputs=all",
  ]

  result = subprocess.run(["bazel", "build"] + bazel_options + [
      "--aspects=@bazel_compdb//:aspects.bzl%compilation_database_aspect",
      "--output_groups=compdb_files,header_files",
      "--"  # separator: target patterns (including negations) follow here
  ] + args.bazel_targets)
  if result.returncode != 0:
    logging.warning("bazel build exited with code %d; collecting partial compdb", result.returncode)

  # Filter out build-only flags that are not accepted by "bazel info"
  info_options = [o for o in bazel_options if o not in ("--keep_going", "-k")]
  execroot = subprocess.check_output(["bazel", "info", "execution_root"] +
                                     info_options).decode().strip()

  compdb = []
  for compdb_file in Path(execroot).glob("**/*.compile_commands.json"):
    content = compdb_file.read_text().replace("__EXEC_ROOT__", execroot).strip()
    # bazel_compdb may output a JSON fragment (trailing comma) or a full JSON array
    if content.startswith("["):
      entries = json.loads(content)
    else:
      entries = json.loads("[" + content + "]")
    compdb.extend(entries)
  return compdb


def isHeader(filename):
  for ext in (".h", ".hh", ".hpp", ".hxx"):
    if filename.endswith(ext):
      return True
  return False


def isCompileTarget(target, args):
  filename = target["file"]
  if not args.include_headers and isHeader(filename):
    return False

  if not args.include_genfiles:
    if filename.startswith("bazel-out/"):
      return False

  if not args.include_external:
    if filename.startswith("external/"):
      return False

  # Always exclude third_party — upstream code we don't own
  if filename.startswith("third_party/"):
    return False

  # memcached_cache.cc requires libmemcached headers which only build on x64
  if filename.endswith("pagespeed/system/memcached_cache.cc"):
    return False

  # admin_license_handler.cc uses PAGESPEED_SERVER/OS/ARCH/DISTRIBUTION macros
  # that are passed as unquoted -D flags by Bazel, causing clang-tidy to see
  # them as undeclared identifiers rather than string literals.
  if filename.endswith("pagespeed/system/admin_license_handler.cc"):
    return False

  return True


def modifyCompileCommand(target, args):
  cc, options = target["command"].split(" ", 1)

  # Workaround for bazel added C++11 options, those doesn't affect build itself but
  # clang-tidy will misinterpret them.
  options = options.replace("-std=c++0x ", "")
  options = options.replace("-std=c++11 ", "")
  # Strip GCC-only flags not understood by clang-tidy
  options = options.replace("-fno-canonical-system-headers ", "")

  if args.vscode:
    # Visual Studio Code doesn't seem to like "-iquote". Replace it with
    # old-style "-I".
    options = options.replace("-iquote ", "-I ")

  if isHeader(target["file"]):
    options += " -Wno-pragma-once-outside-header -Wno-unused-const-variable"
    options += " -Wno-unused-function"
    if not target["file"].startswith("external/"):
      # *.h file is treated as C header by default while our headers files are all C++17.
      options = "-x c++ -std=c++17 -fexceptions " + options

  target["command"] = " ".join([cc, options])
  return target


def fixCompilationDatabase(args, db):
  db = [modifyCompileCommand(target, args) for target in db if isCompileTarget(target, args)]

  with open("compile_commands.json", "w") as db_file:
    json.dump(db, db_file, indent=2)


if __name__ == "__main__":
  parser = argparse.ArgumentParser(description='Generate JSON compilation database')
  parser.add_argument('--include_external', action='store_true')
  parser.add_argument('--include_genfiles', action='store_true')
  parser.add_argument('--include_headers', action='store_true')
  parser.add_argument('--vscode', action='store_true')
  # @@@
  parser.add_argument('bazel_targets', nargs='*', default=["//..."])
  args = parser.parse_args()
  fixCompilationDatabase(args, generateCompilationDatabase(args))
