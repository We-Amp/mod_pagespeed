#ifndef PAGESPEED_IIS_IIS_CONFIG_UTIL_H_
#define PAGESPEED_IIS_IIS_CONFIG_UTIL_H_

#include <string>
#include <vector>

namespace iis_config_util {

// Tokenize a string by splitting on characters for which the predicate |fp|
// returns non-zero (e.g. isspace).  Supports double-quote delimited tokens:
// content between quotes is kept verbatim (spaces included).  Doubled quotes
// ("") inside a quoted region produce a single literal quote character.
inline std::vector<std::string> tokenize(const std::string& s,
                                         int (*fp)(int)) {
  std::vector<std::string> r;
  std::string tmp;
  bool instring = false;

  for (size_t i = 0; i < s.size(); i++) {
    char current = s[i];
    if (fp(static_cast<unsigned char>(s[i])) && !instring) {
      if (tmp.size()) {
        r.push_back(tmp);
        tmp = "";
      }
    } else {
      if (current == '\"') {
        if (instring) {
          if (i < (s.size() - 1) && s[i + 1] == '\"') {
            tmp += s[i];
            i++;
            continue;
          } else {
          }
        }
        instring = !instring;
        continue;
      } else {
        tmp += s[i];
      }
    }
  }

  if (tmp.size()) {
    r.push_back(tmp);
  }

  return r;
}

}  // namespace iis_config_util

#endif  // PAGESPEED_IIS_IIS_CONFIG_UTIL_H_
