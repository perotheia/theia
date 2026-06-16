// nft_lib — in-process nftables via libnftables (no `nft` exec).
//
// APP-OWNED. Replaces the `nft -f -` / `nft list` / `nft delete` popens in
// fw_backend with libnftables' `nft_run_cmd_from_buffer()` — the same engine the
// `nft` CLI is a thin wrapper over. fw already GENERATES valid nft script text;
// we hand that text straight to the library. Output (for list/verify) is
// captured via the ctx output buffer. No fork, structured result.
//
// One nft_ctx per call (cheap; avoids cross-call state). errno-style: returns 0
// on success, the captured stderr on failure.

#pragma once

#include <string>

#include <nftables/libnftables.h>

namespace ara::fw {
namespace nft_lib {

// Run an nft command/script `cmd` in-process. On success returns "" and (if the
// command produced output, e.g. `list ...`) fills `out` with it. On failure
// returns the error text. `capture` = collect stdout into `out` (for list/json).
inline std::string run(const std::string& cmd, std::string* out = nullptr) {
    struct nft_ctx* ctx = nft_ctx_new(NFT_CTX_DEFAULT);
    if (!ctx) return "nft_ctx_new failed";
    // Buffer both streams so nothing leaks to the process stdio.
    nft_ctx_buffer_output(ctx);
    nft_ctx_buffer_error(ctx);

    int rc = nft_run_cmd_from_buffer(ctx, cmd.c_str());

    std::string result;
    if (rc != 0) {
        const char* err = nft_ctx_get_error_buffer(ctx);
        result = err && *err ? err : "nft command failed";
    } else if (out) {
        const char* o = nft_ctx_get_output_buffer(ctx);
        *out = o ? o : "";
    }
    nft_ctx_free(ctx);
    return result;
}

}  // namespace nft_lib
}  // namespace ara::fw
