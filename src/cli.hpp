#pragma once
// Tiny "--key value" argument parser shared by the command-line tools.

#include <map>
#include <stdexcept>
#include <string>

namespace mdr {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      if (k.rfind("--", 0) != 0) throw std::invalid_argument("expected --option, got " + k);
      k = k.substr(2);
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) kv_[k] = argv[++i];
      else kv_[k] = "1";  // boolean flag
    }
  }
  bool has(const std::string& k) const { return kv_.count(k) != 0; }
  std::string str(const std::string& k, const std::string& def) const {
    used_[k] = true;
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  std::uint64_t u64(const std::string& k, std::uint64_t def) const {
    return has(k) ? std::stoull(str(k, "")) : (used_[k] = true, def);
  }
  double f64(const std::string& k, double def) const { return has(k) ? std::stod(str(k, "")) : (used_[k] = true, def); }
  bool flag(const std::string& k) const {
    used_[k] = true;
    return has(k);
  }
  void reject_unknown() const {
    for (const auto& [k, v] : kv_)
      if (!used_.count(k)) throw std::invalid_argument("unknown option --" + k);
  }

 private:
  std::map<std::string, std::string> kv_;
  mutable std::map<std::string, bool> used_;
};

}  // namespace mdr
