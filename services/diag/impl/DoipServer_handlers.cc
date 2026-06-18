// User do_* bodies for the runnable node DoipServer.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for this
// file's existence and refuses to overwrite unless `--force` is passed.
// Bodies are yours; the declarations are in lib/DoipServer.hh.
//
// A runnable owns a thread and runs do_loop() until it returns. Replace the
// cooperative skeleton below with the real worker (e.g. build + run a gRPC
// server). do_stop() must make do_loop() return.

#include "lib/DoipServer.hh"

#include <chrono>
#include <cstdio>
#include <thread>

namespace ara::diag {


// One-time setup on the worker thread, before do_loop(). Build the server,
// open sockets, etc. here.
void DoipServer::do_start() {
    std::fprintf(stderr, "[%s] runnable starting\n", kNodeName);
    // TODO: e.g. grpc::ServerBuilder b; b.AddListeningPort(...); server_ =
    //       b.BuildAndStart();
}

// The body. Runs until it returns.
void DoipServer::do_loop() {
    while (!stop_requested()) {
        // TODO: serve one quantum of work, e.g. server_->Wait(deadline).
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr, "[%s] runnable loop exiting\n", kNodeName);
}

// Release + signal do_loop() to return. stop_requested() is already set by
// the base when this runs; for a BLOCKING do_loop() (bare server->Wait()),
// also wake it here (e.g. server_->Shutdown()).
void DoipServer::do_stop() {
    std::fprintf(stderr, "[%s] runnable stopping\n", kNodeName);
    // TODO: e.g. if (server_) server_->Shutdown();
}

}  // namespace ara::diag
