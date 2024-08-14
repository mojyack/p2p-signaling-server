#pragma once
#include <juice/juice.h>

#include "peer-linker-session.hpp"

namespace p2p::ice {
declare_autoptr(JuiceAgent, juice_agent_t, juice_destroy);

struct EventKind {
    enum {
        Connected = plink::EventKind::Limit,
        SDPSet,
        RemoteGatheringDone,
        Linked,

        Limit,
    };
};

struct IceSessionParams {
    wss::ServerLocation              stun_server;
    std::vector<juice_turn_server_t> turn_servers;
};

class IceSession : public plink::PeerLinkerSession {
  private:
    AutoJuiceAgent agent;
    std::string    remote_sdp;
    Event          sdp_set_event;
    Event          gathering_done_event;
    Event          connected_event;

  protected:
    virtual auto on_packet_received(std::span<const std::byte> payload) -> bool override;

  public:
    // internal use
    auto on_p2p_connected_state(bool flag) -> void;
    auto on_p2p_new_candidate(std::string_view sdp) -> void;
    auto on_p2p_gathering_done() -> void;

    // api
    virtual auto on_p2p_packet_received(std::span<const std::byte> payload) -> void;

    auto start(const IceSessionParams& params, const plink::PeerLinkerSessionParams& plink_params) -> bool;
    auto start_ice(const IceSessionParams& params, const plink::PeerLinkerSessionParams& plink_params) -> bool;
    auto send_packet_p2p(const std::span<const std::byte> payload) -> bool;

    IceSession();
    virtual ~IceSession() {}
};
} // namespace p2p::ice
