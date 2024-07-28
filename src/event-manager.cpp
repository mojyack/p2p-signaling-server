#include <optional>

#include "event-manager.hpp"
#include "macros/unwrap.hpp"

namespace p2p {
auto Events::invoke(uint32_t kind, const uint32_t id, const uint32_t value) -> void {
    if(debug) {
        if(id != no_id) {
            PRINT("new event kind: ", kind, " id: ", id, " value: ", value);
        } else {
            PRINT("new event kind: ", kind, " value: ", value);
        }
    }

    auto found = std::optional<EventHandlerInfo>();
    {
        auto guard = std::lock_guard(lock);
        for(auto i = handlers.begin(); i < handlers.end(); i += 1) {
            if(i->kind == kind && i->id == id) {
                found = std::move(*i);
                handlers.erase(i);
                break;
            }
        }
    }
    unwrap_on(info, found, "unhandled event");
    info.handler(value);
}

auto Events::add_handler(EventHandlerInfo info) -> void {
    if(debug) {
        PRINT("new event handler registered kind: ", info.kind, " id: ", info.id);
    }

    auto guard = std::lock_guard(lock);
    handlers.push_back(info);
}

auto Events::drain() -> void {
    if(debug) {
        PRINT("draining...");
    }

loop:
    auto found = std::optional<EventHandlerInfo>();
    {
        auto guard = std::lock_guard(lock);
        if(handlers.empty()) {
            return;
        }
        found = std::move(handlers.back());
        handlers.pop_back();
    }
    found->handler(0);
    goto loop;
}
} // namespace p2p
