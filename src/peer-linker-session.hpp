#pragma once
#include "websocket-session.hpp"

namespace p2p::plink {
struct EventKind {
    enum {
        Linked = wss::EventKind::Limit,

        Limit,
    };
};

struct PeerLinkerSessionParams {
    wss::ServerLocation peer_linker;
    wss::ServerLocation stun_server;
    std::string_view    pad_name;
    std::string_view    target_pad_name;
    const char*         bind_address = nullptr;
};

class PeerLinkerSession : public wss::WebSocketSession {
  private:
    auto get_error_packet_type() const -> uint16_t override;

  protected:
    virtual auto on_pad_created() -> void;
    virtual auto auth_peer(std::string_view peer_name) -> bool;
    virtual auto on_packet_received(std::span<const std::byte> payload) -> bool override;

  public:
    auto start(const PeerLinkerSessionParams& params) -> bool;

    virtual ~PeerLinkerSession();
};
} // namespace p2p::plink
