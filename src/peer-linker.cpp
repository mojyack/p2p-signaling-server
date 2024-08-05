#include "macros/unwrap.hpp"
#include "peer-linker-protocol.hpp"
#include "protocol-helper.hpp"
#include "server-args.hpp"
#include "util/string-map.hpp"
#include "ws/misc.hpp"
#include "ws/server.hpp"

namespace p2p::plink {
namespace {
struct Pad {
    std::string name;
    std::string authenticator_name;
    lws*        wsi    = nullptr;
    Pad*        linked = nullptr;
};

struct Error {
    enum {
        EmptyPadName = 0,
        AlreadyRegistered,
        NotRegistered,
        PadFound,
        PadNotFound,
        AlreadyLinked,
        NotLinked,
        AuthInProgress,
        AuthNotInProgress,
        AutherMismatched,

        Limit,
    };
};

const auto estr = std::array{
    "empty pad name",                        // EmptyPadName
    "session already has pad",               // AlreadyRegistered
    "session has no pad",                    // NotRegistered
    "pad with that name already registered", // PadFound
    "no such pad registered",                // PadNotFound
    "pad already linked",                    // AlreadyLinked
    "pad not linked",                        // NotLinked
    "another authentication in progress",    // AuthInProgress
    "pad not authenticating",                // AuthNotInProgress
    "authenticator mismatched",              // AutherMismatched
};

static_assert(Error::Limit == estr.size());

struct Server {
    ws::server::Context websocket_context;
    StringMap<Pad>      pads;
    bool                verbose = false;

    auto remove_pad(Pad* pad) -> void {
        if(pad == nullptr) {
            return;
        }
        if(pad->linked) {
            send_to(pad->linked->wsi, proto::Type::Unlinked, 0);
            pad->linked->linked = nullptr;
        }
        pads.erase(pad->name);
    }

    template <class... Args>
    auto send_to(lws* const wsi, const uint16_t type, const uint32_t id, Args... args) -> bool {
        const auto packet = p2p::proto::build_packet(type, id, args...);
        assert_b(websocket_context.send(wsi, packet));
        return true;
    }
};

struct Session {
    Server* server;
    lws*    wsi;
    Pad*    pad = nullptr;

    auto handle_payload(std::span<const std::byte> payload) -> bool;
};

auto Session::handle_payload(const std::span<const std::byte> payload) -> bool {
    unwrap_pb(header, p2p::proto::extract_header(payload));
    switch(header.type) {
    case proto::Type::Register: {
        const auto name = p2p::proto::extract_last_string<proto::Register>(payload);
        PRINT("received pad register request name:", name);

        assert_b(!name.empty(), estr[Error::EmptyPadName]);
        assert_b(pad == nullptr, estr[Error::AlreadyRegistered]);
        assert_b(server->pads.find(name) == server->pads.end(), estr[Error::PadFound]);

        PRINT("pad ", name, " registerd");
        pad = &server->pads.insert(std::pair{name, Pad{std::string(name), "", wsi, nullptr}}).first->second;
    } break;
    case proto::Type::Unregister: {
        PRINT("received unregister request");

        assert_b(pad != nullptr, estr[Error::NotRegistered]);

        PRINT("unregistering pad ", pad->name);
        server->remove_pad(pad);
        pad = nullptr;
    } break;
    case proto::Type::Link: {
        unwrap_pb(packet, p2p::proto::extract_payload<proto::Link>(payload));
        assert_b(sizeof(proto::Link) + packet.requestee_name_len + packet.secret_len == payload.size());
        const auto requestee_name = std::string_view(std::bit_cast<char*>(payload.data() + sizeof(proto::Link)), packet.requestee_name_len);
        const auto secret         = std::span(payload.data() + sizeof(proto::Link) + packet.requestee_name_len, packet.secret_len);
        PRINT("received pad link request to ", requestee_name);

        assert_b(pad != nullptr, estr[Error::NotRegistered]);
        assert_b(pad->linked == nullptr, estr[Error::AlreadyLinked]);
        assert_b(pad->authenticator_name.empty(), estr[Error::AuthInProgress]);
        const auto it = server->pads.find(requestee_name);
        assert_b(it != server->pads.end(), estr[Error::PadNotFound]);
        auto& requestee = it->second;

        PRINT("sending auth request from ", pad->name, " to ", requestee_name);
        assert_b(server->send_to(requestee.wsi, proto::Type::LinkAuth, 0,
                                 uint16_t(pad->name.size()),
                                 uint16_t(secret.size()),
                                 pad->name,
                                 secret));
        pad->authenticator_name = requestee.name;
    } break;
    case proto::Type::Unlink: {
        PRINT("received unlink request");

        assert_b(pad != nullptr, estr[Error::NotRegistered]);
        assert_b(pad->linked != nullptr, estr[Error::NotLinked]);

        PRINT("unlinking pad ", pad->name, " and ", pad->linked->name);
        assert_b(server->send_to(pad->linked->wsi, proto::Type::Unlinked, 0));
        pad->linked->linked = nullptr;
        pad->linked         = nullptr;
    } break;
    case proto::Type::LinkAuthResponse: {
        unwrap_pb(packet, p2p::proto::extract_payload<proto::LinkAuthResponse>(payload));
        const auto requester_name = p2p::proto::extract_last_string<proto::LinkAuthResponse>(payload);
        PRINT("received link auth to name: ", requester_name, " ok: ", int(packet.ok));

        assert_b(pad != nullptr, estr[Error::NotRegistered]);

        const auto it = server->pads.find(requester_name);
        assert_b(it != server->pads.end(), estr[Error::PadNotFound]);
        auto& requester = it->second;
        assert_b(!requester.authenticator_name.empty(), estr[Error::AuthNotInProgress]);
        assert_b(pad->name == requester.authenticator_name, estr[Error::AutherMismatched]);

        pad->authenticator_name.clear();
        if(packet.ok == 0) {
            assert_b(server->send_to(requester.wsi, proto::Type::LinkDenied, header.id));
        } else {
            PRINT("linking ", pad->name, " and ", requester.name);
            assert_b(server->send_to(requester.wsi, proto::Type::LinkSuccess, 0));
            pad->linked      = &requester;
            requester.linked = pad;
        }
    } break;
    default: {
        if(server->verbose) {
            PRINT("received general command ", int(header.type));
        }

        assert_b(pad != nullptr, estr[Error::NotRegistered]);
        assert_b(pad->linked != nullptr, estr[Error::NotLinked]);

        if(server->verbose) {
            PRINT("passthroughing packet from ", pad->name, " to ", pad->linked->name);
        }

        assert_b(server->websocket_context.send(pad->linked->wsi, payload));
        return true;
    }
    }

    assert_b(server->send_to(wsi, proto::Type::Success, header.id));
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
        server->remove_pad(session.pad);
        session.~Session();
    }

    SessionDataInitializer(Server& server)
        : server(&server) {}
};

auto run(const int argc, const char* argv[]) -> bool {
    unwrap_ob(args, ServerArgs::parse(argc, argv, "channel-hub"));

    auto server    = Server();
    server.verbose = args.verbose;

    auto& wsctx   = server.websocket_context;
    wsctx.handler = [&server](lws* wsi, std::span<const std::byte> payload) -> void {
        auto& session = *std::bit_cast<Session*>(ws::server::wsi_to_userdata(wsi));
        if(server.verbose) {
            PRINT("session ", &session, ": ", "received ", payload.size(), " bytes");
        }
        if(!session.handle_payload(payload)) {
            WARN("payload handling failed");

            const auto& header_o = p2p::proto::extract_header(payload);
            if(!header_o) {
                WARN("packet too short");
                assert_n(server.send_to(wsi, proto::Type::Error, 0));
            } else {
                assert_n(server.send_to(wsi, proto::Type::Error, header_o->id));
            }
        }
    };
    wsctx.session_data_initer.reset(new SessionDataInitializer(server));
    wsctx.verbose      = args.websocket_verbose;
    wsctx.dump_packets = args.websocket_dump_packets;
    ws::set_log_level(args.libws_debug_bitmap);
    assert_b(wsctx.init({
        .protocol    = "peer-linker",
        .cert        = nullptr,
        .private_key = nullptr,
        .port        = 8080,
    }));
    print("ready");
    while(wsctx.state == ws::server::State::Connected) {
        wsctx.process();
    }
    return true;
}
} // namespace
} // namespace p2p::plink

auto main(const int argc, const char* argv[]) -> int {
    return p2p::plink::run(argc, argv) ? 0 : 1;
}
