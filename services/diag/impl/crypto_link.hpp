// crypto_link — the UdsRouter's RemoteRef link to services/crypto's
// CryptoOneShotIf (0x80010006), for UDS 0x27 SecurityAccess seed/key. APP-OWNED.
//
// A gen_server handler (UdsRouter::handle_call) blocks on a synchronous UDS
// request/reply, so a BLOCKING RemoteRef call to crypto from inside the handler
// is correct — the tester waits for the seed/key verdict anyway. The link is a
// process-global lazily-connected RemoteRef + its own TipcMux reply pump (the
// com sup_link shape). RemoteCodec<VerifyReq/VerifyReply> matches the service_id
// crypto's register_call uses (keyed on the message's defining package).

#pragma once

#include "RemoteCodec.hh"        // THEIA_DECLARE_REMOTE_CODEC
#include "TipcMux.hh"
#include "NodeRef.hh"

#include "system/services/crypto/crypto.pb.h"   // VerifyReq/VerifyReply/HashReq...

#include <cstring>
#include <string>

// The crypto types crossing the wire need a RemoteCodec so RemoteRef encodes +
// dispatches them to the same service_id crypto's register_call registered.
THEIA_DECLARE_REMOTE_CODEC(system_services_crypto_VerifyReq)
THEIA_DECLARE_REMOTE_CODEC(system_services_crypto_VerifyReply)
THEIA_DECLARE_REMOTE_CODEC(system_services_crypto_HashReq)
THEIA_DECLARE_REMOTE_CODEC(system_services_crypto_HashReply)

namespace ara::diag {

struct CryptoLink {
    struct CryptoTag { static constexpr const char* kNodeName = "crypto_oneshot"; };
    using Ref = theia::runtime::RemoteRef<CryptoTag, 0x80010006u, 0u>;

    theia::runtime::TipcMux mux;
    Ref                     ref;
    bool                    up = false;

    static CryptoLink& instance() { static CryptoLink l; return l; }

    bool ensure() {
        if (up) return true;
        if (!ref.connect_instance(0, /*timeout_ms=*/1500)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        up = true;
        return true;
    }

    // Verify a signature over `data` against `key_slot` (SHA-256). Returns true +
    // sets `valid` on a reply; false on transport failure (caller fails CLOSED).
    bool verify(const std::string& key_slot, const std::string& data,
                const std::string& signature, bool& valid) {
        if (!ensure()) return false;
        system_services_crypto_VerifyReq req = system_services_crypto_VerifyReq_init_zero;
        std::snprintf(req.key_slot, sizeof(req.key_slot), "%s", key_slot.c_str());
        req.algo = system_services_crypto_HashAlgo_HashAlgo_SHA256;
        req.data.size = static_cast<pb_size_t>(
            data.size() > sizeof(req.data.bytes) ? sizeof(req.data.bytes) : data.size());
        std::memcpy(req.data.bytes, data.data(), req.data.size);
        req.signature.size = static_cast<pb_size_t>(
            signature.size() > sizeof(req.signature.bytes) ? sizeof(req.signature.bytes)
                                                           : signature.size());
        std::memcpy(req.signature.bytes, signature.data(), req.signature.size);

        auto r = theia::runtime::call<system_services_crypto_VerifyReply>(
            ref, req, /*act=*/0, /*timeout_ms=*/3000);
        if (r.tag != theia::runtime::CallTag::Reply) return false;
        valid = r.reply.valid;
        return true;
    }
};

}  // namespace ara::diag
