// V2V mesh backend — NULL (the default; radio compiled OUT).
//
// Every call is a no-op. Selected when the build is NOT `--define mesh=meshtastic`
// (mirrors services/shwa's host backend default). The Meshtastic node treats
// backend_open()==false as "no radio" and idles its loop — the estimator still
// runs on any beacons that arrive by other means (e.g. a robot-test inject onto
// OsiV2v's beacons_in over TIPC). This keeps the OSI FC fully buildable + the V2V
// plane testable with ZERO serial/radio dependency on the CI/dev path.

#include "impl/v2v/mesh_backend.hpp"

namespace ara::osi::mesh {

bool backend_open(const MeshParams&)            { return false; }
bool backend_send(const std::string&)           { return false; }
bool backend_recv(std::string&, int /*timeout*/) { return false; }
void backend_close()                            {}
const char* backend_name()                      { return "null"; }

}  // namespace ara::osi::mesh
