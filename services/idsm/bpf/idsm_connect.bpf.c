// idsm_connect.bpf.c — eBPF CO-RE sensor for IDSM Cat B (short-lived dials).
//
// Attaches the stable `sys_enter_connect` syscall tracepoint and writes one
// event per connect() — {pid, comm, daddr, dport, family} — to a ring buffer the
// userspace IdsBackend drains. Edge-triggered at the syscall boundary, so it
// catches a connection that opens and closes BETWEEN userspace polls (the one
// thing the ss/netlink poll + nft counters can't see). The userspace side
// correlates against the egress allow-list to decide if it's a violation.
//
// CO-RE: built once against vmlinux.h BTF, runs on any matching kernel.

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

#define AF_INET  2
#define AF_INET6 10

struct conn_event {
    __u32 pid;
    __u32 family;       // AF_INET / AF_INET6
    __u32 daddr4;       // IPv4 dst (network order); 0 for v6
    __u16 dport;        // dst port (host order)
    char  comm[16];
};

// Ring buffer — the userspace ring_buffer__poll() drains this.
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 16);   // 64 KiB
} events SEC(".maps");

// Tracepoint args for sys_enter_connect: after the common header,
//   long fd; struct sockaddr* uservaddr; long addrlen;
struct connect_args {
    __u64 _pad;             // common tracepoint header
    long  syscall_nr;
    __u64 fd;
    __u64 uservaddr;        // user pointer to struct sockaddr
    __u64 addrlen;
};

SEC("tracepoint/syscalls/sys_enter_connect")
int idsm_connect(struct connect_args* ctx) {
    // Peek the sockaddr family from userspace.
    __u16 family = 0;
    void* uaddr = (void*)ctx->uservaddr;
    if (!uaddr) return 0;
    bpf_probe_read_user(&family, sizeof(family), uaddr);
    if (family != AF_INET && family != AF_INET6) return 0;   // only IP dials

    struct conn_event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return 0;
    e->pid = bpf_get_current_pid_tgid() >> 32;
    e->family = family;
    e->daddr4 = 0;
    e->dport = 0;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    if (family == AF_INET) {
        // struct sockaddr_in: u16 family; u16 port(be); u32 addr(be).
        __u16 port_be = 0; __u32 addr_be = 0;
        bpf_probe_read_user(&port_be, sizeof(port_be), (char*)uaddr + 2);
        bpf_probe_read_user(&addr_be, sizeof(addr_be), (char*)uaddr + 4);
        e->dport  = (port_be >> 8) | (port_be << 8);   // be → host
        e->daddr4 = addr_be;                           // keep network order
    } else {
        // sockaddr_in6: u16 family; u16 port(be); ... (addr at +8).
        __u16 port_be = 0;
        bpf_probe_read_user(&port_be, sizeof(port_be), (char*)uaddr + 2);
        e->dport = (port_be >> 8) | (port_be << 8);
    }
    bpf_ringbuf_submit(e, 0);
    return 0;
}
