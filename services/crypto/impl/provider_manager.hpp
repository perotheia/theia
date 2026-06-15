// provider_manager — the Crypto Service Manager's provider router (AUTOSAR CP
// broker). Routes each request to a backend (Software=OpenSSL, Hardware=HSM
// stub) by SLOT; callers never know which serves them. Phase 4 of
// docs/tasks/PROGRESS/grpc-certificates.md — the portability point: changing a
// slot's backend is a config change here, with zero change to the FC handler or
// any caller (com TLS).
//
// Routing config: THEIA_CRYPTO_SLOT_PROVIDERS = "slotA=hw,slotB=sw,…" (or a
// crypto param). Unlisted slots use the default (software). A real deploy maps
// the HSM-resident slots to "hw"; today "hw" is the stub (proves the seam).
//
// Context handles are provider-LOCAL. create() tags the returned handle with its
// owning provider (top bit set for hardware) so start/update/finish route back
// to the same backend. Hash (no slot) uses the default provider.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "impl/crypto_provider.hpp"
#include "impl/software_provider.hpp"
#include "impl/hardware_provider.hpp"

namespace ara::crypto {

class CryptoManager {
public:
    CryptoManager(std::string slot_dir, std::string hsm_device,
                  std::string routing)
        : sw_(std::move(slot_dir)), hw_(std::move(hsm_device)) {
        parse_routing_(routing);
    }

    // The backend that owns a given slot (default: software).
    ICryptoProvider& for_slot(const std::string& slot) {
        auto it = route_.find(slot);
        return (it != route_.end() && it->second) ? hw_impl_() : sw_;
    }

    // The default backend (for slot-less ops like hash).
    ICryptoProvider& default_provider() { return sw_; }

    // ---- context routing (handles are provider-local) ---------------------
    // create() on the slot's provider, then tag the handle so later ctx ops
    // route back. We keep a small handle→provider map (the .art ctx handle is
    // opaque to the caller, so we can remap freely).
    uint64_t create(int kind, int algo, const std::string& slot,
                    ProviderResult& err) {
        ICryptoProvider& p = (kind == CK_HASH) ? default_provider()
                                               : for_slot(slot);
        uint64_t h = p.create(kind, algo, slot, err);
        if (h != 0) {
            std::lock_guard<std::mutex> lk(mu_);
            ctx_owner_[h] = &p;
        }
        return h;
    }
    ProviderResult start(uint64_t h)  { return ctx(h).start(h); }
    ProviderResult update(uint64_t h, const uint8_t* d, size_t n) {
        return ctx(h).update(h, d, n);
    }
    ProviderResult finish(uint64_t h, const uint8_t* s, size_t n) {
        auto r = ctx(h).finish(h, s, n);
        std::lock_guard<std::mutex> lk(mu_);
        ctx_owner_.erase(h);   // released on finish
        return r;
    }

private:
    void parse_routing_(const std::string& routing) {
        // "slotA=hw,slotB=sw" — only "hw" entries matter (default is sw).
        size_t i = 0;
        while (i < routing.size()) {
            size_t comma = routing.find(',', i);
            std::string item = routing.substr(i, comma - i);
            size_t eq = item.find('=');
            if (eq != std::string::npos) {
                std::string slot = item.substr(0, eq);
                std::string be   = item.substr(eq + 1);
                if (!slot.empty()) route_[slot] = (be == "hw" || be == "hardware");
            }
            if (comma == std::string::npos) break;
            i = comma + 1;
        }
    }

    // Hardware impl accessor (kept a method so a future "no HSM compiled" build
    // can swap it). Today always the stub.
    ICryptoProvider& hw_impl_() { return hw_; }

    // The provider that owns a context handle (falls back to default if lost).
    ICryptoProvider& ctx(uint64_t h) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = ctx_owner_.find(h);
        return it != ctx_owner_.end() ? *it->second : sw_;
    }

    SoftwareProvider sw_;
    HardwareProvider hw_;
    std::map<std::string, bool> route_;     // slot → is-hardware
    std::mutex mu_;
    std::map<uint64_t, ICryptoProvider*> ctx_owner_;
};

}  // namespace ara::crypto
