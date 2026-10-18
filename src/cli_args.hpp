#pragma once
// Tiny argument parser: `--key value`, `--flag`, and positional arguments.

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace cli {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
      const std::string a = argv[i];
      if (a.rfind("--", 0) == 0) {
        const std::string key = a.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
          opts_[key] = argv[++i];
        } else {
          opts_[key] = "";
        }
      } else {
        positional.push_back(a);
      }
    }
  }

  bool has(const std::string& k) const { return opts_.count(k) > 0; }
  std::string get_str(const std::string& k, const std::string& dflt) const {
    const auto it = opts_.find(k);
    return it == opts_.end() ? dflt : it->second;
  }
  std::int64_t get_int(const std::string& k, std::int64_t dflt) const {
    const auto it = opts_.find(k);
    return it == opts_.end() || it->second.empty() ? dflt : std::strtoll(it->second.c_str(), nullptr, 10);
  }
  double get_double(const std::string& k, double dflt) const {
    const auto it = opts_.find(k);
    return it == opts_.end() || it->second.empty() ? dflt : std::strtod(it->second.c_str(), nullptr);
  }

  std::vector<std::string> positional;

 private:
  std::map<std::string, std::string> opts_;
};

}  // namespace cli
