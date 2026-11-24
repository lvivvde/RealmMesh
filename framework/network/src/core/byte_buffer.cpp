#include "realmmesh/network/core/byte_buffer.hpp"

#include <stdexcept>

namespace realm::network {

void ByteBuffer::append(std::span<const std::byte> data) {
    storage_.insert(storage_.end(), data.begin(), data.end());
}

bool ByteBuffer::empty() const noexcept {
    return readable_bytes() == 0;
}

std::size_t ByteBuffer::readable_bytes() const noexcept {
    return storage_.size() - read_offset_;
}

std::span<const std::byte> ByteBuffer::readable_data() const noexcept {
    if (empty()) {
        return {};
    }

    return std::span<const std::byte>{storage_.data() + read_offset_, readable_bytes()};
}

void ByteBuffer::consume(std::size_t byte_count) {
    if (byte_count > readable_bytes()) {
        throw std::out_of_range("cannot consume beyond readable bytes");
    }

    read_offset_ += byte_count;
    if (read_offset_ == storage_.size()) {
        clear();
        return;
    }

    compact_if_needed();
}

void ByteBuffer::clear() noexcept {
    storage_.clear();
    read_offset_ = 0;
}

void ByteBuffer::compact_if_needed() {
    constexpr std::size_t compact_threshold = 4096;
    if (read_offset_ < compact_threshold || read_offset_ < storage_.size() / 2) {
        return;
    }

    storage_.erase(storage_.begin(), storage_.begin() + static_cast<std::ptrdiff_t>(read_offset_));
    read_offset_ = 0;
}

}  // namespace realm::network
