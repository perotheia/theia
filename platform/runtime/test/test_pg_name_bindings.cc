// PgClient::name_bindings: the number of sockets bound to a TIPC name (what join_checked_
// uses to refuse a member name shared with a stale socket). Needs the TIPC module.
#include "PgClient.hh"

#include <cstdio>
#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

static int bind_one(uint32_t type, uint32_t inst) {
    int fd = ::socket(AF_TIPC, SOCK_RDM, 0);
    if (fd < 0) return -1;
    sockaddr_tipc a{};
    a.family = AF_TIPC; a.addrtype = TIPC_ADDR_NAME; a.scope = TIPC_CLUSTER_SCOPE;
    a.addr.name.name.type = type; a.addr.name.name.instance = inst;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) { ::close(fd); return -1; }
    return fd;
}

int main() {
    int probe = ::socket(AF_TIPC, SOCK_RDM, 0);
    if (probe < 0) { std::printf("test_pg_name_bindings: no TIPC, skipped\n"); return 0; }
    ::close(probe);
    const uint32_t type = 0x8003fff0u + (::getpid() & 0xf), inst = 7;
    int fails = 0;
    auto eq = [&](int got, int want, const char* what) {
        if (got != want) { std::printf("FAIL %s: %d (want %d)\n", what, got, want); ++fails; }
    };
    eq(theia::runtime::PgClient::name_bindings(type, inst), 0, "unbound");
    int a = bind_one(type, inst);
    eq(theia::runtime::PgClient::name_bindings(type, inst), 1, "one socket");
    int b = bind_one(type, inst);
    eq(theia::runtime::PgClient::name_bindings(type, inst), 2, "two sockets (the shared-name case)");
    ::close(b);
    eq(theia::runtime::PgClient::name_bindings(type, inst), 1, "back to one");
    ::close(a);
    std::printf(fails ? "%d FAILED\n" : "pg_name_bindings: ok\n", fails);
    return fails ? 1 : 0;
}
