#include "mdr/files.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>

namespace mdr {

MappedFile::MappedFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open " + path);
  struct stat sb {};
  if (::fstat(fd, &sb) != 0) {
    ::close(fd);
    throw std::runtime_error("cannot stat " + path);
  }
  size_ = static_cast<std::size_t>(sb.st_size);
  if (size_ > 0) {
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
      ::close(fd);
      throw std::runtime_error("mmap failed for " + path);
    }
    ::madvise(p, size_, MADV_SEQUENTIAL);
    data_ = static_cast<const std::uint8_t*>(p);
  }
  ::close(fd);
}

MappedFile::~MappedFile() {
  if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write " + path);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) throw std::runtime_error("write failed for " + path);
}

void write_kv(const std::string& path, const std::map<std::string, std::string>& kv) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write " + path);
  for (const auto& [k, v] : kv) out << k << '=' << v << '\n';
}

std::map<std::string, std::string> read_kv(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::map<std::string, std::string> kv;
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return kv;
}

}  // namespace mdr
