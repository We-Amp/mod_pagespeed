// data2c: turns a data file (JavaScript, CSS, HTML) into a C++ source file
// that defines the file's contents as a string constant.
//
// The tool runs as a build-time host tool for genrules, so it deliberately
// depends on nothing but the C++ standard library: anything it links is
// compiled a second time for the exec configuration.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

const char kOutputPrefix[] =
    "/*\n"
    " * Licensed to the Apache Software Foundation (ASF) under one\n"
    " * or more contributor license agreements.  See the NOTICE file\n"
    " * distributed with this work for additional information\n"
    " * to you under the Apache License, Version 2.0 (the\n"
    " * \"License\"); you may not use this file except in compliance\n"
    " * with the License.  You may obtain a copy of the License at\n"
    " * \n"
    " *   http://www.apache.org/licenses/LICENSE-2.0\n"
    " * \n"
    " * Unless required by applicable law or agreed to in writing,\n"
    " * software distributed under the License is distributed on an\n"
    " * \"AS IS\" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY\n"
    " * KIND, either express or implied.  See the License for the\n"
    " * specific language governing permissions and limitations\n"
    " * under the License.\n"
    " */\n"
    "// Automatically generated from ";

// Number of input bytes per string-literal line in the output.
const size_t kChunkSize = 60;

// Escapes `src` for use inside a C string literal. Matches the escaping of
// net_instaweb::CEscape so the generated files do not change.
void AppendCEscaped(const std::string& src, size_t pos, size_t len,
                    std::string* out) {
  for (size_t i = pos; i < pos + len; ++i) {
    unsigned char ch = static_cast<unsigned char>(src[i]);
    switch (ch) {
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      case '\"':
        *out += "\\\"";
        break;
      case '\'':
        *out += "\\\'";
        break;
      case '\\':
        *out += "\\\\";
        break;
      default:
        if (ch < 32 || ch >= 127) {
          char buf[5];
          std::snprintf(buf, sizeof(buf), "\\%03o", ch);
          *out += buf;
        } else {
          *out += static_cast<char>(ch);
        }
        break;
    }
  }
}

// Accepts --name=value, -name=value, --name value and -name value.
bool ParseFlags(int argc, char* argv[], std::string* data_file,
                std::string* c_file, std::string* varname) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.size() < 2 || arg[0] != '-') {
      std::fprintf(stderr, "data2c: unexpected argument '%s'\n", argv[i]);
      return false;
    }
    arg.erase(0, arg[1] == '-' ? 2 : 1);
    std::string name = arg;
    std::string value;
    size_t eq = arg.find('=');
    if (eq != std::string::npos) {
      name = arg.substr(0, eq);
      value = arg.substr(eq + 1);
    } else if (i + 1 < argc) {
      value = argv[++i];
    } else {
      std::fprintf(stderr, "data2c: missing value for '%s'\n", argv[i]);
      return false;
    }
    if (name == "data_file") {
      *data_file = value;
    } else if (name == "c_file") {
      *c_file = value;
    } else if (name == "varname") {
      *varname = value;
    } else {
      std::fprintf(stderr, "data2c: unknown flag '%s'\n", name.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string data_file = "/tmp/a.js";
  std::string c_file = "/tmp/a.c";
  std::string varname = "str";
  if (!ParseFlags(argc, argv, &data_file, &c_file, &varname)) {
    std::fprintf(stderr,
                 "usage: data2c --data_file=<in> --c_file=<out> "
                 "--varname=<name>\n");
    return EXIT_FAILURE;
  }

  std::ifstream in(data_file, std::ios::in | std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "data2c: cannot read '%s'\n", data_file.c_str());
    return EXIT_FAILURE;
  }
  std::string input((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  if (in.bad()) {
    std::fprintf(stderr, "data2c: error reading '%s'\n", data_file.c_str());
    return EXIT_FAILURE;
  }

  std::string output = kOutputPrefix;
  output += data_file;
  output += "\n\nnamespace net_instaweb {\n\nconst char* ";
  output += varname;
  output += " =";
  for (size_t i = 0; i < input.size(); i += kChunkSize) {
    size_t len = input.size() - i < kChunkSize ? input.size() - i : kChunkSize;
    output += "\n    \"";
    AppendCEscaped(input, i, len, &output);
    output += "\"";
  }
  output += ";\n\n}  // namespace net_instaweb\n";

  std::ofstream out(c_file, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "data2c: cannot write '%s'\n", c_file.c_str());
    return EXIT_FAILURE;
  }
  out.write(output.data(), static_cast<std::streamsize>(output.size()));
  out.close();
  if (!out) {
    std::fprintf(stderr, "data2c: error writing '%s'\n", c_file.c_str());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
