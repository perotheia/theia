// supervisor CLI entry point. Usage:
//   supervisor run <manifest.yaml> [--root-dir DIR]

#include "supervisor/runtime.h"
#include "supervisor/spec.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace {

supervisor::Supervisor* g_sup = nullptr;

void on_signal(int /*signum*/) {
    if (g_sup) g_sup->request_shutdown();
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s run <manifest.yaml> [--root-dir DIR]\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "run") != 0) {
        print_usage(argv[0]);
        return 2;
    }

    std::string manifest = argv[2];
    std::string root_dir = ".";

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--root-dir" && i + 1 < argc) {
            root_dir = argv[++i];
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    // Resolve root_dir to an absolute path so children can chdir into it.
    char* abs = realpath(root_dir.c_str(), nullptr);
    if (!abs) {
        std::fprintf(stderr, "cannot resolve root_dir %s: %s\n",
                     root_dir.c_str(), std::strerror(errno));
        return 2;
    }
    root_dir = abs;
    std::free(abs);

    try {
        auto root = supervisor::load_manifest(manifest);
        supervisor::Supervisor sup(std::move(root), root_dir);
        g_sup = &sup;

        // SIGTERM/SIGINT are delivered via signalfd to the runtime, but
        // we install a backup handler in case anything during early init
        // races signalfd setup. Once run() starts, signalfd wins.
        // (No-op since signals are blocked; left here as a documented
        // intent.)
        (void)on_signal;  // referenced for static check

        return sup.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "supervisor: %s\n", e.what());
        return 1;
    }
}
