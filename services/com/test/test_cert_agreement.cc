// Live check of the com/crypto certificate AGREEMENT (com_tls.hpp crypto_get_cert).
//
// Requires the stack up with crypto listening (theia start). Asserts:
//   - an UNDEPLOYED slot → crypto replies UNKNOWN_IDENTIFIER → NO_CERT
//     (the agreed "no certificate" → com would run insecure).
// The CERT_OK and UNREACHABLE branches are exercised by the env-driven
// make_server_creds path (a deployed slot / crypto stopped) — this test pins the
// common NO_CERT agreement so a regression that makes com silently insecure on a
// crypto OUTAGE (the security hole this change closes) is caught.

#include "impl/com_tls.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    // A slot that is definitely not deployed (no <slot>.crt in crypto's dir).
    std::string pem;
    auto r = services_com::crypto_get_cert(
        "test", "definitely-not-a-real-slot-xyz", pem);

    if (r == services_com::CryptoCert::UNREACHABLE) {
        // No live crypto → can't assert the agreement. Treat as skip (the live
        // robot suite gates on crypto being bound; a bare unit run without a
        // stack lands here).
        std::printf("cert-agreement: SKIP (crypto unreachable — start the stack)\n");
        return 0;
    }
    assert(r == services_com::CryptoCert::NO_CERT &&
           "an undeployed slot must yield the NO_CERT agreement, not CERT_OK");
    assert(pem.empty() && "NO_CERT must not return a PEM");
    std::printf("cert-agreement: OK — crypto reports NO_CERT for an undeployed "
                "slot (com would run insecure by agreement; a crypto outage "
                "would instead fail closed)\n");
    return 0;
}
