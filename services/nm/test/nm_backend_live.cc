// Standalone live exerciser for the NM backend observers (nm_backend.hpp).
//
// nm_backend.hpp is self-contained (libc + Linux headers only — no theia
// runtime, no TIPC), so it compiles + runs natively on ANY Linux box, including
// hosts whose kernel lacks the TIPC module (e.g. the Raspberry Pi). This proves
// the WiFi-management observation path on REAL hardware (a real wlan0 +
// association) independent of the full FC stack.
//
// Build + run on the rig (e.g. the Pi):
//   g++ -std=c++17 -I services nm_backend_live.cc -o /tmp/nm_live && /tmp/nm_live
// (run with sudo for an active `iw scan`; falls back to `scan dump` otherwise.)
//
// It walks the same observers NmPoller/NmDaemon call:
//   probe_link()       — the netlink link/addr observation (carrier/address)
//   wifi_assoc_probe() — the cheap per-tick association probe
//   wifi_observe()     — the full scan behind the WifiScan op (tdb/rtdb `wifi`)
//   vpn_observe()      — the tailscale/wg tunnel probe (VPN_ESTABLISHED rung)
// and prints the readiness ladder rung the live observations imply.

#include "nm/impl/nm_backend.hpp"

#include <cstdio>
#include <string>

using namespace ara::nm;

int main(int argc, char** argv) {
    const std::string want = argc > 1 ? argv[1] : "";   // optional iface hint

    // Connect mode: `nm_backend_live <iface> connect <ssid> [psk]` drives the
    // wpa_cli sequence (the DRIVE path) instead of just observing.
    if (argc >= 4 && std::string(argv[2]) == "connect") {
        std::string iface = wifi_iface(want);
        std::string ssid = argv[3];
        std::string psk = argc > 4 ? argv[4] : "";
        std::printf("=== wifi_connect('%s', '%s', psk=%s) ===\n",
                    iface.c_str(), ssid.c_str(), psk.empty() ? "(open)" : "***");
        ConnectResult cr = wifi_connect(iface, ssid, psk);
        std::printf("connect: ok=%d — %s\n", cr.ok, cr.note.c_str());
        return cr.ok ? 0 : 1;
    }

    std::printf("=== NM backend live observation ===\n");

    // 1. Link/address (netlink) — the wired/base rung.
    LinkObservation link = probe_link(want);
    std::printf("[link] iface=%s carrier=%d address=%d\n",
                link.interface.empty() ? "(none)" : link.interface.c_str(),
                link.has_carrier, link.has_address);

    // 2. WiFi association (cheap probe) — the WIFI_ASSOCIATED rung.
    WifiObservation assoc;
    bool is_wifi = wifi_assoc_probe(want, assoc);
    std::printf("[wifi-assoc] wireless=%d iface=%s associated=%d ssid=%s bssid=%s\n",
                is_wifi, assoc.interface.empty() ? "(none)" : assoc.interface.c_str(),
                assoc.associated,
                assoc.assoc_ssid.empty() ? "-" : assoc.assoc_ssid.c_str(),
                assoc.assoc_bssid.empty() ? "-" : assoc.assoc_bssid.c_str());

    // 3. Full scan — the WifiScan op (tdb/rtdb `wifi`).
    if (is_wifi) {
        WifiObservation scan = wifi_observe(want);
        std::printf("[wifi-scan] %zu AP(s) on %s:\n", scan.bss.size(),
                    scan.interface.c_str());
        int shown = 0;
        for (const auto& b : scan.bss) {
            if (shown++ >= 12) { std::printf("    ... (%zu more)\n",
                                             scan.bss.size() - 12); break; }
            std::printf("    %-20s %-18s %4d dBm  %5u MHz  %s%s\n",
                        b.ssid.empty() ? "(hidden)" : b.ssid.c_str(),
                        b.bssid.c_str(), b.signal_dbm, b.freq_mhz,
                        b.security.c_str(),
                        b.bssid == scan.assoc_bssid ? "  *associated" : "");
        }
    }

    // 4. VPN tunnel — the VPN_ESTABLISHED rung.
    VpnObservation vpn;
    bool has_vpn = vpn_observe("", vpn);
    std::printf("[vpn] mechanism=%d iface=%s up=%d (%s)\n",
                has_vpn, vpn.interface.c_str(), vpn.up, vpn.note.c_str());

    // 5. The readiness rung the live observations imply (the FSM ladder).
    const char* rung;
    if (!link.has_carrier)                 rung = "NETWORK_OFF";
    else if (is_wifi && !assoc.associated) rung = "LINK_AVAILABLE (wifi, not associated)";
    else if (!link.has_address && is_wifi) rung = "WIFI_ASSOCIATED";
    else if (!link.has_address)            rung = "LINK_AVAILABLE";
    else if (vpn.up)                       rung = "VPN_ESTABLISHED → NETWORK_OPERATIONAL";
    else                                   rung = "IP_ACQUIRED (no VPN)";
    std::printf("=== implied readiness rung: %s ===\n", rung);
    return 0;
}
