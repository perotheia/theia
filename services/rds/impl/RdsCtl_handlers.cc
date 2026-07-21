// User handler bodies for RdsCtl.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/RdsCtl.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/RdsCtl.hh"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace ara::rds {

namespace {

// Is the iceoryx RouDi broker running? A live RouDi holds an exclusive advisory
// lock on its lock file for its whole lifetime; we probe by trying to take that
// lock non-blockingly. LOCK acquired → nobody holds it → RouDi is NOT up (we drop
// it immediately). EWOULDBLOCK → held → RouDi is up. This is a real liveness
// signal (vs the old hardcoded `true`), needs no RouDi client, and never disturbs
// a running broker. Default path matches iceoryx's fixed lock name; RDS_ROUDI_LOCK
// overrides for a non-default IPC-channel deploy.
bool roudi_running() {
    const char* env = ::getenv("RDS_ROUDI_LOCK");
    const char* path = (env && *env) ? env : "/tmp/roudi.lock";
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;                 // no lock file → never started
    bool up = false;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        ::flock(fd, LOCK_UN);                 // we got it → RouDi is NOT holding it
        up = false;
    } else {
        up = (errno == EWOULDBLOCK);          // held by RouDi → up
    }
    ::close(fd);
    return up;
}

}  // namespace


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/RdsCtl_state.hh.
void RdsCtl::init(RdsCtlState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void RdsCtl::handle_info(const char* /*info*/, RdsCtlState& /*s*/) {
}




// GetStatus — report whether the iceoryx broker is actually running. `roudi_up`
// is a REAL probe (the RouDi lock file), not the old hardcoded true. `n_streams`
// is still 0: counting registered iceoryx ports needs a RouDi introspection
// client (iox::runtime port-introspection topic) that this FC does not yet embed
// — reporting a probed liveness + an honest "not counted" beats fabricating a
// count. (Follow-up: subscribe to the RouDi introspection topic for n_streams.)
RdsStatus RdsCtl::handle_call(
        const GetRdsStatus& /*req*/,
        RdsCtlState& /*s*/) {
    RdsStatus reply{};
    reply.roudi_up  = roudi_running();
    reply.n_streams = 0;   // TODO: RouDi port-introspection subscription
    return reply;
}


}  // namespace ara::rds
