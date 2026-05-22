// crasher — demo daemon for libtombstone.
//
// Usage:
//   crasher [--mode segv|abort|div0|none] [--delay SECONDS]
//
// Installs the tombstone handler, sleeps for --delay seconds, then
// commits the chosen crime. With --mode none, just sleeps forever so
// the supervisor can SIGKILL it manually.

#include "tombstone/tombstone.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>

namespace {

void perform_segv() {
    volatile int* p = nullptr;
    *p = 42;            // boom
}

void perform_abort() {
    abort();
}

void perform_div0() {
    volatile int a = 10;
    volatile int b = 0;
    volatile int c = a / b;
    (void)c;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode  = "none";
    int         delay = 2;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
            delay = std::atoi(argv[++i]);
        }
    }

    if (!tombstone::install_handlers("crasher", "/tmp/tombstones")) {
        std::fprintf(stderr, "crasher: tombstone::install_handlers failed\n");
        return 1;
    }

    std::printf("crasher started (pid=%d, mode=%s, delay=%ds)\n",
                getpid(), mode, delay);
    std::fflush(stdout);

    sleep(delay);

    if (std::strcmp(mode, "segv") == 0)  perform_segv();
    if (std::strcmp(mode, "abort") == 0) perform_abort();
    if (std::strcmp(mode, "div0") == 0)  perform_div0();

    // --mode none: just sleep forever.
    while (true) sleep(60);
    return 0;
}
