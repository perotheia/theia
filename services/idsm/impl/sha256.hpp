// sha256 — a compact, self-contained SHA-256 (no OpenSSL/exec dependency).
//
// APP-OWNED. Replaces the `sha256sum` popen in proc_detector's Cat H integrity
// check: idsm `read()`s the running ELF (/proc/<pid>/exe) and hashes it in
// process. A spot-check, NOT the crypto plane — real signature verification is
// the crypto FC's job (Verify/GetCert); this only flags a digest drift vs the
// manifest. Keeping it dependency-free (no -lcrypto) avoids coupling idsm to
// libcrypto for a hash.
//
// Standard FIPS-180-4 SHA-256. Public-domain construction.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ara::idsm {
namespace sha_detail {

struct Sha256 {
    uint32_t s[8];
    uint64_t len = 0;
    uint8_t  buf[64];
    size_t   buflen = 0;

    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    Sha256() {
        static const uint32_t init[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(s, init, sizeof(init));
    }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
            0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
            0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
            0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
            0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
            0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|
                   (uint32_t(p[i*4+2])<<8)|uint32_t(p[i*4+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e,6)^ror(e,11)^ror(e,25);
            uint32_t ch = (e&f)^(~e&g);
            uint32_t t1 = h+S1+ch+k[i]+w[i];
            uint32_t S0 = ror(a,2)^ror(a,13)^ror(a,22);
            uint32_t maj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0+maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d;
        s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
    }

    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = 64 - buflen; if (take > n) take = n;
            std::memcpy(buf + buflen, p, take);
            buflen += take; p += take; n -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) update(&zero, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - i*8));
        update(lb, 8);
        static const char* hx = "0123456789abcdef";
        std::string out; out.reserve(64);
        for (int i = 0; i < 8; ++i)
            for (int j = 3; j >= 0; --j) {
                uint8_t byte = (uint8_t)(s[i] >> (j*8));
                out += hx[byte >> 4]; out += hx[byte & 0xf];
            }
        return out;
    }
};

}  // namespace sha_detail

// Lowercase-hex SHA-256 of a file, hashed in-process via read(). "" on open
// failure (e.g. /proc/<pid>/exe of a dead pid, or no permission).
inline std::string sha256_file_inproc(const std::string& path) {
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return "";
    sha_detail::Sha256 h;
    uint8_t buf[65536];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) h.update(buf, n);
    ::fclose(f);
    return h.hex();
}

}  // namespace ara::idsm
