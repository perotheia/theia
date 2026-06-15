// In-memory Store — the etcd-free fallback + test default. No etcd dependency,
// so this TU lives in :per_impl (not :per_etcd). watch_prefix is a no-op: the
// in-memory path has no async change source, so the handler fans ConfigUpdated
// out directly on Put (see PerClient_handlers.cc).

#include "etcd_store.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ara::per {

namespace {

class MemStore : public Store {
public:
    StoreValue get(const std::string& node) override {
        std::lock_guard<std::mutex> lk(mu_);
        StoreValue out;
        auto it = kv_.find(node);
        if (it == kv_.end()) return out;
        out = it->second;
        out.found = true;
        return out;
    }

    int64_t put(const std::string& node, const std::string& config,
                const std::string& digest, int64_t expect_rev) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = kv_.find(node);
        const int64_t cur_rev = (it == kv_.end()) ? 0 : it->second.mod_rev;
        if (expect_rev != 0 && expect_rev != cur_rev) return 0;  // CAS conflict
        StoreValue& v = kv_[node];
        v.config = config;
        v.digest = digest;
        v.mod_rev = ++rev_;
        v.found = true;
        return v.mod_rev;
    }

    void watch_prefix(std::function<void(const WatchEvent&)>) override {
        // no-op — see file header.
    }

    bool is_watched() const override { return false; }

    std::vector<std::pair<std::string, StoreValue>> scan() override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::pair<std::string, StoreValue>> out;
        out.reserve(kv_.size());
        for (const auto& e : kv_) {
            StoreValue v = e.second;
            v.found = true;
            out.emplace_back(e.first, std::move(v));
        }
        return out;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, StoreValue> kv_;
    int64_t rev_ = 0;
};

}  // namespace

std::unique_ptr<Store> make_memory_store() {
    return std::make_unique<MemStore>();
}

// Process-shared store handle (see etcd_store.hpp). A plain atomic pointer —
// set once by PerClient::init before any MigrateBulk could run; read by
// PerManager. No ownership (PerClientState owns the unique_ptr).
namespace {
std::atomic<Store*> g_shared_store{nullptr};
}
void set_shared_store(Store* s) { g_shared_store.store(s); }
Store* shared_store() { return g_shared_store.load(); }

}  // namespace ara::per
