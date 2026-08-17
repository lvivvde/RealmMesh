#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace realm::network {

class ByteBuffer final {
public:
    void append(std::span<const std::byte> data);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t readable_bytes() const noexcept;
    [[nodiscard]] std::span<const std::byte> readable_data() const noexcept;

    void consume(std::size_t byte_count);
    void clear() noexcept;

private:
    void compact_if_needed();

    std::vector<std::byte> storage_;
    std::size_t read_offset_{0};
};

}  // namespace realm::network
