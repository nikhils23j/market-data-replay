#pragma once
// mmap'd read-only files, whole-file writes, and the key=value truth file.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mdr {

class MappedFile {
 public:
  explicit MappedFile(const std::string& path);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  [[nodiscard]] const std::uint8_t* data() const { return data_; }
  [[nodiscard]] std::size_t size() const { return size_; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes);
void write_kv(const std::string& path, const std::map<std::string, std::string>& kv);
std::map<std::string, std::string> read_kv(const std::string& path);

}  // namespace mdr
