// ucm_validate — validators for the UNTRUSTED fields of a PackageManifest, and
// the argv UCM hands to the Mender CLI. Header-only, std-only (no runtime deps),
// so it is unit-testable: //services/ucm/test:test_ucm_validate.
//
// Why: `version` becomes a path component (ReleaseLayout::release_of →
// <releases>/<version>) and `artifact_path` becomes an argument of the Mender
// CLI. Both arrive over the wire (UpdateCtl.RequestUpdate).
//
// VERSION      ^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$
//              one path component: no '/', never "." or "..", no whitespace.
//
// ARTIFACT REFERENCE (what `mender install <ref>` is allowed to receive) —
// exactly two forms, everything else is rejected:
//   1. an ABSOLUTE LOCAL PATH under an allowed directory
//        - starts with '/', characters [A-Za-z0-9._+/-] only
//        - no empty, "." or ".." component (so no "//", no traversal)
//        - lexically inside one of the allowed directories. Default:
//          /opt/theia  and  /var/lib/theia ; a deployment overrides the list
//          with THEIA_UCM_ARTIFACT_DIRS (colon-separated absolute dirs).
//          The check is LEXICAL (symlinks are not resolved): the allowed
//          directories must be writable by root only.
//   2. an https:// URL
//        - "https://" + non-empty host + optional path/query
//        - characters [A-Za-z0-9._~:/?&=%+@,-] only (no whitespace, quotes,
//          backslash, ';', '|', '$', '`', '<', '>', '(', ')', '#', control chars)
//        - no ".." path component
//   Not allowed: relative paths, http://, file://, s3://, anything starting
//   with '-' (would read as a CLI option). Max length 512.
//
// The reference is passed to Mender as ONE argv element via fork/execvp — there
// is no shell anywhere on the path (mender_install.cc).

#pragma once

#include <cstdlib>
#include <string>
#include <vector>

namespace ara::ucm {

namespace validate_detail {
inline bool alnum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
inline bool in_set(char c, const char* extra) {
    if (alnum(c)) return true;
    for (const char* p = extra; *p; ++p) if (*p == c) return true;
    return false;
}
// Split on '/', and check no component is "", "." or "..". `s` has no leading '/'.
inline bool clean_components(const std::string& s, bool allow_trailing_slash) {
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        const std::string comp = s.substr(i, j - i);
        if (comp.empty()) {
            // only a single trailing '/' may produce an empty component
            if (!(allow_trailing_slash && j == s.size() && i == s.size() && i > 0))
                return false;
        } else if (comp == "." || comp == "..") {
            return false;
        }
        i = j + 1;
    }
    return true;
}
}  // namespace validate_detail

constexpr size_t kMaxVersionLen  = 64;
constexpr size_t kMaxArtifactLen = 512;

// ^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$
inline bool valid_version(const std::string& v) {
    if (v.empty() || v.size() > kMaxVersionLen) return false;
    if (!validate_detail::alnum(v[0])) return false;
    for (char c : v) if (!validate_detail::in_set(c, "._+-")) return false;
    return true;
}

// The allowed local artifact directories: THEIA_UCM_ARTIFACT_DIRS (colon list)
// or the default {/opt/theia, /var/lib/theia}. Non-absolute entries are dropped.
inline std::vector<std::string> allowed_artifact_dirs() {
    std::vector<std::string> out;
    const char* e = std::getenv("THEIA_UCM_ARTIFACT_DIRS");
    const std::string list = (e && *e) ? e : "/opt/theia:/var/lib/theia";
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(':', i);
        if (j == std::string::npos) j = list.size();
        std::string d = list.substr(i, j - i);
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        if (d.size() > 1 && d[0] == '/') out.push_back(d);   // never "/" itself
        i = j + 1;
    }
    return out;
}

// Form 1: absolute local path under one of `dirs`.
inline bool valid_local_artifact(const std::string& p,
                                 const std::vector<std::string>& dirs,
                                 std::string* why = nullptr) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (p.size() < 2 || p[0] != '/') return fail("not an absolute path");
    for (char c : p)
        if (!validate_detail::in_set(c, "._+/-"))
            return fail("character outside [A-Za-z0-9._+/-] in path");
    if (!validate_detail::clean_components(p.substr(1), false))
        return fail("empty, '.' or '..' path component");
    for (const auto& d : dirs) {
        if (p.size() > d.size() + 1 && p.compare(0, d.size(), d) == 0 &&
            p[d.size()] == '/')
            return true;
    }
    return fail("path is not under an allowed artifact directory");
}

// Form 2: https:// URL.
inline bool valid_https_artifact(const std::string& u, std::string* why = nullptr) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    static const std::string scheme = "https://";
    if (u.compare(0, scheme.size(), scheme) != 0) return fail("not an https:// URL");
    const std::string rest = u.substr(scheme.size());
    for (char c : rest)
        if (!validate_detail::in_set(c, "._~:/?&=%+@,-"))
            return fail("character outside [A-Za-z0-9._~:/?&=%+@,-] in URL");
    const size_t slash = rest.find_first_of("/?");
    const std::string host = rest.substr(0, slash);
    if (host.empty() || !validate_detail::alnum(host[0]))
        return fail("URL has no host");
    if (slash != std::string::npos && rest[slash] == '/') {
        std::string path = rest.substr(slash + 1);
        const size_t q = path.find('?');
        if (q != std::string::npos) path.resize(q);
        size_t i = 0;                       // '..' components only; '//' tolerated
        while (i <= path.size()) {
            size_t j = path.find('/', i);
            if (j == std::string::npos) j = path.size();
            if (path.compare(i, j - i, "..") == 0) return fail("'..' component in URL path");
            i = j + 1;
        }
    }
    return true;
}

// The one entry point: is `ref` an artifact reference UCM may hand to Mender?
// On false, *why holds a short reason for the log.
inline bool valid_artifact_ref(const std::string& ref,
                               const std::vector<std::string>& dirs,
                               std::string* why = nullptr) {
    if (ref.empty()) { if (why) *why = "empty artifact reference"; return false; }
    if (ref.size() > kMaxArtifactLen) { if (why) *why = "artifact reference too long"; return false; }
    if (ref[0] == '/') return valid_local_artifact(ref, dirs, why);
    if (ref.compare(0, 8, "https://") == 0) return valid_https_artifact(ref, why);
    if (why) *why = "neither an absolute local path nor an https:// URL";
    return false;
}

// The Mender CLI invocations to try, in order (the verb differs per version):
//   4.x  mender-update install <ref>
//   3.x  mender install <ref>
//   2.x  mender -install <ref>
// Each inner vector is an argv for execvp; <ref> is always exactly ONE element.
inline std::vector<std::vector<std::string>> mender_install_argvs(const std::string& ref) {
    return {
        {"mender-update", "install", ref},
        {"mender", "install", ref},
        {"mender", "-install", ref},
    };
}

}  // namespace ara::ucm
