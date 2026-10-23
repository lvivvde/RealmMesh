#pragma once
#include <memory>
namespace realm::service_host { class ServiceModule; struct ServiceModuleContext; }
namespace realm::game::chat {
[[nodiscard]] std::unique_ptr<service_host::ServiceModule> make_chat_module(
    const service_host::ServiceModuleContext& context);
}
