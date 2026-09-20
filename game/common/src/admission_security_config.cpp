#include "realmmesh/game/common/admission_security_config.hpp"

#include <stdexcept>

namespace realm::game::common {

void QueueAdmissionSecurityConfig::validate() const {
    if (!active_queue_number_key.has_value() ||
        !active_admission_grant_key.has_value()) {
        throw std::invalid_argument(
            "Queue Scheduler requires one active signing key per credential role");
    }

    // 构造模块即执行各自的 kid、key ring、TTL/window/issuer/deployment
    // 校验；对象只活到本函数返回，不启动或切换任何线上协议。
    static_cast<void>(QueueNumberV2Codec(
        *active_queue_number_key,
        queue_number_verification_keys,
        queue_number_ttl));
    static_cast<void>(AdmissionGrantIssuer(
        *active_admission_grant_key,
        admission_grant));
    static_cast<void>(AdmissionGrantVerifier(
        admission_grant_verification_keys,
        admission_grant));

    if (active_queue_number_key->kid == active_admission_grant_key->kid) {
        throw std::invalid_argument(
            "credential roles must not share an active kid");
    }
    const auto queue_active =
        ed25519_public_key_from_seed(active_queue_number_key->seed);
    const auto grant_active =
        ed25519_public_key_from_seed(active_admission_grant_key->seed);
    if (queue_active == grant_active) {
        throw std::invalid_argument(
            "credential roles must not share signing material");
    }
    bool grant_active_present = false;
    for (const auto& grant_key : admission_grant_verification_keys) {
        if (grant_key.kid == active_admission_grant_key->kid) {
            if (grant_key.public_key != grant_active) {
                throw std::invalid_argument(
                    "Admission Grant active kid does not match signing key");
            }
            grant_active_present = true;
        }
    }
    if (!grant_active_present) {
        throw std::invalid_argument(
            "Admission Grant verification ring omits active signing key");
    }
    for (const auto& queue_key : queue_number_verification_keys) {
        for (const auto& grant_key : admission_grant_verification_keys) {
            if (queue_key.kid == grant_key.kid ||
                queue_key.public_key == grant_key.public_key) {
                throw std::invalid_argument(
                    "credential verification key rings are role-confused");
            }
        }
    }
}

}  // namespace realm::game::common
