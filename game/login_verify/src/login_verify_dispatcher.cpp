#include "realmmesh/game/login_verify/login_verify_dispatcher.hpp"

#include "realmmesh/game/common/json.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace realm::game::login_verify {
namespace {

[[nodiscard]] network::Http1Response verifier_busy_response() {
    network::Http1Response response;
    response.status = 503;
    response.headers.emplace_back("Content-Type", "application/json");
    response.headers.emplace_back(
        "Retry-After", std::to_string(verifier_busy_retry_after.count()));
    response.body = common::JsonCodec::encode(
        {{"code", static_cast<std::int64_t>(error_verifier_busy)},
         {"message", common::JsonValue(std::string("verifier busy"))},
         {"retry_after_seconds",
          static_cast<std::int64_t>(verifier_busy_retry_after.count())}});
    return response;
}

}  // namespace

LoginVerifyDispatcher::LoginVerifyDispatcher(
    const LoginVerifyHandler& handler, Limits limits)
    : handler_(&handler), pool_(limits.workers, limits.capacity) {}

network::HttpHandlerResult LoginVerifyDispatcher::dispatch(
    const network::Http1Request& request, network::HttpResponseToken token) {
    if (!LoginVerifyHandler::is_verification(request.method, request.target)) {
        return handler_->handle(request.method, request.target, request.body);
    }
    const auto submitted = pool_.try_submit(
        token,
        [handler = handler_,
         method = request.method,
         target = request.target,
         body = request.body]() noexcept -> network::Http1Response {
            try {
                return handler->handle(method, target, body);
            } catch (...) {
                return {.status = 500, .headers = {}, .body = ""};
            }
        });
    if (submitted != concurrency::WorkSubmitResult::Submitted) {
        return verifier_busy_response();
    }
    return network::HttpDeferred{};
}

std::vector<LoginVerifyDispatcher::Completion> LoginVerifyDispatcher::drain(
    std::size_t max_items) {
    std::vector<Completion> completions;
    for (auto& completion : pool_.drain(max_items)) {
        completions.push_back(
            {.token = completion.id, .response = std::move(completion.result)});
    }
    return completions;
}

}  // namespace realm::game::login_verify
