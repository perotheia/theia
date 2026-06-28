// V2V transport seam — the pluggable broadcast backend.
//
// The Meshtastic node calls THIS abstract surface; the concrete backend is
// chosen at BUILD time via bazel select (--define mesh=null|meshtastic), exactly
// like services/shwa picks its host/jetson backend. The backend is TRANSPORT
// ONLY — it moves serialized beacon bytes to/from the radio. It knows nothing
// about the estimator; the node owns the Beacon<->wire conversion at its edge.
//
//   null       — every call is a no-op (radio compiled OUT). DEFAULT.
//   meshtastic — opens the serial device + speaks the Meshtastic device serial
//                protobuf stream (ToRadio/FromRadio framing). NO MQTT.

#pragma once

#include <string>

namespace ara::osi::mesh {

// Radio open parameters (from MeshtasticConfig). Transport-level only.
struct MeshParams {
    std::string serial;    // device path, e.g. /dev/ttyUSB0
    std::string channel;   // Meshtastic channel name
    std::string topic;     // → portnum / app id
    std::string key;       // channel PSK (base64); "" = default channel key
};

// Open the transport. Returns false if unavailable (null always false; the node
// treats false as "no radio" and idles — accounting/estimate still run on any
// beacons that arrive by other means).
bool backend_open(const MeshParams& p);

// TX one serialized beacon frame (the compact on-air form). Returns sent.
bool backend_send(const std::string& bytes);

// RX the next beacon frame (blocking up to timeout_ms). Returns true + fills
// `out` with the decoded-to-internal serialized Beacon bytes on success; false
// on timeout / no radio. (The meshtastic backend strips the radio envelope and
// hands back the inner V2V payload; the node deserializes it into a Beacon.)
bool backend_recv(std::string& out, int timeout_ms);

// Close the transport.
void backend_close();

// Human name of the compiled-in backend ("null" / "meshtastic") — for logs.
const char* backend_name();

}  // namespace ara::osi::mesh
