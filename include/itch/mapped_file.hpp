// Read-only memory-mapped file (POSIX: Linux and macOS).
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace itch {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error("fstat " + path + ": " + std::strerror(errno));
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) return;
        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) {
            ::close(fd_);
            throw std::runtime_error("mmap " + path + ": " + std::strerror(errno));
        }
        data_ = static_cast<const std::uint8_t*>(p);
        ::madvise(const_cast<std::uint8_t*>(data_), size_, MADV_SEQUENTIAL);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Touches every page so later timing runs measure parsing, not page faults.
    std::uint64_t prefault() const noexcept {
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < size_; i += 4096) sum += data_[i];
        return sum;
    }

private:
    int fd_ = -1;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace itch
