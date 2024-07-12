#include "channel-hub-protocol.hpp"
#include "macros/unwrap.hpp"
#include "protocol-helper.hpp"
#include "util/assert.hpp"
#include "util/string-map.hpp"
#include "ws/misc.hpp"
#include "ws/server.hpp"

namespace p2p::chub {
namespace {
struct Channel {
    std::string name;
    lws*        wsi;
};

struct Error {
    enum {
        EmptyChannelName = 0,
        ChannelFound,
        ChannelNotFound,
        SenderMismatch,
        AnotherRequestPending,
        RequesterNotFound,

        Limit,
    };
};

const auto estr = std::array{
    "empty channel name",                        // EmptyChannelName
    "channel with that name already registered", // ChannelFound
    "no such channel registered",                // ChannelNotFound
    "channel not registered by the sender",      // SenderMismatch
    "another request in progress",               // AnotherRequestPending
    "requester not found",                       // RequesterNotFound
};

static_assert(Error::Limit == estr.size());

struct Server;

struct Session {
    Server* server;
    lws*    wsi;

    auto handle_payload(std::span<const std::byte> payload) -> bool;
};

struct Server {
    StringMap<Channel>                     channels;
    std::unordered_map<uint32_t, Session*> pending_sessions;
    uint32_t                               packet_id;
};

auto Session::handle_payload(const std::span<const std::byte> payload) -> bool {
    unwrap_pb(header, p2p::proto::extract_header(payload));
    switch(header.type) {
    case proto::Type::Success:
    case proto::Type::Error:
        WARN("unexpected packet");
        return true;
    case proto::Type::Register: {
        const auto name = p2p::proto::extract_last_string<proto::Register>(payload);
        PRINT("received channel register request name:", name);

        assert_b(!name.empty(), estr[Error::EmptyChannelName]);
        assert_b(server->channels.find(name) == server->channels.end(), estr[Error::ChannelFound]);

        PRINT("channel ", name, " registerd");
        server->channels.insert(std::pair{name, Channel{std::string(name), wsi}});
    } break;
    case proto::Type::Unregister: {
        const auto name = p2p::proto::extract_last_string<proto::Unregister>(payload);
        PRINT("received channel unregister request name: ", name);

        const auto it = server->channels.find(name);
        assert_b(it != server->channels.end(), estr[Error::ChannelNotFound]);
        auto& channel = it->second;
        assert_b(channel.wsi == wsi, estr[Error::SenderMismatch]);

        PRINT("unregistering channel ", channel.name);
        server->channels.erase(it);
    } break;
    case proto::Type::GetChannels: {
        PRINT("received channel list request");
        auto payload = std::vector<std::byte>();
        for(auto it = server->channels.begin(); it != server->channels.end(); it = std::next(it)) {
            const auto& name      = it->second.name;
            const auto  prev_size = payload.size();
            payload.resize(prev_size + name.size() + 1);
            std::memcpy(payload.data() + prev_size, name.data(), name.size() + 1);
        }

        p2p::proto::send_packet(wsi, proto::Type::GetChannelsResponse, header.id, payload);
        return true;
    } break;
    case proto::Type::PadRequest: {
        const auto name = p2p::proto::extract_last_string<proto::PadRequest>(payload);
        PRINT("received pad request for channel: ", name);

        // check if another request is pending
        for(auto i = server->pending_sessions.begin(); i != server->pending_sessions.end(); i = std::next(i)) {
            assert_b(i->second != this, estr[Error::AnotherRequestPending]);
        }

        const auto it = server->channels.find(name);
        assert_b(it != server->channels.end(), estr[Error::ChannelNotFound]);
        auto& channel = it->second;

        const auto id = server->packet_id += 1;
        server->pending_sessions.insert({id, this});
        p2p::proto::send_packet(channel.wsi, proto::Type::PadRequest, id, name);
    } break;
    case proto::Type::PadRequestResponse: {
        PRINT("received pad request response");

        unwrap_pb(packet, p2p::proto::extract_payload<proto::PadRequestResponse>(payload));
        const auto pad_name = p2p::proto::extract_last_string<proto::PadRequestResponse>(payload);

        const auto requester_it = server->pending_sessions.find(header.id);
        assert_b(requester_it != server->pending_sessions.end(), estr[Error::RequesterNotFound]);
        const auto requester = requester_it->second;
        server->pending_sessions.erase(header.id);

        PRINT("sending pad name ok: ", packet.ok, " pad_name: ", pad_name);
        p2p::proto::send_packet(requester->wsi, proto::Type::PadRequestResponse, 0, packet.ok, pad_name);
    } break;
    default: {
        WARN("unknown command ", int(header.type));
        return false;
    }
    }

    p2p::proto::send_packet(wsi, proto::Type::Success, header.id);
    return true;
}

struct SessionDataInitializer : ws::server::SessionDataInitializer {
    Server* server;

    auto get_size() -> size_t override {
        return sizeof(Session);
    }

    auto init(void* const ptr, lws* wsi) -> void override {
        PRINT("session created: ", ptr);
        auto& session  = *new(ptr) Session();
        session.server = server;
        session.wsi    = wsi;
    }

    auto deinit(void* const ptr) -> void override {
        PRINT("session destroyed: ", ptr);
        auto& session = *std::bit_cast<Session*>(ptr);

        // remove corresponding channels
        std::erase_if(server->channels, [&session](const auto& p) { return p.second.wsi == session.wsi; });

        // remove from pending list
        std::erase_if(server->pending_sessions, [&session](const auto& p) { return p.second->wsi == session.wsi; });

        session.~Session();
    }

    SessionDataInitializer(Server& server)
        : server(&server) {}
};

auto run() -> bool {
    auto server = Server();

    auto wsctx    = ws::server::Context();
    wsctx.handler = [](lws* wsi, std::span<const std::byte> payload) -> void {
        auto& session = *std::bit_cast<Session*>(ws::server::wsi_to_userdata(wsi));
        PRINT("session ", &session, ": ", "received ", payload.size(), " bytes");
        if(!session.handle_payload(payload)) {
            WARN("payload handling failed");

            const auto& header_o = p2p::proto::extract_header(payload);
            if(!header_o) {
                WARN("packet too short");
                p2p::proto::send_packet(wsi, proto::Type::Error, 0);
            } else {
                p2p::proto::send_packet(wsi, proto::Type::Error, header_o->id);
            }
        }
    };
    wsctx.session_data_initer.reset(new SessionDataInitializer(server));
    wsctx.verbose      = true;
    wsctx.dump_packets = true;
    assert_b(wsctx.init(8081, "channel-hub"));
    while(wsctx.state == ws::server::State::Connected) {
        wsctx.process();
    }
    return true;
}
} // namespace
} // namespace p2p::chub

auto main() -> int {
    return p2p::chub::run() ? 0 : 1;
}
