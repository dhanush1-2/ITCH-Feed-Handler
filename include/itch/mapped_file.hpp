// include/itch/mapped_file.hpp
#pragma once

// Read-only memory map of a whole file. Errors are std::runtime_error with a message that
// names the path and says what to do next, because the usual cause is the wrong file, not
// a bug: a missing path, an empty download, or the .gz that fetch_itch.py has not
// decompressed yet.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace itch {

class MappedFile {
public:
    [[nodiscard]] static MappedFile open(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("cannot open '" + path + "': " + std::strerror(errno) +
                                     ". Check the path; scripts/fetch_itch.py downloads the data.");
        }
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            const int err = errno;
            ::close(fd);
            throw std::runtime_error("cannot stat '" + path + "': " + std::strerror(err));
        }
        if (!S_ISREG(st.st_mode)) {
            ::close(fd);
            throw std::runtime_error("'" + path + "' is not a regular file");
        }
        if (st.st_size == 0) {
            ::close(fd);
            throw std::runtime_error("'" + path +
                                     "' is empty. The download may have failed; rerun "
                                     "scripts/fetch_itch.py.");
        }
        const auto size = static_cast<std::size_t>(st.st_size);

        // A gzip file is not an ITCH stream: its first two bytes would be read as a frame
        // length and the census would report garbage instead of the real problem.
        unsigned char magic[2] = {0, 0};
        if (size >= 2 && ::pread(fd, magic, 2, 0) == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
            ::close(fd);
            throw std::runtime_error("'" + path +
                                     "' is gzip-compressed. Decompress it first: "
                                     "scripts/fetch_itch.py writes the decompressed file.");
        }

        void*     p   = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        const int err = errno;
        ::close(fd);  // the mapping outlives the descriptor
        if (p == MAP_FAILED) {
            throw std::runtime_error("cannot map '" + path + "': " + std::strerror(err));
        }
        // Advisory only; a failure here costs speed, not correctness.
        (void)::madvise(p, size, MADV_SEQUENTIAL);
        return MappedFile(p, size);
    }

    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& o) noexcept
        : addr_(std::exchange(o.addr_, nullptr)), size_(std::exchange(o.size_, 0)) {}

    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this != &o) {
            release();
            addr_ = std::exchange(o.addr_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    ~MappedFile() { release(); }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(addr_), size_};
    }

    // Touch every page so the first timed pass does not pay for page faults.
    void prefault() const noexcept {
        const long page = ::sysconf(_SC_PAGESIZE);
        const auto step = page > 0 ? static_cast<std::size_t>(page) : std::size_t{4096};
        const auto data = bytes();
        unsigned   sink = 0;
        for (std::size_t i = 0; i < data.size(); i += step) {
            sink += std::to_integer<unsigned>(static_cast<const volatile std::byte&>(data[i]));
        }
        sink_ = sink;  // keeps the loop from being optimised away
    }

private:
    MappedFile(void* addr, std::size_t size) noexcept : addr_(addr), size_(size) {}

    void release() noexcept {
        if (addr_ != nullptr) {
            ::munmap(addr_, size_);
            addr_ = nullptr;
            size_ = 0;
        }
    }

    void*                     addr_ = nullptr;
    std::size_t               size_ = 0;
    mutable volatile unsigned sink_ = 0;
};

}  // namespace itch
