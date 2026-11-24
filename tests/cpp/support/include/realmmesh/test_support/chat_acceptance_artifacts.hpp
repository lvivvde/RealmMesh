#pragma once

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

namespace realm::test_support {

inline std::optional<std::filesystem::path>
chat_acceptance_case_directory() {
    const auto* root = std::getenv("REALMMESH_CHAT_ARTIFACT_DIRECTORY");
    const auto* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    if (!root || !info) return std::nullopt;
    auto result = std::filesystem::path(root) / info->name();
    std::filesystem::create_directories(result);
    return result;
}

inline void copy_chat_acceptance_file(
    const std::filesystem::path& source,
    const std::filesystem::path& relative_target) {
    const auto root = chat_acceptance_case_directory();
    if (!root || !std::filesystem::is_regular_file(source)) return;
    const auto target = *root / relative_target;
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::copy_file(
        source,
        target,
        std::filesystem::copy_options::overwrite_existing);
}

inline void copy_chat_acceptance_directory(
    const std::filesystem::path& source,
    const std::filesystem::path& relative_target) {
    const auto root = chat_acceptance_case_directory();
    if (!root || !std::filesystem::is_directory(source)) return;
    const auto target = *root / relative_target;
    std::filesystem::create_directories(target);
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(source)) {
        const auto destination =
            target / entry.path().lexically_relative(source);
        if (entry.is_directory())
            std::filesystem::create_directories(destination);
        else if (entry.is_regular_file()) {
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(
                entry.path(),
                destination,
                std::filesystem::copy_options::overwrite_existing);
        }
    }
}

inline void write_chat_acceptance_artifact(
    const std::filesystem::path& relative_target,
    std::string_view contents) {
    const auto root = chat_acceptance_case_directory();
    if (!root) return;
    const auto target = *root / relative_target;
    std::filesystem::create_directories(target.parent_path());
    std::ofstream(target) << contents;
}

}  // namespace realm::test_support
