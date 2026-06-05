// EtcdStore impl — the ONLY TU that includes etcd-cpp-apiv3. Wraps
// etcd::SyncClient + etcd::Watcher behind the abstract Store (etcd_store.hpp).

#include "etcd_store.hpp"

#include <etcd/SyncClient.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>

#include <memory>
#include <string>

namespace system_services_per {

namespace {

constexpr char kPrefix[] = "/theia/config/";

std::string key_for(const std::string& node) { return kPrefix + node; }

// node name from a full key (strip the prefix); "" if it doesn't match.
std::string node_from_key(const std::string& key) {
    const std::size_t n = sizeof(kPrefix) - 1;
    if (key.compare(0, n, kPrefix) != 0) return "";
    return key.substr(n);
}

// Pack digest + config bytes into one opaque etcd value: "<digest>\0<bytes>".
std::string pack(const std::string& digest, const std::string& config) {
    std::string v;
    v.reserve(digest.size() + 1 + config.size());
    v.append(digest);
    v.push_back('\0');
    v.append(config);
    return v;
}

// Split a packed value back into (digest, config). A value with no NUL is
// treated as all-config / empty-digest (defensive).
void unpack(const std::string& v, std::string& digest, std::string& config) {
    auto nul = v.find('\0');
    if (nul == std::string::npos) { digest.clear(); config = v; return; }
    digest = v.substr(0, nul);
    config = v.substr(nul + 1);
}

class EtcdStore : public Store {
public:
    explicit EtcdStore(const std::string& endpoint)
        : client_("http://" + endpoint) {}

    StoreValue get(const std::string& node) override {
        StoreValue out;
        auto resp = client_.get(key_for(node));
        if (!resp.is_ok()) return out;          // not found / error -> empty
        out.found = true;
        unpack(resp.value().as_string(), out.digest, out.config);
        out.mod_rev = resp.value().modified_index();
        return out;
    }

    int64_t put(const std::string& node, const std::string& config,
                const std::string& digest, int64_t expect_rev) override {
        const std::string k = key_for(node);
        const std::string v = pack(digest, config);
        // The resulting revision is the RESPONSE header revision (resp.index()),
        // not value().modified_index() — the latter is only populated for a get,
        // and is 0 after a put, which would look like a CAS failure.
        if (expect_rev != 0) {
            // CAS: only write if the current mod revision matches expect_rev.
            auto resp = client_.modify_if(k, v, expect_rev);
            if (!resp.is_ok()) return 0;        // CAS conflict
            return resp.index();
        }
        auto resp = client_.put(k, v);
        if (!resp.is_ok()) return 0;
        return resp.index();
    }

    void watch_prefix(std::function<void(const WatchEvent&)> cb) override {
        cb_ = std::move(cb);
        // Recursive watch over the whole config prefix. The lib delivers
        // prev_kv on each event, so we can hand prev+cur to the consumer.
        watcher_ = std::make_unique<etcd::Watcher>(
            client_, kPrefix,
            [this](etcd::Response resp) { on_watch(resp); },
            /*recursive=*/true);
    }

    bool is_watched() const override { return watcher_ != nullptr; }

private:
    void on_watch(const etcd::Response& resp) {
        if (!cb_) return;
        for (const auto& ev : resp.events()) {
            WatchEvent we;
            we.target_node = node_from_key(ev.kv().key());
            if (we.target_node.empty()) continue;
            // current value
            we.cur.found = true;
            unpack(ev.kv().as_string(), we.cur.digest, we.cur.config);
            we.cur.mod_rev = ev.kv().modified_index();
            // previous value (absent on first put)
            if (ev.has_prev_kv()) {
                we.prev.found = true;
                unpack(ev.prev_kv().as_string(), we.prev.digest, we.prev.config);
                we.prev.mod_rev = ev.prev_kv().modified_index();
            }
            cb_(we);
        }
    }

    etcd::SyncClient client_;
    std::unique_ptr<etcd::Watcher> watcher_;
    std::function<void(const WatchEvent&)> cb_;
};

}  // namespace

std::unique_ptr<Store> make_etcd_store(const std::string& endpoint) {
    try {
        return std::make_unique<EtcdStore>(endpoint);
    } catch (...) {
        return nullptr;
    }
}

}  // namespace system_services_per
