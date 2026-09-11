#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace realm::test_support {

/// 临时目录:名字 = 前缀 + pid + 进程内自增序号,析构时递归清理。
///
/// 序号保证同一进程内多个实例互不冲突(只用 pid 的话,同一测试里前后两个
/// 临时目录会拿到同一个路径)。测试把配置树拷进来再改,避免向源码树写日志。
class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(std::string prefix) {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                (prefix +
                 std::to_string(static_cast<long long>(::getpid())) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

}  // namespace realm::test_support
