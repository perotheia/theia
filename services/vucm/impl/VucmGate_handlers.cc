// User handler bodies for VucmGate.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/VucmGate.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/VucmGate.hh"

#include <cstdio>

namespace ara::vucm {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/VucmGate_state.hh.
void VucmGate::init(VucmGateState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void VucmGate::handle_info(const char* /*info*/, VucmGateState& /*s*/) {
}


void VucmGate::handle_cast(const EvDeployment& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to EvDeployment (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvDeployment\n",
                 kNodeName);
}

void VucmGate::handle_cast(const EvPlanned& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to EvPlanned (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvPlanned\n",
                 kNodeName);
}

void VucmGate::handle_cast(const EvAuthorized& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to EvAuthorized (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvAuthorized\n",
                 kNodeName);
}

void VucmGate::handle_cast(const EvInstalled& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to EvInstalled (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvInstalled\n",
                 kNodeName);
}

void VucmGate::handle_cast(const EvValidated& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to EvValidated (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvValidated\n",
                 kNodeName);
}

void VucmGate::handle_cast(const EvBlocked& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to EvBlocked (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvBlocked\n",
                 kNodeName);
}

void VucmGate::handle_cast(const EvFailed& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to EvFailed (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvFailed\n",
                 kNodeName);
}

void VucmGate::handle_cast(const SmStateMsg& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to SmStateMsg (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received SmStateMsg\n",
                 kNodeName);
}

void VucmGate::handle_cast(const PhmHealthStatus& /*msg*/,
                                 VucmGateState& /*s*/) {
    // TODO: react to PhmHealthStatus (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received PhmHealthStatus\n",
                 kNodeName);
}



CampaignReply VucmGate::handle_call(
        const CampaignRequest& /*req*/,
        VucmGateState& /*s*/) {
    // TODO: implement CheckForCampaign (CampaignRequest →
    //                                 CampaignReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] CheckForCampaign called\n",
                 kNodeName);
    return CampaignReply{};
}

CampaignProgress VucmGate::handle_call(
        const CampaignStatusReq& /*req*/,
        VucmGateState& /*s*/) {
    // TODO: implement GetCampaignStatus (CampaignStatusReq →
    //                                 CampaignProgress).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] GetCampaignStatus called\n",
                 kNodeName);
    return CampaignProgress{};
}


}  // namespace ara::vucm
