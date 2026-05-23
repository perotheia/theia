// supervisor CLI entry point. Usage:
//   supervisor run <manifest.yaml> [--root-dir DIR]
//                                  [--etcd-endpoints host:port[,host:port...]]
//                                  [--machine-name NAME]
//
// etcd endpoints / machine name can also come from env vars:
//   THEIA_ETCD_ENDPOINTS
//   HOSTNAME (machine name fallback)
//
// CLI overrides env. Both empty → etcd publishing disabled (TIPC only).

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
        "usage: %s run <manifest.yaml>\n"
        "           [--root-dir DIR]\n"
        "           [--etcd-endpoints host:port[,host:port...]]\n"
        "           [--machine-name NAME]\n"
        "\n"
        "env:\n"
        "  THEIA_ETCD_ENDPOINTS  same as --etcd-endpoints if absent\n"
        "  HOSTNAME              machine-name fallback\n",
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
    std::string etcd_endpoints;
    std::string machine_name;

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--root-dir" && i + 1 < argc) {
            root_dir = argv[++i];
        } else if (a == "--etcd-endpoints" && i + 1 < argc) {
            etcd_endpoints = argv[++i];
        } else if (a == "--machine-name" && i + 1 < argc) {
            machine_name = argv[++i];
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    // Fall back to env if the CLI didn't set --etcd-endpoints.
    // (machine_name's env fallback to $HOSTNAME happens inside the
    // Supervisor ctor.)
    if (etcd_endpoints.empty()) {
        const char* e = std::getenv("THEIA_ETCD_ENDPOINTS");
        if (e && *e) etcd_endpoints = e;
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
        supervisor::Supervisor sup(std::move(root), root_dir,
                                    etcd_endpoints, machine_name);
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
