// Config-prefix snapshot/restore — back up + restore JUST per's config
// keyspace (/theia/config/), NOT a full etcd snapshot. So restore is a safe
// LIVE re-put with no cluster disruption (a full etcdctl restore would stop +
// replace the whole etcd data-dir, which per has no authority to do).
//
// Store-agnostic: works on the abstract Store (scan to snapshot, put to
// restore), so it covers both the etcd and in-memory backends. The file is a
// tiny self-describing length-prefixed record stream — no external dep.
//
//   file: "PERSNAP1\n" then, per record:
//     u32 node_len, node bytes,
//     u32 digest_len, digest bytes,
//     u32 config_len, config bytes
//
// All u32 little-endian. Restore re-puts each record UNCONDITIONALLY (CAS rev 0)
// — a restore is an explicit "make the store match this backup" intent.

#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "impl/etcd_store.hpp"

namespace system_services_per {

namespace snapshot_detail {

inline constexpr char kMagic[] = "PERSNAP1\n";

inline void put_u32(std::ofstream& f, uint32_t v) {
    char b[4] = {char(v & 0xff), char((v >> 8) & 0xff),
                 char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
    f.write(b, 4);
}
inline void put_bytes(std::ofstream& f, const std::string& s) {
    put_u32(f, static_cast<uint32_t>(s.size()));
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

inline bool get_u32(std::ifstream& f, uint32_t& out) {
    unsigned char b[4];
    if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
    out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
          (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}
inline bool get_bytes(std::ifstream& f, std::string& out) {
    uint32_t n;
    if (!get_u32(f, n)) return false;
    out.resize(n);
    return n == 0 || static_cast<bool>(f.read(&out[0], n));
}

}  // namespace snapshot_detail

// Write a config-prefix snapshot of `store` to `path`. Returns the record count
// on success, -1 on a file error.
inline long write_config_snapshot(Store& store, const std::string& path) {
    using namespace snapshot_detail;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return -1;
    f.write(kMagic, sizeof(kMagic) - 1);
    long n = 0;
    for (auto& [node, val] : store.scan()) {
        put_bytes(f, node);
        put_bytes(f, val.digest);
        put_bytes(f, val.config);
        ++n;
    }
    return f.good() ? n : -1;
}

// Restore a config-prefix snapshot from `path` into `store` (re-put each record
// unconditionally). Returns the restored count, or -1 on a file/format error.
inline long restore_config_snapshot(Store& store, const std::string& path) {
    using namespace snapshot_detail;
    std::ifstream f(path, std::ios::binary);
    if (!f) return -1;
    char magic[sizeof(kMagic) - 1];
    if (!f.read(magic, sizeof(magic)) ||
        std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
        return -1;   // not a per snapshot file
    }
    long n = 0;
    for (;;) {
        std::string node, digest, config;
        if (!get_bytes(f, node)) break;                 // clean EOF
        if (!get_bytes(f, digest) || !get_bytes(f, config)) return -1;  // truncated
        store.put(node, config, digest, /*expect_rev=*/0);
        ++n;
    }
    return n;
}

}  // namespace system_services_per
