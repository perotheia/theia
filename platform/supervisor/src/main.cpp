// supervisor CLI entry point. Usage:
//   supervisor run <manifest.yaml> [--root-dir DIR]

#include "supervisor/runtime.h"
#include "supervisor/spec.h"
#include "supervisor/tcp_publisher.h"

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
        "usage: %s run <manifest.yaml> [--root-dir DIR] [--listen-port PORT]\n",
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
    uint16_t    cli_listen_port = 0;       // 0 = use manifest, else binary default

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--root-dir" && i + 1 < argc) {
            root_dir = argv[++i];
        } else if (a == "--listen-port" && i + 1 < argc) {
            const char* p = argv[++i];
            char* end = nullptr;
            long v = std::strtol(p, &end, 10);
            if (end == p || *end != '\0' || v <= 0 || v > 65535) {
                std::fprintf(stderr, "invalid --listen-port: %s\n", p);
                return 2;
            }
            cli_listen_port = static_cast<uint16_t>(v);
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

        // Resolve listen port: CLI flag wins, else manifest, else binary default.
        uint16_t port = supervisor::TcpPublisher::kDefaultPort;
        if (root && root->is_supervisor() && root->sup.listen_port != 0) {
            port = root->sup.listen_port;
        }
        if (cli_listen_port != 0) {
            port = cli_listen_port;
        }

        supervisor::Supervisor sup(std::move(root), root_dir, port);
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
