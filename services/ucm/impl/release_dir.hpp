// release_dir — the release-directory model behind the UCM agent (replaces A/B
// partitions). APP-OWNED, in-process std::filesystem (no shell), so it's atomic
// + unit-testable.
//
//   <root>/releases/<version>/   bin/ lib/ config/ migrations/ hooks/
//   <root>/current   -> releases/<version>     (the live release)
//   <root>/previous  -> releases/<prev>        (the rollback target)
//
// A FULL switch re-aims `current` atomically (rename(2) of a temp symlink), so
// the supervisor's bin/lib (symlinked through current/) re-point in one step.
// Rollback restores current→previous. A PARTIAL update swaps one FC's binary in
// the staged release + the per-FC symlink, then the supervisor restarts just it.
//
// Everything here is BEST-EFFORT + returns a bool: a failed step → the caller
// posts EvFailed → ROLLBACK. No exceptions escape (filesystem_error is caught).

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#if defined(__has_include)
#  if __has_include(<filesystem>)
#    include <filesystem>
namespace ucm_fs = std::filesystem;
#  endif
#endif

namespace ara::ucm {

struct ReleaseLayout {
    std::string root;       // UcmConfig.releases_root's PARENT (the deploy prefix)
    std::string releases;   // <root>/releases
    std::string current;    // <root>/current   symlink
    std::string previous;   // <root>/previous  symlink

    explicit ReleaseLayout(const std::string& releases_root) {
        // releases_root is ".../releases"; the prefix is its parent.
        releases = releases_root;
        ucm_fs::path rp(releases_root);
        root = rp.parent_path().string();
        current  = (ucm_fs::path(root) / "current").string();
        previous = (ucm_fs::path(root) / "previous").string();
    }

    std::string release_of(const std::string& version) const {
        return (ucm_fs::path(releases) / version).string();
    }
};

namespace release_detail {

// Atomic symlink (re)point: write a temp symlink then rename(2) over `link`.
// POSIX rename of a symlink is atomic — no window where `link` is missing.
inline bool atomic_symlink(const std::string& target, const std::string& link) {
    std::error_code ec;
    ucm_fs::path tmp = ucm_fs::path(link).string() + ".tmp";
    ucm_fs::remove(tmp, ec);                       // clear any stale temp
    ucm_fs::create_symlink(target, tmp, ec);
    if (ec) {
        std::fprintf(stderr, "[ucm] symlink %s->%s: %s\n",
                     link.c_str(), target.c_str(), ec.message().c_str());
        return false;
    }
    ucm_fs::rename(tmp, link, ec);                 // atomic replace
    if (ec) {
        std::fprintf(stderr, "[ucm] rename %s: %s\n",
                     link.c_str(), ec.message().c_str());
        ucm_fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Read a symlink's target ("" if absent / not a symlink).
inline std::string readlink_(const std::string& link) {
    std::error_code ec;
    auto t = ucm_fs::read_symlink(link, ec);
    return ec ? std::string() : t.string();
}

}  // namespace release_detail

// Does a staged release dir exist for `version`? (STAGED precondition.)
inline bool release_staged(const ReleaseLayout& l, const std::string& version) {
    std::error_code ec;
    return ucm_fs::is_directory(l.release_of(version), ec);
}

// Make the release dir skeleton (idempotent). The download/untar lands the real
// bin/lib/config under it; this just guarantees the tree exists so a demo /
// partial swap has somewhere to write.
inline bool ensure_release_skeleton(const ReleaseLayout& l,
                                    const std::string& version) {
    std::error_code ec;
    ucm_fs::path base = l.release_of(version);
    for (const char* sub : {"", "bin", "lib", "config", "migrations", "hooks"}) {
        ucm_fs::create_directories(base / sub, ec);
        if (ec) {
            std::fprintf(stderr, "[ucm] mkdir %s/%s: %s\n",
                         base.c_str(), sub, ec.message().c_str());
            return false;
        }
    }
    return true;
}

// FULL switch: point previous→(old current target), then current→releases/<ver>,
// atomically. Returns false if the new release isn't staged.
inline bool switch_full(const ReleaseLayout& l, const std::string& version) {
    using namespace release_detail;
    if (!release_staged(l, version)) {
        std::fprintf(stderr, "[ucm] switch_full: release %s not staged\n",
                     version.c_str());
        return false;
    }
    // previous := whatever current points at now (so rollback can restore it).
    std::string cur_target = readlink_(l.current);
    if (!cur_target.empty())
        atomic_symlink(cur_target, l.previous);   // best-effort
    // current := the new release (atomic).
    return atomic_symlink(l.release_of(version), l.current);
}

// Rollback: current := previous (the prior release), atomically. Returns false
// if there's no previous to roll back to.
inline bool rollback_full(const ReleaseLayout& l) {
    using namespace release_detail;
    std::string prev = readlink_(l.previous);
    if (prev.empty()) {
        std::fprintf(stderr, "[ucm] rollback: no previous release\n");
        return false;
    }
    return atomic_symlink(prev, l.current);
}

// Apply a Configuration Package (kind=UK_CONFIG): push the staged release's
// config/ into etcd via per + drop static props into the release's config/.
// REUSES the PREP-B seed path (migration/seed.py defaults → per PutConfig), so
// the SAME all-FC seeder that runs at first boot also applies a config-package
// update — per fans the change to FCs as ConfigUpdated casts (live, no restart).
// Best-effort: the package's own config/seed_defaults.json (if present) is the
// source; absent → a no-op success (a props-only package). Shells the seeder
// (the doc explicitly allows config/migration hooks to shell).
inline bool apply_config_package(const ReleaseLayout& l,
                                 const std::string& version) {
    std::error_code ec;
    ucm_fs::path rel = l.release_of(version);
    ucm_fs::path defs = rel / "config" / "seed_defaults.json";
    ucm_fs::path schema = rel / "config" / "seed_schema.json";
    if (!ucm_fs::is_regular_file(defs, ec)) {
        std::fprintf(stderr,
            "[ucm] config package v%s has no config/seed_defaults.json — "
            "props-only (nothing to push to etcd)\n", version.c_str());
        return true;   // props-only package is a valid no-op
    }
    // migration/seed.py defaults --defaults <defs> --schema <schema> PutConfig's
    // each FC's value through per. THEIA_ROOT locates the workspace/seed tool.
    std::string root_env;
    if (const char* tr = ::getenv("THEIA_ROOT")) root_env = tr;
    std::string seed_py = (root_env.empty() ? std::string(".")
                                            : root_env) + "/migration/seed.py";
    std::string cmd = "python3 " + seed_py + " defaults --defaults " +
        defs.string() + " --schema " + schema.string() + " >&2";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "[ucm] config seed failed (rc=%d): %s\n",
                     rc, cmd.c_str());
        return false;
    }
    return true;
}

// Prune releases beyond `keep`, never removing the current/previous targets.
// Best-effort (a failed unlink is logged, not fatal).
inline void prune_releases(const ReleaseLayout& l, unsigned keep) {
    using namespace release_detail;
    std::error_code ec;
    std::string cur = readlink_(l.current), prev = readlink_(l.previous);
    std::vector<ucm_fs::path> dirs;
    for (auto& e : ucm_fs::directory_iterator(l.releases, ec)) {
        if (e.is_directory(ec)) dirs.push_back(e.path());
    }
    if (dirs.size() <= keep) return;
    // Oldest-first by write time; keep the newest `keep` + never touch cur/prev.
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        return ucm_fs::last_write_time(a, e1) < ucm_fs::last_write_time(b, e2);
    });
    size_t to_remove = dirs.size() - keep;
    for (size_t i = 0; i < dirs.size() && to_remove; ++i) {
        std::string p = dirs[i].string();
        if (p == cur || p == prev) continue;
        ucm_fs::remove_all(dirs[i], ec);
        --to_remove;
    }
}

}  // namespace ara::ucm
