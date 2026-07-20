// User handler bodies for VucmGate — the L4-B vehicle campaign orchestrator.
//
// VucmGate is the ONLY TIPC-reachable VUCM surface. It serves VucmCtlIf
// (Mender/com → CheckForCampaign), drives the VucmCampaign FSM through the
// campaign lifecycle, and runs the L4-B aggregate barrier: fan RequestUpdate to
// the boards, wait for EVERY board's PROVISIONAL marker in the SHARED etcd
// keyspace (ucm_activation_<board>), then fan the aggregate Confirm so all boards
// activate together — or on any-fail/timeout, fan Cancel so all roll back.
//
// The gate↔FSM idiom mirrors ucm (UcmGate↔UcmFsm): the gate post_event()s the
// .art transition triggers into the campaign statem (for the STATE + the
// CampaignProgress broadcast) AND re-posts the next lifecycle step to ITSELF to
// drive the chain a mailbox step at a time (no deep recursion).

#include "lib/VucmGate.hh"
#include "lib/VucmCampaign.hh"      // the campaign FSM the gate post_event()s into
#include "impl/VucmGate_state.hh"

#include "GenStateM.hh"            // theia::runtime::post_event
#include "NodeRef.hh"             // LocalRef / RemoteRef / cast / call
#include "TimerService.hh"        // send_after / process_timers (confirm poll)
#include "TipcMux.hh"             // reply pump for the per / ucm calls
#include "RemoteCodec.hh"         // THEIA_DECLARE_REMOTE_CODEC
#include "ParamsConfig.hh"        // get_config() — boards / confirm budget
#include "system/services/per/per.pb.h"
#include "system/services/ucm/ucm.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <unistd.h>              // readlink (current-release version)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// NOTE: the per (GetConfigReq/ConfigSnapshot) + ucm (ConfirmRequest/CancelRequest/
// UcmReply) RemoteCodecs are already declared by the generated vucm_codecs.hh
// (pulled via lib/VucmGate.hh) — the to_per + to_ucm client ports put them in the
// codec set. Re-declaring them here would redefine RemoteCodec<T>. UcmActivation
// (the marker payload) is decoded inline below, not over a port, so it needs no
// codec — only pb_decode of the raw bytes from the ConfigSnapshot.

// The config-migration MECHANISM, shared with UCM. V-UCM is the VEHICLE's
// migration authority (it runs ON the vehicle, not on the fleet): it migrates the
// vehicle's shared per ONCE (Snapshot + MigrateBulk), centrally, BEFORE fanning
// RequestUpdate to the vehicle's N boards — one per/etcd singleton → one migration.
// (per_migrate.hpp's PerReply/Snapshot/MigrateBulk/RestoreSnapshot codecs are
// #ifndef-guarded, so they compose with vucm_codecs.hh's per set.)
#include "impl/per_migrate.hpp"
using theia::migrate::MigrationStep;
using theia::migrate::load_migration_steps;
using theia::migrate::per_snapshot;
using theia::migrate::per_restore;
using theia::migrate::per_migrate_bulk;
using theia::migrate::plugin_path;
using theia::migrate::version_delta_migrates;

namespace ara::vucm {

// ---- IMPL-owned shared singletons (the gate→campaign wiring) ---------------
// The campaign FSM peer. VucmCampaign::on_enter publishes itself here on first
// entry; the gate post_event's the transition triggers into it.
theia::runtime::LocalRef<VucmCampaign>& vucm_campaign_ref() {
    static theia::runtime::LocalRef<VucmCampaign> ref;
    return ref;
}
// The gate peer. VucmGate::init publishes itself here so handlers can re-post the
// NEXT lifecycle event onto the gate's own mailbox (drive the chain).
theia::runtime::LocalRef<VucmGate>& vucm_gate_ref() {
    static theia::runtime::LocalRef<VucmGate> ref;
    return ref;
}

namespace {

// Advance the campaign FSM (→ CampaignProgress broadcast for that state).
template <typename Evt>
void to_fsm(const char* name, Evt evt) {
    auto& ref = vucm_campaign_ref();
    if (!ref.valid()) {
        std::fprintf(stderr, "[vucm_gate] %s before FSM wired — dropping\n", name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}
// Re-post the next gate step onto the gate's OWN mailbox (a mailbox step, not
// recursion — runs after the current handler returns).
template <typename Evt>
void to_gate(Evt evt) {
    auto& ref = vucm_gate_ref();
    if (ref.valid()) cast(ref, std::move(evt));
}

// ---- per (shared-etcd) link — the SOLE etcd reader. Lazily connected, its own
//      TipcMux reply pump. Mirrors UcmGate's PerLink exactly.
struct PerLink {
    struct PerClientTag { static constexpr const char* kNodeName = "per_client"; };
    using PerRef = ::theia::runtime::RemoteRef<PerClientTag, 0x80010007u, 0u>;
    PerRef                    ref;
    ::theia::runtime::TipcMux mux;
    bool                      started = false;
    std::mutex                mu;
    static PerLink& instance() { static PerLink l; return l; }
    bool ensure_started() {
        if (started) return true;
        if (!ref.connect(/*timeout_ms=*/2000)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        started = true;
        return true;
    }
};

// ---- PER-BOARD ucm (UpdateCtl) link — L4-C. UcmDaemon serves UpdateCtl
//      (RequestUpdate/Confirm/Cancel) at TIPC 0x8001000E, instance = the board's
//      MACHINE INDEX (the supervisor shifts each child's --tipc instance by
//      THEIA_MACHINE_INSTANCE: central=0, compute=1, …). So V-UCM reaches a
//      SPECIFIC board's UCM via connect_instance(idx) — the cross-board fan-out.
//      One link per board index, lazily connected; a board whose UCM isn't
//      reachable just fails the call (logged, the campaign reacts).
constexpr uint32_t kUcmDaemonTipcType = 0x8001000Eu;

struct UcmLink {
    struct UcmDaemonTag { static constexpr const char* kNodeName = "ucm_daemon"; };
    using UcmRef = ::theia::runtime::RemoteRef<UcmDaemonTag, kUcmDaemonTipcType, 0u>;
    UcmRef                    ref;
    ::theia::runtime::TipcMux mux;
    bool                      started = false;
    uint32_t                  board_idx = 0;
    // One UcmLink per board index (central=0, compute=1, …). connect_instance()
    // aims the ref at THAT board's UcmDaemon across the TIPC cluster bearer.
    static UcmLink& for_board(uint32_t idx) {
        static std::mutex mu;
        static std::map<uint32_t, UcmLink*> links;
        std::lock_guard<std::mutex> lk(mu);
        auto it = links.find(idx);
        if (it != links.end()) return *it->second;
        auto* l = new UcmLink();
        l->board_idx = idx;
        links.emplace(idx, l);
        return *l;
    }
    bool ensure_started() {
        if (started) return true;
        if (!ref.connect_instance(board_idx, /*timeout_ms=*/2000)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        started = true;
        return true;
    }
};

// The board→machine-index map. PRIMARY source: an EXPLICIT config string
// (VucmConfig.board_instances = "central:0,compute:1") set in init() — robust, no
// dependency on the cluster machine manifest being deployed (it often isn't on a
// colony rig). The machine-manifest scan below is the fallback.
std::map<std::string, uint32_t>& board_index_map() {
    static std::map<std::string, uint32_t> m;
    return m;
}
void set_board_instances(const std::string& csv) {
    auto& m = board_index_map();
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto c = tok.find(':');
        if (c == std::string::npos) continue;
        std::string name = tok.substr(0, c);
        // trim
        size_t a = name.find_first_not_of(" \t");
        size_t z = name.find_last_not_of(" \t");
        if (a != std::string::npos) name = name.substr(a, z - a + 1);
        m[name] = static_cast<uint32_t>(std::strtoul(tok.c_str() + c + 1, nullptr, 10));
    }
}

// Resolve a board NAME → its MACHINE INDEX (the TIPC instance shift). PRIMARY:
// the explicit board_instances config map. FALLBACK: the cluster machine manifest
// (THEIA_MACHINE_MANIFEST → machines.json order); then a discovery-order cache.
uint32_t board_index(const std::string& board) {
    static std::mutex mu;
    static std::map<std::string, uint32_t> cache;
    static std::vector<std::string> order;   // discovery order = last-resort index
    std::lock_guard<std::mutex> lk(mu);
    // PRIMARY: the explicit config map.
    if (auto it = board_index_map().find(board); it != board_index_map().end())
        return it->second;
    auto it = cache.find(board);
    if (it != cache.end()) return it->second;
    // machines.json under THEIA_MACHINE_MANIFEST lists machine names in index
    // order. Parse just the "machines":[...] array (no json dep — a tiny scan).
    uint32_t idx = static_cast<uint32_t>(order.size());
    if (const char* root = std::getenv("THEIA_MACHINE_MANIFEST"); root && *root) {
        std::string path = std::string(root) + "/machines.json";
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::string buf;
            char tmp[512]; size_t n;
            while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, n);
            std::fclose(f);
            // find the board name's ordinal among the quoted strings in machines[].
            size_t pos = 0; uint32_t ord = 0; bool found = false;
            while ((pos = buf.find('"', pos)) != std::string::npos) {
                size_t end = buf.find('"', pos + 1);
                if (end == std::string::npos) break;
                std::string tok = buf.substr(pos + 1, end - pos - 1);
                pos = end + 1;
                if (tok == "machines") continue;   // the key itself
                if (tok == board) { idx = ord; found = true; break; }
                ++ord;
            }
            if (!found) idx = ord;   // append at the end if not listed
        }
    }
    cache.emplace(board, idx);
    order.push_back(board);
    return idx;
}

// Read one board's PROVISIONAL marker from the SHARED etcd keyspace. Returns the
// ActivationState (0 NONE / 1 PROVISIONAL / 2 CONFIRMED / -1 error) for
// ucm_activation_<board>. The whole point of L4-B's shared etcd: V-UCM reads
// EVERY board's marker locally (its per → the central's etcd), no cross-board TIPC.
// Read board's ucm_activation_<board> marker. out_campaign receives the marker's
// OWN campaign_id (field 3) so the CONFIRMING poll can reject a STALE marker left
// by a PREVIOUS campaign — a board that reached PROVISIONAL for campaign X must
// NOT count toward campaign Y's barrier. Returns the ActivationState (0 NONE /
// 1 PROVISIONAL / 2 CONFIRMED / -1 err).
int read_board_marker(const std::string& board, std::string* out_campaign = nullptr) {
    if (out_campaign) out_campaign->clear();
    system_services_per_GetConfigReq req = system_services_per_GetConfigReq_init_zero;
    std::string key = "ucm_activation_" + board;
    std::snprintf(req.target_node, sizeof(req.target_node), "%s", key.c_str());

    auto& link = PerLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) return -1;
    auto result = ::theia::runtime::call<system_services_per_ConfigSnapshot>(
        link.ref, req, 0, 3000);
    if (result.tag != ::theia::runtime::CallTag::Reply) return -1;
    if (result.reply.config.size == 0) return 0;   // no marker == NONE

    system_services_ucm_UcmActivation a = system_services_ucm_UcmActivation_init_zero;
    pb_istream_t is = pb_istream_from_buffer(result.reply.config.bytes,
                                             result.reply.config.size);
    if (!pb_decode(&is, system_services_ucm_UcmActivation_fields, &a)) return -1;
    if (out_campaign) *out_campaign = a.campaign_id;
    return static_cast<int>(a.state);
}

// ---- L4-C.4: V-UCM campaign PERSISTENCE — survive the supervisor restart in
//      step 5 (and a coordinator reboot). The campaign lives in the SHARED etcd
//      under key "vucm_campaign" so a restarted V-UCM reads it back + resumes the
//      CONFIRMING poll (the GS poll then returns the right state before AND after
//      the restart). Record = a tab-delimited line (V-UCM owns both ends; no proto
//      churn): campaign_id \t version \t scope \t phase \t boards_csv. phase=="" /
//      no key → idle.
constexpr const char* kCampaignNode = "vucm_campaign";

void write_campaign(const std::string& campaign_id, const std::string& version,
                    uint32_t scope, const std::string& phase,
                    const std::vector<std::string>& boards) {
    std::string boards_csv;
    for (size_t i = 0; i < boards.size(); ++i)
        boards_csv += (i ? "," : "") + boards[i];
    std::string rec = campaign_id + "\t" + version + "\t" +
                      std::to_string(scope) + "\t" + phase + "\t" + boards_csv;

    system_services_per_PutConfigReq req = system_services_per_PutConfigReq_init_zero;
    std::snprintf(req.target_node, sizeof(req.target_node), "%s", kCampaignNode);
    size_t n = rec.size();
    if (n > sizeof(req.config.bytes)) n = sizeof(req.config.bytes);
    std::memcpy(req.config.bytes, rec.data(), n);
    req.config.size = static_cast<pb_size_t>(n);
    req.expect_rev = 0;

    auto& link = PerLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) return;
    ::theia::runtime::call<system_services_per_PerReply>(link.ref, req, 0, 3000);
}

// Read the persisted campaign on (re)start. Returns true + fills the out-params if
// a non-idle campaign is stored. phase "" (or no key) → false (idle).
bool read_campaign(std::string& id, std::string& version, uint32_t& scope,
                   std::string& phase, std::vector<std::string>& boards) {
    system_services_per_GetConfigReq req = system_services_per_GetConfigReq_init_zero;
    std::snprintf(req.target_node, sizeof(req.target_node), "%s", kCampaignNode);
    auto& link = PerLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) return false;
    auto r = ::theia::runtime::call<system_services_per_ConfigSnapshot>(
        link.ref, req, 0, 3000);
    if (r.tag != ::theia::runtime::CallTag::Reply || r.reply.config.size == 0)
        return false;
    std::string rec(reinterpret_cast<const char*>(r.reply.config.bytes),
                    r.reply.config.size);
    // split on tab
    std::vector<std::string> f; size_t p = 0, t;
    while ((t = rec.find('\t', p)) != std::string::npos) { f.push_back(rec.substr(p, t - p)); p = t + 1; }
    f.push_back(rec.substr(p));
    if (f.size() < 5 || f[3].empty() || f[3] == "IDLE") return false;
    id = f[0]; version = f[1];
    scope = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 10));
    phase = f[3];
    boards.clear();
    { std::stringstream ss(f[4]); std::string b;
      while (std::getline(ss, b, ',')) if (!b.empty()) boards.push_back(b); }
    return true;
}

// Fan RequestUpdate to a SPECIFIC board's UCM (step 2 of the user story). The
// manifest names the role package; UCM resolves its own slice + invokes its
// installer back-end (Mender standalone), then holds PROVISIONAL. confirm_window>0
// → the manifest carries `confirm=<ms>` so UCM holds for the aggregate Confirm.
bool ucm_request_update(const std::string& board, const std::string& version,
                        const std::string& campaign_id, uint32_t scope,
                        uint32_t confirm_window_ms, const std::string& bundle_base) {
    system_services_ucm_PackageManifest m =
        system_services_ucm_PackageManifest_init_zero;
    std::snprintf(m.name, sizeof(m.name), "theia");
    std::snprintf(m.version, sizeof(m.version), "%s", version.c_str());
    m.kind  = system_services_ucm_UpdateKind_UpdateKind_UK_SOFTWARE;
    m.scope = static_cast<system_services_ucm_UpdateScope>(scope);
    // L4-C/D: the bundle base — UCM's role_artifact() appends /<role>.mender and
    // runs `mender install` of it. Empty → UCM simulate (the bench/no-bundle path).
    std::snprintf(m.artifact_path, sizeof(m.artifact_path), "%s", bundle_base.c_str());
    // carry confirm=<ms> + campaign=<id> in the manifest `requires` so UCM holds
    // PROVISIONAL for the aggregate Confirm and tags its marker with the campaign.
    int rc = 0;
    if (confirm_window_ms > 0)
        std::snprintf(m.requires[rc++], sizeof(m.requires[0]), "confirm=%u", confirm_window_ms);
    std::snprintf(m.requires[rc++], sizeof(m.requires[0]), "campaign=%s", campaign_id.c_str());
    m.requires_count = static_cast<pb_size_t>(rc);

    auto& link = UcmLink::for_board(board_index(board));
    if (!link.ensure_started()) {
        std::fprintf(stderr, "[vucm_gate] board %s UCM (inst %u) unreachable\n",
                     board.c_str(), board_index(board));
        return false;
    }
    auto r = ::theia::runtime::call<system_services_ucm_UcmReply>(link.ref, m, 0, 6000);
    return r.tag == ::theia::runtime::CallTag::Reply;
}

void ucm_confirm(const std::string& board, const std::string& campaign_id) {
    system_services_ucm_ConfirmRequest req = system_services_ucm_ConfirmRequest_init_zero;
    std::snprintf(req.campaign_id, sizeof(req.campaign_id), "%s", campaign_id.c_str());
    auto& link = UcmLink::for_board(board_index(board));
    if (!link.ensure_started()) return;
    ::theia::runtime::call<system_services_ucm_UcmReply>(link.ref, req, 0, 3000);
}
void ucm_cancel(const std::string& board, const std::string& campaign_id) {
    system_services_ucm_CancelRequest req = system_services_ucm_CancelRequest_init_zero;
    std::snprintf(req.campaign_id, sizeof(req.campaign_id), "%s", campaign_id.c_str());
    auto& link = UcmLink::for_board(board_index(board));
    if (!link.ensure_started()) return;
    ::theia::runtime::call<system_services_ucm_UcmReply>(link.ref, req, 0, 3000);
}

std::vector<std::string> split_boards(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string b;
    while (std::getline(ss, b, ',')) {
        size_t a = b.find_first_not_of(" \t");
        size_t z = b.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(b.substr(a, z - a + 1));
    }
    return out;
}

constexpr unsigned kPollMs = 2000;   // confirm-poll cadence


// True iff `now` (UTC minutes-of-day) is inside [start,end); start>end
// spans midnight; 0/0 = always (no window). Shared by admission + auto-confirm.
bool in_window(uint32_t start_min, uint32_t end_min) {
    if (start_min == 0 && end_min == 0) return true;      // no window configured
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    const uint32_t now = static_cast<uint32_t>(tm.tm_hour * 60 + tm.tm_min);
    if (start_min <= end_min) return now >= start_min && now < end_min;
    return now >= start_min || now < end_min;             // spans midnight
}

// The master's currently-installed release version = readlink(<root>/current) with
// the "releases/" prefix stripped. "" if unresolved (a fresh box). This is V-UCM's
// FROM-version for the version-delta gate — no separate tracking / proto field.
std::string current_release_version(const std::string& releases_root) {
    // releases_root is "<root>/releases"; current is "<root>/current".
    const std::string root = releases_root.substr(0, releases_root.find_last_of('/'));
    const std::string link = root + "/current";
    char buf[512];
    const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string tgt(buf);
    const std::string pfx = "releases/";
    const size_t p = tgt.rfind(pfx);
    return (p != std::string::npos) ? tgt.substr(p + pfx.size()) : tgt;
}
}  // namespace

// ---- OTP init/1 — publish the gate peer + read the campaign config. ---------
void VucmGate::init(VucmGateState& s) {
    vucm_gate_ref() = theia::runtime::LocalRef<VucmGate>(*this);
    auto cfg = ::theia::runtime::get_config().node(kNodeName);
    s.cfg_boards        = split_boards(cfg.str("boards", ""));
    s.confirm_budget_ms = cfg.u32("confirm_budget_ms", 120000);
    s.bundle_base       = cfg.str("bundle_base", "");
    // Where V-UCM reads the target release's migration/ to drive the CENTRAL
    // config migration. Defaults to the standard prefix; THEIA_ROOT_DIR (set by
    // `theia start`) overrides it for a non-/opt install.
    if (const char* rd = std::getenv("THEIA_ROOT_DIR"); rd && *rd)
        s.releases_root = std::string(rd) + "/releases";
    s.releases_root     = cfg.str("releases_root", s.releases_root);
    // Update-admission knobs (AUTHORIZING conjunction + garage window).
    s.enforce_sm           = cfg.u32("enforce_sm", 0) != 0;
    s.enforce_nm           = cfg.u32("enforce_nm", 0) != 0;
    s.enforce_phm          = cfg.u32("enforce_phm", 0) != 0;
    s.min_net_state        = cfg.u32("min_net_state", 6);
    s.window_start_min     = cfg.u32("window_start_min", 0);
    s.window_end_min       = cfg.u32("window_end_min", 0);
    s.require_user_confirm = cfg.u32("require_user_confirm", 0) != 0;
    s.auto_confirm_in_window = cfg.u32("auto_confirm_in_window", 0) != 0;
    // board_instances: the explicit board→machine-index map ("central:0,compute:1")
    // so connect_instance(idx) reaches the right board's UCM without depending on
    // the cluster machine manifest being deployed.
    set_board_instances(cfg.str("board_instances", ""));
    const char* m = std::getenv("THEIA_MACHINE");
    s.self_board        = (m && *m) ? m : "central";
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_IDLE;
    s.campaign_id.clear();
    this->log().info(std::string("vucm gate up — board=") + s.self_board +
                     " roster=" + std::to_string(s.cfg_boards.size()) +
                     " (vehicle campaign orchestrator)");

    // L4-C.4 RECOVERY: a supervisor restart (step 5) or coordinator reboot wiped
    // our in-memory campaign. Read the persisted campaign back from the shared
    // etcd; if one was in CONFIRMING, RESUME the aggregate poll (the boards' own
    // PROVISIONAL markers survive in etcd too — [[L4-A]] reboot-resume), so the GS
    // poll returns the right state after the restart. A campaign that was still
    // INSTALLING when we died re-enters CONFIRMING (the markers tell the truth).
    std::string cid, ver, phase; uint32_t sc = 0; std::vector<std::string> bds;
    if (read_campaign(cid, ver, sc, phase, bds)) {
        s.campaign_id = cid; s.version = ver; s.scope = sc;
        s.boards = bds.empty() ? s.cfg_boards : bds;
        s.confirm_ticks = 0;
        s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_CONFIRMING;
        this->log().info(std::string("RESUMING campaign ") + cid + " (was " +
                         phase + ") — re-entering CONFIRMING aggregate poll");
        ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                     kPollMs, *this, "confirm_poll");
    }
}

// ---- string handle_info — the confirm-poll timer. Polls every board's
//      PROVISIONAL marker; ALL PROVISIONAL → EvProvisioned + fan Confirm;
//      budget exceeded → EvFailed + fan Cancel.
void VucmGate::handle_info(const char* info, VucmGateState& s) {
    // Admission re-check: a BLOCKED campaign (vehicle moving / tunnel /
    // outside the window) retries until admitted or operator-cancelled.
    if (info && std::strcmp(info, "authorize_poll") == 0) {
        s.authorize_pending = false;
        if (!s.campaign_id.empty() &&
            s.last_state ==
                system_services_vucm_CampaignState_CampaignState_CMP_PLANNING) {
            to_gate(EvPlanned{});   // re-enter AUTHORIZING via the gate chain
            to_fsm("EvPlanned", EvPlanned{});
        }
        return;
    }
    if (!info || std::strcmp(info, "confirm_poll") != 0) return;
    if (s.campaign_id.empty() || s.boards.empty()) return;

    size_t provisional = 0;
    for (const auto& b : s.boards) {
        std::string marker_campaign;
        int st = read_board_marker(b, &marker_campaign);  // 0 NONE 1 PROV 2 CONF -1 err
        // Count ONLY a PROVISIONAL/CONFIRMED marker that belongs to THIS campaign —
        // a stale marker from a previous campaign (same board, old campaign_id) must
        // not satisfy the barrier. (An empty marker_campaign is a legacy/unstamped
        // marker; treat it as matching so a board UCM that omits the id still works.)
        if ((st == 1 || st == 2) &&
            (marker_campaign.empty() || marker_campaign == s.campaign_id))
            ++provisional;
    }

    if (provisional == s.boards.size()) {
        s.confirm_ticks = 0;
        // GARAGE AUTO-CONFIRM (case 2): the user consented up front, so once
        // every board is PROVISIONAL AND we're inside the window, VUCM fires
        // the aggregate Confirm ITSELF (pre-consent IS the confirm) — the
        // vehicle "finishes before morning" with no live operator. PROVISIONAL's
        // deadline still auto-cancels a stuck board, and a broken update still
        // rolls back downstream (PHM verify / migration fail).
        if (s.auto_confirm_in_window &&
            in_window(s.window_start_min, s.window_end_min)) {
            this->log().info(std::string("campaign ") + s.campaign_id + ": ALL " +
                             std::to_string(s.boards.size()) +
                             " boards PROVISIONAL, in-window — AUTO-CONFIRM (garage)");
            to_gate(EvCommit{});   // → fan Confirm to every board → VALIDATING
            return;
        }
        // Case 1 (operator-first): HOLD for a live CommitCampaign/Rollback.
        this->log().info(std::string("campaign ") + s.campaign_id + ": ALL " +
                         std::to_string(s.boards.size()) +
                         " boards PROVISIONAL — AWAITING OPERATOR COMMIT");
        s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_AWAITING_COMMIT;
        write_campaign(s.campaign_id, s.version, s.scope, "AWAITING_COMMIT", s.boards);
        to_fsm("EvProvisioned", EvProvisioned{});   // CMP_CONFIRMING → CMP_AWAITING_COMMIT
        // NO to_gate / no auto-advance: the gate waits for CommitCampaign/Rollback.
        return;
    }

    if (++s.confirm_ticks * kPollMs >= s.confirm_budget_ms) {
        this->log().warn(std::string("campaign ") + s.campaign_id +
                         ": CONFIRMING budget exceeded (" +
                         std::to_string(provisional) + "/" +
                         std::to_string(s.boards.size()) +
                         " PROVISIONAL) — fanning Cancel");
        for (const auto& b : s.boards) ucm_cancel(b, s.campaign_id);
        to_fsm("EvFailed", EvFailed{});
        to_gate(EvFailed{});
        return;
    }
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                 kPollMs, *this, "confirm_poll");
}

// ---- The fleet-FACING control surface (the fleet's entry to THIS vehicle's
//      campaign: GS/Mender → com → CheckForCampaign; the handler runs on-vehicle).
CampaignReply VucmGate::handle_call(const CampaignRequest& req, VucmGateState& s) {
    CampaignReply reply = system_services_vucm_CampaignReply_init_zero;
    if (!s.campaign_id.empty()) {
        this->log().warn("CheckForCampaign while one is in flight — rejecting");
        reply.accepted = 0;
        reply.state = s.last_state;
        return reply;
    }
    s.campaign_id   = req.campaign_id;
    s.version       = req.version;
    s.scope         = static_cast<uint32_t>(req.scope);
    s.boards        = s.cfg_boards;
    s.confirm_ticks = 0;
    s.config_migrated = false;              // fresh campaign — no central migration yet
    if (s.boards.empty()) s.boards.push_back(s.self_board);   // single-board fallback

    this->log().info(std::string("CheckForCampaign id=") + s.campaign_id +
                     " version=" + s.version + " boards=" +
                     std::to_string(s.boards.size()));
    // L4-C.4: persist the campaign to the shared etcd BEFORE driving the FSM, so a
    // restart at any point (incl. the step-5 restart) recovers it.
    write_campaign(s.campaign_id, s.version, s.scope, "CONFIRMING", s.boards);
    to_fsm("EvDeployment", EvDeployment{});   // → CMP_PLANNING
    to_gate(EvDeployment{});
    reply.accepted = 1;
    reply.state = system_services_vucm_CampaignState_CampaignState_CMP_PLANNING;
    return reply;
}

CampaignProgress VucmGate::handle_call(const CampaignStatusReq& /*req*/, VucmGateState& s) {
    CampaignProgress p = system_services_vucm_CampaignProgress_init_zero;
    std::snprintf(p.campaign_id, sizeof(p.campaign_id), "%s", s.campaign_id.c_str());
    std::snprintf(p.version, sizeof(p.version), "%s", s.version.c_str());
    p.state = s.last_state;
    return p;
}

// ---- L4-C operator commit/rollback (step 7). Valid only while AWAITING_COMMIT.
//      CommitCampaign → fan Confirm to every board → ACTIVE. RollbackCampaign →
//      fan Cancel → all roll back. Both reject if no campaign is awaiting commit.
DecisionReply VucmGate::handle_call(const CommitRequest& req, VucmGateState& s) {
    DecisionReply reply = system_services_vucm_DecisionReply_init_zero;
    if (s.campaign_id.empty() ||
        s.last_state != system_services_vucm_CampaignState_CampaignState_CMP_AWAITING_COMMIT) {
        this->log().warn(std::string("CommitCampaign ") + req.campaign_id +
                         " — no campaign awaiting commit");
        reply.accepted = 0; reply.state = s.last_state;
        return reply;
    }
    this->log().info(std::string("operator COMMIT campaign ") + s.campaign_id);
    to_gate(EvCommit{});   // EvCommit handler fans Confirm + advances to VALIDATING
    reply.accepted = 1;
    reply.state = system_services_vucm_CampaignState_CampaignState_CMP_VALIDATING;
    return reply;
}

DecisionReply VucmGate::handle_call(const RollbackRequest& req, VucmGateState& s) {
    DecisionReply reply = system_services_vucm_DecisionReply_init_zero;
    if (s.campaign_id.empty()) {
        this->log().warn(std::string("RollbackCampaign ") + req.campaign_id +
                         " — no active campaign");
        reply.accepted = 0; reply.state = s.last_state;
        return reply;
    }
    this->log().info(std::string("operator ROLLBACK campaign ") + s.campaign_id +
                     " — fanning Cancel");
    for (const auto& b : s.boards) ucm_cancel(b, s.campaign_id);
    to_gate(EvFailed{});   // EvFailed handler advances to ROLLBACK + clears persist
    reply.accepted = 1;
    reply.state = system_services_vucm_CampaignState_CampaignState_CMP_ROLLBACK;
    return reply;
}

// ---- Lifecycle event handlers — the gate↔FSM chain. Each advances the FSM
//      (state + broadcast) and re-posts the NEXT step. The lab auto-advances the
//      PLANNING/AUTHORIZING legs (the SM/PHM go/no-go gates are observed via
//      from_sm/from_phm; a real deny would post EvBlocked / EvFailed instead).

// ── Update admission (AUTHORIZING) ──────────────────────────────────────────
// The conjunction the vehicle must satisfy BEFORE a campaign fans out:
//   SM  — machine RUNNING (a moving/updating/shutting-down vehicle blocks);
//   NM  — network ≥ min_net_state (no SW update mid-tunnel / degraded link);
//   PHM — health < DEGRADED;
//   garage window — minutes-of-day UTC, start>end spans midnight; 0/0 = open.
// Each gate is enforce-flagged (VucmConfig) — observe-only by default (lab),
// so enabling is a config/policy act, not a code change. A denial posts
// EvBlocked (→ CMP_PLANNING) and arms a re-check poll: the campaign starts by
// itself when the vehicle parks / the tunnel ends / the window opens.
namespace { constexpr uint32_t kAuthPollMs = 5000; }  // namespace

// "" when admitted; else the (log-ready) denial reason.
static std::string admission_denied(const VucmGateState& s) {
    if (s.enforce_sm && s.last_sm != 2u /*RUNNING*/)
        return "SM state " + std::to_string(s.last_sm) + " != RUNNING";
    if (s.enforce_nm && (s.last_nm == 0xFFFFFFFFu || s.last_nm < s.min_net_state
                         || s.last_nm == 3u /*DEGRADED*/))
        return "NM state " + std::to_string(s.last_nm) + " < required "
               + std::to_string(s.min_net_state);
    if (s.enforce_phm && s.last_phm >= 2u /*DEGRADED*/)
        return "PHM level " + std::to_string(s.last_phm) + " unhealthy";
    if (!in_window(s.window_start_min, s.window_end_min))
        return "outside the update window ("
               + std::to_string(s.window_start_min) + "–"
               + std::to_string(s.window_end_min) + " min UTC)";
    return "";
}

void VucmGate::handle_cast(const EvDeployment& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_PLANNING;
    to_fsm("EvPlanned", EvPlanned{});         // PLANNING → AUTHORIZING
    to_gate(EvPlanned{});
}

void VucmGate::handle_cast(const EvPlanned& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_AUTHORIZING;
    const std::string denied = admission_denied(s);
    if (!denied.empty()) {
        this->log().warn(std::string("campaign ") + s.campaign_id +
                         ": admission BLOCKED — " + denied +
                         " (re-checking every " + std::to_string(kAuthPollMs) +
                         "ms)");
        to_fsm("EvBlocked", EvBlocked{});     // AUTHORIZING → PLANNING (hold)
        s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_PLANNING;
        if (!s.authorize_pending) {
            s.authorize_pending = true;
            ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                         kAuthPollMs, *this, "authorize_poll");
        }
        return;
    }
    s.authorize_pending = false;
    to_fsm("EvAuthorized", EvAuthorized{});   // AUTHORIZING → INSTALLING
    to_gate(EvAuthorized{});
}

void VucmGate::handle_cast(const EvAuthorized& /*msg*/, VucmGateState& s) {
    // INSTALLING (step 2): V-UCM FANS OUT RequestUpdate to EACH board's UCM
    // (addressed per-board via connect_instance(machine_index)). Each board's UCM
    // resolves its role package + invokes its installer (Mender standalone), then
    // HOLDS PROVISIONAL with confirm=<budget> so it waits for the aggregate
    // Confirm. Then arm the confirm-poll → the FSM CONFIRMING barrier waits for
    // every board's marker. The fan-out is V-UCM INITIATING the install (the user
    // story), not Mender server-pull.
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_INSTALLING;
    this->log().info(std::string("campaign ") + s.campaign_id +
                     ": INSTALLING — fanning RequestUpdate to " +
                     std::to_string(s.boards.size()) + " boards");

    // ── CENTRAL config migration (V-UCM owns it — ONCE, before the fan-out) ──
    // One V-UCM + one per/etcd (both master singletons) ⇒ one migration authority.
    // Migrate the SHARED store here so the N boards install BINARIES only; per's
    // migrate-on-read serves each board its schema until the bulk pass converges.
    //
    // VERSION-DELTA GATE: a PATCH bump ships the SAME .art + SAME configs + SAME
    // schema digests — the SWP interface is unchanged — so it MUST NOT migrate.
    // Only a MINOR delta (schema evolved) migrates; a MAJOR is a fresh deploy, not
    // a migration. version_delta_migrates() encodes that; below it we ALSO honour
    // an empty migration.json (a minor that happened to change no config).
    {
        const std::string from = current_release_version(s.releases_root);
        if (!version_delta_migrates(from, s.version)) {
            this->log().info(std::string("campaign ") + s.campaign_id +
                ": " + (from.empty() ? "?" : from) + " → " + s.version +
                " is a PATCH/same-interface step — NO config migration (SWP "
                "interface unchanged)");
        } else {
            std::vector<MigrationStep> steps;
            if (!load_migration_steps(s.releases_root, s.version, &steps)) {
                this->log().warn(std::string("campaign ") + s.campaign_id +
                    ": migration manifest unreadable — FAILING campaign");
                to_fsm("EvFailed", EvFailed{});
                to_gate(EvFailed{});
                return;
            }
            if (!steps.empty()) {
                std::string err;
                if (!per_snapshot("pre-" + s.version, &err)) {
                    this->log().warn(std::string("campaign ") + s.campaign_id +
                        ": central snapshot failed (" + err + ") — FAILING campaign");
                    to_fsm("EvFailed", EvFailed{});
                    to_gate(EvFailed{});
                    return;
                }
                for (const auto& st : steps) {
                    const std::string so =
                        plugin_path(s.releases_root, s.version, st.plugin);
                    if (!per_migrate_bulk(st, so, &err)) {
                        this->log().warn(std::string("campaign ") + s.campaign_id +
                            ": central MigrateBulk failed (" + err +
                            ") — restoring + FAILING campaign");
                        std::string rerr;
                        per_restore("pre-" + s.version, &rerr);
                        to_fsm("EvFailed", EvFailed{});
                        to_gate(EvFailed{});
                        return;
                    }
                    this->log().info(std::string("campaign ") + s.campaign_id +
                        ": config migrated (central) " + st.config_type + " " +
                        st.from_digest + " → " + st.to_digest);
                }
                s.config_migrated = true;
            }
        }
    }

    // Garage-mode policy: the user/operator confirm leg is mandatory — force a
    // confirm window so every board HOLDS PROVISIONAL (CommitCampaign is the
    // confirm surface) even if the deployment declared none.
    uint32_t confirm_ms = s.confirm_budget_ms;
    if (s.require_user_confirm && confirm_ms == 0) confirm_ms = 900000;
    for (const auto& b : s.boards) {
        bool ok = ucm_request_update(b, s.version, s.campaign_id, s.scope,
                                     confirm_ms, s.bundle_base);
        this->log().info(std::string("  → board ") + b + ": RequestUpdate " +
                         (ok ? "accepted" : "UNREACHABLE/rejected"));
    }
    s.confirm_ticks = 0;
    to_fsm("EvInstalled", EvInstalled{});     // INSTALLING → CMP_CONFIRMING
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_CONFIRMING;
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                 kPollMs, *this, "confirm_poll");
}

void VucmGate::handle_cast(const EvInstalled& /*msg*/, VucmGateState& /*s*/) {
    // FSM now in CMP_CONFIRMING; the confirm-poll (handle_info) drives the rest.
}

void VucmGate::handle_cast(const EvProvisioned& /*msg*/, VucmGateState& /*s*/) {
    // FSM is now in CMP_AWAITING_COMMIT — HOLD. The gate advances only on the
    // operator's CommitCampaign (→ EvCommit) or RollbackCampaign (→ EvFailed).
}

// EvCommit — the operator committed (via CommitCampaign): fan the aggregate
// Confirm to every board's UCM (PROVISIONAL → ACTIVE) and advance to VALIDATING.
void VucmGate::handle_cast(const EvCommit& /*msg*/, VucmGateState& s) {
    this->log().info(std::string("campaign ") + s.campaign_id +
                     ": operator COMMIT — fanning Confirm to " +
                     std::to_string(s.boards.size()) + " boards");
    for (const auto& b : s.boards) ucm_confirm(b, s.campaign_id);
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_VALIDATING;
    to_fsm("EvValidated", EvValidated{});     // VALIDATING → DONE
    to_gate(EvValidated{});
}

void VucmGate::handle_cast(const EvValidated& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_DONE;
    this->log().info(std::string("campaign ") + s.campaign_id +
                     ": DONE — all boards confirmed ACTIVE");
    // L4-C.4: clear the persisted campaign (phase IDLE) so a restart doesn't resume
    // a finished campaign.
    write_campaign("", "", 0, "IDLE", {});
    s.campaign_id.clear();
    s.boards.clear();
    s.config_migrated = false;              // committed — the migration is now the new baseline
}

void VucmGate::handle_cast(const EvBlocked& /*msg*/, VucmGateState& /*s*/) {
    to_fsm("EvBlocked", EvBlocked{});         // AUTHORIZING → PLANNING (retry)
}

void VucmGate::handle_cast(const EvFailed& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_ROLLBACK;
    this->log().warn(std::string("campaign ") + s.campaign_id + ": FAILED — rolled back");
    // CENTRAL config rollback: V-UCM took the pre-<ver> snapshot before the fan-out,
    // so V-UCM restores it — ONCE, here (the boards roll their binaries back; they
    // do NOT touch the shared snapshot — that would be the N-cardinality bug again).
    if (s.config_migrated) {
        std::string err;
        if (per_restore("pre-" + s.version, &err))
            this->log().warn(std::string("campaign ") + s.campaign_id +
                ": config restored from central snapshot pre-" + s.version);
        else
            this->log().error(std::string("campaign ") + s.campaign_id +
                ": CENTRAL CONFIG RESTORE FAILED (" + err + ") — config may be "
                "AHEAD of the rolled-back boards; restore 'pre-" + s.version +
                "' manually");
        s.config_migrated = false;
    }
    write_campaign("", "", 0, "IDLE", {});   // L4-C.4: clear the persisted campaign
    to_fsm("EvFailed", EvFailed{});
    s.campaign_id.clear();
    s.boards.clear();
}

// ---- Observed foreign edges — the ADMISSION inputs. Recorded always;
//      enforcement is the VucmConfig enforce_* flags (observe-only default).
void VucmGate::handle_cast(const SmStateMsg& msg, VucmGateState& s) {
    if (s.last_sm != static_cast<uint32_t>(msg.state))
        this->log().info(std::string("SM state → ") +
                         std::to_string(static_cast<uint32_t>(msg.state)));
    s.last_sm = static_cast<uint32_t>(msg.state);
}
void VucmGate::handle_cast(const NmStatusMsg& msg, VucmGateState& s) {
    if (s.last_nm != static_cast<uint32_t>(msg.state))
        this->log().info(std::string("NM state → ") +
                         std::to_string(static_cast<uint32_t>(msg.state)));
    s.last_nm = static_cast<uint32_t>(msg.state);
}
void VucmGate::handle_cast(const PhmHealthStatus& msg, VucmGateState& s) {
    s.last_phm = msg.level;
}

}  // namespace ara::vucm
