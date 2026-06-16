#include "impl/sha256.hpp"
#include <cstdio>
int main() {
    // SHA256 of a file containing "abc" → known FIPS vector
    // ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    FILE* f = fopen("/tmp/sha_abc.txt","w"); fputs("abc",f); fclose(f);
    auto h = ara::idsm::sha256_file_inproc("/tmp/sha_abc.txt");
    printf("sha256(\"abc\") = %s\n", h.c_str());
    printf("%s\n", h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ? "MATCH ✓" : "MISMATCH ✗");
    return h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ? 0 : 1;
}
