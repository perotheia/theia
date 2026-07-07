// User do_* bodies for the runnable node Meshtastic — the V2V mesh transport.
//
// Owns the radio thread: opens the (compile-time-selected) backend — `null`
// (no-op default) or `meshtastic` (serial protobuf stream) — and forwards every
// decoded inbound Beacon to OsiV2v over V2vBeaconStream (a PG broadcast of the
// Beacon group). Pure transport: NO estimator math here. The backend seam
// (impl/v2v/mesh_backend.hpp) hides the radio; this node only frames Beacons.

#include "lib/Meshtastic.hh"

#include "MachineInstance.hh"   // resolve_node_tipc → this node's pg watcher addr
#include "PgClient.hh"          // producer pg_watch + broadcast_members
#include "RemoteCodec.hh"       // RemoteCodec<Beacon>::fields()
#include "ParamsConfig.hh"      // get_config().node(kNodeName).str(...)
#include "lib/meshtastic_codecs.hh"    // THEIA_DECLARE_REMOTE_CODEC(Beacon) (FC-wide)
#include "packages/v2v/v2v.pb.h"    // Beacon (from the v2v package)

#include <pb_decode.h>
#include <pb_encode.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "mesh_backend.hpp"
// (Beacon comes from packages/v2v/v2v.pb.h, included above)

namespace ara::meshtastic {

namespace {

using Beacon = packages_v2v_Beacon;

// File-static runnable state (GenRunnable binds no State type; mirror DoipServer).
struct MeshState {
    ::theia::runtime::PgClient pg;
    bool      radio_up = false;
    uint32_t  self_type = 0, self_inst = 0;
    bool      pg_ready = false;
};
MeshState& S() { static MeshState s; return s; }

// Broadcast a Beacon onto the Beacon PG group (OsiV2v pg_joins it). Mirrors the
// generated broadcast_<port>() body: pg_watch (idempotent) + encode + fan out.
void broadcast_beacon(const Beacon& b) {
    if (!S().pg_ready) return;
    S().pg.watch<Beacon>(S().self_type, S().self_inst);   // OTP pg:monitor (idempotent)
    uint8_t buf[1024];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, ::theia::runtime::RemoteCodec<Beacon>::fields(), &b)) return;
    S().pg.broadcast_members<Beacon>(buf, static_cast<uint16_t>(os.bytes_written));
}

}  // namespace

// One-time setup on the worker thread: open the radio + attach the producer PG.
void Meshtastic::do_start() {
    // Resolve our own TIPC addr (the pg watcher address the supervisor casts
    // PgMembership to). Same call main.cc makes; falls back to the .art compiled.
    ::theia::runtime::resolve_node_tipc(kNodeName, kTipcType, kTipcInstance,
                                        S().self_type, S().self_inst);

    // Producer PG: attach the supervisor CALL channel so watch/broadcast resolve.
    S().pg.attach("meshtastic-tx", /*binding=*/nullptr);
    S().pg_ready = true;

    // Open the (compile-time) backend from params. `null` returns false → idle.
    auto cfg = ::theia::runtime::get_config().node(kNodeName);
    ::ara::osi::mesh::MeshParams mp;
    mp.serial  = cfg.str("serial",  "/dev/ttyUSB0");
    mp.channel = cfg.str("channel", "LongFast");
    mp.topic   = cfg.str("topic",   "v2v/beacon");
    mp.key     = cfg.str("key",     "");
    S().radio_up = ::ara::osi::mesh::backend_open(mp);

    std::fprintf(stderr, "[%s] mesh backend=%s radio=%s\n", kNodeName,
                 ::ara::osi::mesh::backend_name(), S().radio_up ? "up" : "absent");
}

// The body: pump the radio. RX a beacon frame, deserialize it, re-broadcast onto
// the Beacon PG group for OsiV2v. The HeartbeatPublisher (main.cc) beats the
// watchdog independently, so the loop just blocks on recv with a short timeout.
void Meshtastic::do_loop() {
    std::string payload;
    while (!stop_requested()) {
        if (!S().radio_up) {
            // No radio (null backend / open failed): idle. Beacons may still
            // arrive at OsiV2v via a robot-test inject over TIPC.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (::ara::osi::mesh::backend_recv(payload, /*timeout_ms=*/200)) {
            // The payload is a serialized V2V Beacon (the compact on-air form the
            // backend already converted). Decode + re-broadcast to OsiV2v.
            Beacon b = packages_v2v_Beacon_init_zero;
            auto is = pb_istream_from_buffer(
                reinterpret_cast<const pb_byte_t*>(payload.data()), payload.size());
            if (pb_decode(&is, ::theia::runtime::RemoteCodec<Beacon>::fields(), &b)) {
                broadcast_beacon(b);
            }
        }
    }
    std::fprintf(stderr, "[%s] loop exiting\n", kNodeName);
}

// Release: close the radio + shut down the PG client; unblock do_loop().
void Meshtastic::do_stop() {
    ::ara::osi::mesh::backend_close();
    if (S().pg_ready) { S().pg.shutdown(); S().pg_ready = false; }
}

}  // namespace ara::osi
