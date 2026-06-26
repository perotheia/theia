// mender_install — UCM's installer BACK-END (L4-C step 3/4). The clean ARA/Mender
// separation: the ARA story (V-UCM campaign → UCM lifecycle → PROVISIONAL → confirm)
// is SEPARATE from the Mender story (Mender fetches+writes the bits). UCM owns the
// lifecycle; it DELEGATES the actual fetch+write to Mender by running a STANDALONE
// install of its ROLE artifact — `mender install <url>` (not the server-pull agent
// loop). Mender's update module (theia-release) does the on-disk work.
//
// ROLE resolution: a campaign carries a BUNDLE {fleet/app/version}; each board
// installs only ITS role slice. The role IS the board's machine name (THEIA_MACHINE
// — central/compute/…), the same identity the activation marker keys on. The role
// artifact is <bundle>/<role>.mender (or .deb), so a compute board fetches
// compute.mender. The bundle base comes from the manifest artifact_path.
//
// Pluggable: THEIA_UCM_MENDER selects the back-end —
//   "mender"   (default): exec `mender install <url>` (or `mender-update install`)
//   "simulate"          : log + succeed (the role artifact is assumed pre-staged,
//                         e.g. the colony local-install / the L4-B marker test) so
//                         the ARA fan-out + role-resolution run without a live
//                         Mender artifact on the bench.

#pragma once

#include <cstdlib>
#include <string>

namespace ara::ucm {

// This board's role = THEIA_MACHINE (central/compute/…); "" → single-board "theia".
inline std::string board_role() {
    const char* m = std::getenv("THEIA_MACHINE");
    return (m && *m) ? std::string(m) : std::string("theia");
}

// The role artifact reference for a bundle base + version. bundle_base is the
// manifest artifact_path (e.g. s3://theia-runtime/<ver> or /opt/theia/releases/
// <ver>); the role slice is <bundle_base>/<role>.mender. Empty bundle_base →
// the role-named artifact under the default plane (caller decides the prefix).
inline std::string role_artifact(const std::string& bundle_base,
                                 const std::string& /*version*/) {
    std::string base = bundle_base;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base.empty() ? (board_role() + ".mender")
                        : (base + "/" + board_role() + ".mender");
}

// Result of an install attempt.
struct InstallResult {
    bool        ok = false;
    int         rc = -1;
    std::string detail;
};

// Run the standalone Mender install of the role artifact. Returns ok=false (with
// detail) on a non-zero exit so UCM fails-closed → ROLLBACK. The "simulate"
// back-end (or a missing mender binary) logs + succeeds so the ARA lifecycle runs
// on a bench without a live Mender artifact.
InstallResult mender_standalone_install(const std::string& artifact_url);

}  // namespace ara::ucm
