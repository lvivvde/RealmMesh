#include "realmmesh/game/queue/queue_store.hpp"

#include "realmmesh/test_support/etcd_process.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace realm::game::queue {
namespace {

using namespace std::chrono_literals;

TEST(QueueStoreEtcdIntegrationTest, ColdBackupRecoversAcknowledgedIssue) {
    test_support::EtcdProcess etcd;
    etcd.wait_ready();
    const EtcdQueueStore::Options options{
        .budget_prefix = "/realmmesh/test/budgets",
        .snapshot_key = "/realmmesh/test/queue/snapshot",
        .issuance_prefix = "/realmmesh/test/queue/issuance",
    };
    const auto now = std::chrono::system_clock::now();
    const auto expires_at = now + 30min;

    auto primary_client =
        cluster::make_etcd_http_client(etcd.endpoint(), 2s);
    EtcdQueueStore primary(options, primary_client);
    const auto first = primary.issue_or_recover(QueueIssueRequest{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = now,
        .identity_expires_at = expires_at,
        .snapshot = QueueSnapshot{},
    });
    ASSERT_EQ(first.status, QueueIssueStatus::Issued);
    ASSERT_EQ(first.number, 1U);
    ASSERT_EQ(first.snapshot.next_number, 2U);

    auto backup_client = cluster::make_etcd_http_client(etcd.endpoint(), 2s);
    EtcdQueueStore cold_backup(options, backup_client);
    const auto restored = cold_backup.load_snapshot();
    ASSERT_TRUE(restored.has_value());
    const auto replay = cold_backup.issue_or_recover(QueueIssueRequest{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = now + 1s,
        .identity_expires_at = expires_at,
        .snapshot = *restored,
    });
    EXPECT_EQ(replay.status, QueueIssueStatus::Recovered);
    EXPECT_EQ(replay.number, 1U);
    EXPECT_EQ(replay.issued_at, first.issued_at);
    EXPECT_EQ(replay.snapshot.next_number, 2U);

    const auto distinct = cold_backup.issue_or_recover(QueueIssueRequest{
        .identity_jti = "fedcba9876543210fedcba9876543210",
        .issued_at = now + 1s,
        .identity_expires_at = expires_at,
        .snapshot = replay.snapshot,
    });
    EXPECT_EQ(distinct.status, QueueIssueStatus::Issued);
    EXPECT_EQ(distinct.number, 2U);
    EXPECT_EQ(distinct.snapshot.next_number, 3U);
}

TEST(QueueStoreEtcdIntegrationTest, IdentityExpiryRemovesRecoveryMapping) {
    test_support::EtcdProcess etcd;
    etcd.wait_ready();
    const EtcdQueueStore::Options options{
        .budget_prefix = "/realmmesh/expiry/budgets",
        .snapshot_key = "/realmmesh/expiry/queue/snapshot",
        .issuance_prefix = "/realmmesh/expiry/queue/issuance",
    };
    const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto expires_seconds = ((now_seconds + 60) / 60) * 60;
    const auto issued_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{expires_seconds - 1}};
    const auto expires_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{expires_seconds}};

    EtcdQueueStore primary(
        options, cluster::make_etcd_http_client(etcd.endpoint(), 2s));
    const auto first = primary.issue_or_recover(QueueIssueRequest{
        .identity_jti = "expiring-attempt",
        .issued_at = issued_at,
        .identity_expires_at = expires_at,
        .snapshot = QueueSnapshot{},
    });
    ASSERT_EQ(first.status, QueueIssueStatus::Issued);

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
        EtcdQueueStore retry(
            options,
            cluster::make_etcd_http_client(etcd.endpoint(), 2s));
        const auto restored = retry.load_snapshot();
        ASSERT_TRUE(restored.has_value());
        const auto retry_time = std::chrono::system_clock::now();
        const auto result = retry.issue_or_recover(QueueIssueRequest{
            .identity_jti = "expiring-attempt",
            .issued_at = retry_time,
            .identity_expires_at = retry_time + 30min,
            .snapshot = *restored,
        });
        if (result.status == QueueIssueStatus::Issued) {
            EXPECT_EQ(result.number, 2U);
            return;
        }
    }
    FAIL() << "issuance mapping outlived its identity lease";
}

TEST(QueueStoreEtcdIntegrationTest, MeasuresIssuanceWriteLoadAgainstTarget) {
    test_support::EtcdProcess etcd;
    etcd.wait_ready();
    EtcdQueueStore store(
        {.budget_prefix = "/realmmesh/load/budgets",
         .snapshot_key = "/realmmesh/load/queue/snapshot",
         .issuance_prefix = "/realmmesh/load/queue/issuance"},
        cluster::make_etcd_http_client(etcd.endpoint(), 2s));

    constexpr std::uint64_t issuance_count = 1'700;
    const auto issued_at = std::chrono::system_clock::now();
    const auto expires_at = issued_at + 30min;
    QueueSnapshot snapshot;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < issuance_count; ++index) {
        const auto result = store.issue_or_recover(QueueIssueRequest{
            .identity_jti = "load-attempt-" + std::to_string(index),
            .issued_at = issued_at,
            .identity_expires_at = expires_at,
            .snapshot = snapshot,
        });
        ASSERT_EQ(result.status, QueueIssueStatus::Issued) << index;
        snapshot = result.snapshot;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto seconds = std::chrono::duration<double>(elapsed).count();
    const auto per_second = static_cast<double>(issuance_count) / seconds;
    ::testing::Test::RecordProperty(
        "committed_numbers_per_second", per_second);
    std::cout << "queue issuance write load: " << per_second
              << " committed numbers/s\n";
    // 吞吐是环境测量值，不是跨机器稳定的单测门槛；ADR-0006 记录
    // 本机实测及其与 1700/s 设计假设的差距，#91 在目标 Linux
    // 环境定门槛。
    EXPECT_GT(per_second, 0.0);
}

}  // namespace
}  // namespace realm::game::queue
