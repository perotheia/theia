// theia::runtime::GenServer<Derived, State>
//
// C++14 take on Erlang's gen_server. Modeled directly on
// up/otp/lib/stdlib/src/gen_server.erl:
//
//   * One thread per node, owns its State. Handlers run serially —
//     no locks inside handlers.
//   * call(server, Req, act, timeout) is the SYNCHRONOUS primitive:
//     enqueues a lambda on the server, blocks on a promise the server
//     fulfills after handle_call returns. Returns a CallResult<Reply,
//     Act> the caller pattern-matches on (or runs through
//     call_and_dispatch to hit the right handle_call_result overload).
//   * cast(server, Req) is async fire-and-forget.
//   * handle_info(const char*, State&) — internal string notes posted
//     via post_info() (timer loops, self-ticks). LOCAL-ONLY: `info` is an
//     opaque in-process signal and never crosses the wire. Default is a
//     benign no-op. (Wire-info — delivering an unrouted inbound frame as
//     untyped bytes — was removed: cross-node traffic is exclusively
//     typed cast/call with a registered RemoteCodec; an unrouted frame is
//     dropped with a CRITICAL log in TipcMux, not delivered here.)
//   * handle_call_result / handle_call_error / handle_call_timeout
//     are the caller-side counterparts: when a node makes a call,
//     it writes overloads to consume the result, and the framework
//     dispatches by the CallResult tag. The opaque Act travels with
//     the request and comes back in the result so the caller can
//     route to the right continuation in its state.
//
// Type-erasure strategy: the mailbox holds std::function<void(GenServerBase*)>.
// Each enqueue (in call/cast/post_info) captures the typed payload
// in a lambda; the lambda casts the base pointer to Derived* and
// invokes the right overload via standard C++ overload resolution.
// Mailbox itself never sees the message types — they're erased at the
// boundary.

#pragma once

#include "RemoteCodec.hh"        // encode_for_trace + msg_type_name
#include "Tracer.hh"
#include "Logger.hh"             // process_logger() — config-push apply (#386)
#include "ControlPush.hh"        // shared apply_{log_level,trace_control}_push

#include "platform_runtime/runtime.pb.h"  // LogLevelPush control message

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <future>
#include <memory>
#include <type_traits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Codec for the supervisor→node control push (#386). Declared at global
// scope (the macro opens theia::runtime itself) and here — not in an
// app's _codecs.hh — because GenServer's base config handler below is
// the universal receiver: every node that binds a config service
// register_cast<>'s this type, so the codec belongs with the framework.
// Emits RemoteCodec<platform_runtime_LogLevelPush> + msg_type_name.
THEIA_DECLARE_REMOTE_CODEC(platform_runtime_LogLevelPush)
// Trace control (#403) — the supervisor's per-node trace-kind push.
// Same framework-universal codec rationale as LogLevelPush above.
THEIA_DECLARE_REMOTE_CODEC(platform_runtime_TraceControlPush)
// Config update — services/per's runtime-observable config push. Same
// framework-universal codec rationale; variable-length fields are pinned to
// fixed arrays by runtime.options so this decodes like the scalar pushes.
THEIA_DECLARE_REMOTE_CODEC(platform_runtime_ConfigUpdated)

namespace theia {
namespace runtime {

// ---- CallResult ----------------------------------------------------------

// The tagged result of call(). Matches OTP wait_response/2's
// {reply, Reply} | {error, Reason} | timeout return shape; Act echoes
// OTP's Label from send_request/4.
enum class CallTag { Reply, Error, Timeout };

template <typename Reply, typename Act>
struct CallResult {
    CallTag     tag;
    Act         act;     // echoed verbatim from call()
    Reply       reply;   // valid iff tag == Reply
    std::string error;   // valid iff tag == Error
};

// Forward decls for the async-call API. Full defs further down.
enum class CheckTag { Reply, Error, Timeout, NoReply };
template <typename Reply, typename Act> struct CheckResult;
template <typename Reply, typename Act> class  RequestId;
template <typename Reply, typename Act>
CheckResult<Reply, Act> check_response(RequestId<Reply, Act>&);
template <typename Reply, typename Act>
CallResult<Reply, Act> wait_response(RequestId<Reply, Act>&, int);
template <typename Reply, typename Act>
CallResult<Reply, Act> receive_response(RequestId<Reply, Act>&&, int);
struct NotifyHook;  // defined below
template <typename Reply, typename Server, typename Req, typename Act>
RequestId<Reply, Act> send_request(
    Server&, Req, Act, std::shared_ptr<NotifyHook> = nullptr);

// Wire-info (the InfoMsg / handle_info(const InfoMsg&) fall-through path) was
// removed: an unrouted inbound frame is no longer delivered as untyped bytes.
// TipcMux now logs a CRITICAL and drops it (see TipcMux.cc). `info` is a
// LOCAL-only opaque string (post_info → handle_info(const char*, State&));
// it never crosses the wire. Cross-node traffic is exclusively typed cast /
// call with a registered RemoteCodec.

// ---- GenServerBase -------------------------------------------------------

// The non-template base. Owns the mailbox + thread + lifecycle. The
// Derived class is reached via a virtual hook (run_handler) because
// the mailbox lambdas need to invoke methods that live on Derived,
// which the base doesn't know about.
class GenServerBase : public theia::runtime::NodeLogger {
public:
    using MailboxFn = std::function<void(GenServerBase*)>;

    GenServerBase() = default;
    virtual ~GenServerBase() {
        // Last-resort teardown. Skips terminate() — for ordered shutdown
        // call stop(server, reason) before the object goes out of scope.
        force_stop_();
    }

    GenServerBase(const GenServerBase&) = delete;
    GenServerBase& operator=(const GenServerBase&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { this->loop_(); });
    }

    // OTP-style orderly stop. Enqueues a stop sentinel that runs AFTER
    // all currently-pending mailbox items, drains, calls terminate on
    // the server's thread, then exits the loop. Returns only after
    // terminate has completed.
    //
    // `reason` is forwarded to Derived::terminate(reason, state). For
    // most cases pass "normal".
    void stop(std::string reason = "normal") {
        if (!running_.load()) return;
        auto done = std::make_shared<std::promise<void>>();
        auto done_fut = done->get_future();
        enqueue([r = std::move(reason), done](GenServerBase* base) {
            base->terminate_reason_ = r;
            base->running_.store(false);
            // Done set in the loop AFTER terminate_() runs so the
            // caller's stop() returns only post-terminate.
            base->pending_stop_done_ = done;
        });
        // Block until the loop signals (post-terminate).
        done_fut.wait();
        if (thread_.joinable()) thread_.join();
    }

    // Enqueue a typed lambda. The lambda knows the message type at
    // capture time; we hold it type-erased.
    void enqueue(MailboxFn fn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            mailbox_.push_back(Entry{0, std::move(fn)});
        }
        cv_.notify_one();
    }

    // CONFLATING enqueue (keep-latest) for a periodic STATE-LIKE feed. `key`
    // is the message-type demux id (service_id / djb2("Msg")); a NON-ZERO key
    // whose prior cast is still queued-but-unhandled is OVERWRITTEN IN PLACE
    // instead of appended — so a consumer slower than the producer sees only
    // the NEWEST pending value, not the whole stale backlog (docs/tasks
    // genserver-conflating-mailbox). key==0 is the non-conflated path (use
    // enqueue()). Calls are NEVER conflated — only fire-and-forget casts.
    // FIFO position is preserved: an overwrite keeps the original slot's place
    // in line (a feed that was already ahead of a later call stays ahead), we
    // only swap its payload for the fresher one.
    void enqueue_conflated(uint16_t key, MailboxFn fn) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = conflate_slots_.find(key);
            if (it != conflate_slots_.end()) {
                // A pending cast of this type is still queued — replace its
                // payload with the fresher one (keep-latest), same position.
                it->second->fn = std::move(fn);
                ++conflated_drops_;   // observability: a stale value was dropped
                dropped = true;
                // Fall through to emit the trace event OUTSIDE the lock — trace
                // submit does TIPC I/O, which must never run under mu_ (it would
                // serialize the producer against the consumer thread).
            } else {
                mailbox_.push_back(Entry{key, std::move(fn)});
                conflate_slots_[key] = std::prev(mailbox_.end());
            }
        }
        if (dropped) {
            emit_conflate_(key);      // observability: a stale keep-latest drop
            return;                   // no notify — the slot was already pending
        }
        cv_.notify_one();
    }

    // Count of stale casts dropped by conflation (for a trace/diagnostic).
    uint64_t conflated_drops() const {
        std::lock_guard<std::mutex> lk(mu_);
        return conflated_drops_;
    }

    // Declare a msg-type (by service_id key) conflatable on this node. Called
    // once at wiring time by register_cast(conflate=true); makes the LOCAL
    // same-process cast() path keep-latest for that type, matching the mux path.
    void mark_conflated(uint16_t key) {
        std::lock_guard<std::mutex> lk(mu_);
        if (key) conflate_types_.insert(key);
    }

    // The node's kNodeName, threaded in at wiring time (register_cast conflate=true)
    // so enqueue_conflated can emit a Conflate Tracer event on a keep-latest drop —
    // GenServerBase is non-templated, so it can't reach Derived::kNodeName itself.
    // Empty until set (an un-wired node never conflates, so never needs it).
    void set_trace_name(const char* name) noexcept {
        if (name && !trace_name_) trace_name_ = name;   // set-once, no lock churn
    }
    // Is this msg-type (service_id key) conflatable on this node? Read by the
    // local cast() to choose enqueue_conflated() over enqueue().
    bool is_conflated(uint16_t key) const {
        if (!key) return false;
        std::lock_guard<std::mutex> lk(mu_);
        return conflate_types_.count(key) != 0;
    }

    // Public forwarder used by post_info() lambdas. The actual typed
    // dispatch lives on Derived; this thunks through the virtual
    // dispatch_info_ override so the right Derived::handle_info(...)
    // runs.
    void dispatch_info(const char* info) { dispatch_info_(info); }

    // The node thread's pthread handle, for per-node CPU affinity / scheduler
    // application by main.cc after start() (THEIA_NODE_CFG). 0 before start().
    std::thread::native_handle_type native_handle() {
        return thread_.joinable() ? thread_.native_handle()
                                  : std::thread::native_handle_type{};
    }

    // ---- this node's OWN resolved TIPC instance -----------------------------
    // The instance main() resolved for THIS node (resolve_node_tipc → the
    // machine-shifted or clone instance). Handler code reads it so a node whose
    // node type is CLONED (N instances at the same TIPC type, round-robined) can
    // key its OWN per-instance config as "<component>/<instance>" and pass its
    // instance to per (WatchConfigReq.subscriber_instance) — so per notifies THIS
    // clone, not instance 0. 0 (the default) for the single-instance common case.
    // set_tipc_instance() is called ONCE by the generated main after
    // resolve_node_tipc + before start(); it's a plain store (no concurrency —
    // the node thread isn't running yet).
    void     set_tipc_instance(uint32_t inst) { tipc_instance_ = inst; }
    uint32_t tipc_instance() const { return tipc_instance_; }

protected:
    // Override in Derived (via GenServer<Derived, State>) to forward
    // a const-char* "info" message to the typed handle_info(...) on
    // Derived. The mailbox is type-erased so it can't do this directly.
    virtual void dispatch_info_(const char* info) = 0;

    // Override in Derived to call Derived::terminate(reason, state).
    // Default no-op so apps that don't need cleanup can skip it.
    virtual void dispatch_terminate_(const char* /*reason*/) {}

    // Override in Derived to call Derived::init(state). Runs ONCE on the
    // node's own thread before any mailbox item is handled — the OTP
    // gen_server:init/1 callback. Default no-op; the GenServer<Derived,
    // State> override forwards to Derived::init(state_) (which gen-app
    // emits — empty by default — into every node's impl).
    virtual void dispatch_init_() {}

private:
    void loop_() {
        // OTP init/1: post-construction startup hook, on the node thread,
        // before the first message. A node bootstraps its work loop here
        // (e.g. post_info(*this, ...)) — the items it enqueues land in the
        // mailbox below in order.
        dispatch_init_();
        while (true) {
            MailboxFn fn;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] {
                    return !running_.load() || !mailbox_.empty();
                });
                if (!running_.load() && mailbox_.empty()) break;
                Entry& front = mailbox_.front();
                // Drop this slot's conflation reservation BEFORE we dispatch:
                // once it leaves the queue a same-key cast must append fresh,
                // not overwrite an entry we're about to hand to the handler.
                if (front.conflate_key != 0)
                    conflate_slots_.erase(front.conflate_key);
                fn = std::move(front.fn);
                mailbox_.pop_front();
            }
            fn(this);  // dispatches into Derived via the captured lambda
        }
        // Orderly teardown: terminate() runs on the server's own
        // thread, after the mailbox is drained.
        dispatch_terminate_(terminate_reason_.c_str());
        if (pending_stop_done_) pending_stop_done_->set_value();
    }

    void force_stop_() {
        if (!running_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cv_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }

    // One queued mailbox item. `conflate_key` is 0 for a normal FIFO item;
    // a non-zero key (the msg-type service_id) marks a conflatable cast whose
    // slot enqueue_conflated() overwrites in place while it's still pending.
    struct Entry {
        uint16_t  conflate_key;
        MailboxFn fn;
    };

    std::atomic<bool>      running_{false};
    std::thread            thread_;
    mutable std::mutex     mu_;               // mutable: const conflated_drops()
    std::condition_variable cv_;
    // std::list (not deque) so a conflatable slot's iterator stays valid across
    // other enqueues/pops — enqueue_conflated() overwrites via a stored iterator.
    std::list<Entry>       mailbox_;
    // key → the pending (queued-but-unhandled) slot for that conflatable type.
    std::unordered_map<uint16_t, std::list<Entry>::iterator> conflate_slots_;
    uint64_t               conflated_drops_{0};   // stale casts dropped by conflation
    // service_ids (djb2 of the msg type) declared conflatable on this node via a
    // `[conflate]` receiver port — register_cast(conflate=true) records the key
    // here so the LOCAL same-process cast() path can keep-latest too, not just
    // the TIPC/mux path. Populated once at wiring time, read on every cast.
    std::unordered_set<uint16_t> conflate_types_;
    // This node's kNodeName (set-once at wiring time via set_trace_name), so
    // enqueue_conflated can look up the node's Tracer to emit a Conflate event on
    // a keep-latest drop. nullptr on an un-wired node (never conflates). const
    // char* to a static kNodeName string — no ownership, no lock.
    const char*                          trace_name_{nullptr};
    // Emit a Conflate Tracer event for a keep-latest drop of service_id `key`.
    // Called from enqueue_conflated OUTSIDE mu_ (trace submit does TIPC I/O). The
    // Tracer's own enabled/kind/reporting filters gate the actual submit, so this
    // is cheap (a name lookup + a branch) when tracing is off — the common case.
    void emit_conflate_(uint16_t key) noexcept {
        if (!trace_name_) return;
        auto& tr = ::theia::runtime::tracer_for(trace_name_);
        if (!tr.enabled()) return;
        // The dropped msg's type-name isn't stored per-key; label the event by the
        // conflate service_id so the observer can correlate it to the [conflate]
        // port. corr_id carries the key too (numeric, for machine filtering).
        char label[24];
        std::snprintf(label, sizeof(label), "conflate:0x%04x", key);
        tr.emit(::theia::runtime::TraceEvent::Conflate, label,
                /*corr_id=*/key, /*payload=*/nullptr, /*payload_len=*/0);
    }
    // Touched only on the server thread (in loop_'s post-drain phase
    // and the stop-sentinel lambda) — no synchronization needed.
    std::string                          terminate_reason_{"normal"};
    std::shared_ptr<std::promise<void>>  pending_stop_done_;
    // This node's own resolved TIPC instance (set once by main pre-start).
    uint32_t                             tipc_instance_ = 0;
};

// ---- GenServer<Derived, State> ------------------------------------------

template <typename Derived, typename StateT>
class GenServer : public GenServerBase {
public:
    using State = StateT;

    GenServer() { mark_reporting_(); }
    explicit GenServer(State initial) : state_(std::move(initial)) {
        mark_reporting_();
    }

    State& state() { return state_; }
    const State& state() const { return state_; }

    // Default terminate is a no-op. Derived may shadow it with
    //   void terminate(const char* reason, State& state) noexcept;
    // — overload resolution picks Derived's if present, the framework's
    // default otherwise. Same trick we use for handle_info default.
    void terminate(const char* /*reason*/, StateT& /*s*/) noexcept {}

    // Default init is a no-op (OTP init/1). Derived shadows it with
    //   void init(State& state);
    // gen-app emits an init() body into every node's impl (empty by
    // default, the bootstrap body for self-driving nodes). The default
    // here keeps EXISTING (un-regenerated) FCs compiling against the new
    // runtime — name-hiding picks Derived's init() once it's emitted.
    void init(StateT& /*s*/) {}

    // ---- handle_info defaults -------------------------------------------
    //
    // String clause — internal post_info() notes. Benign no-op default;
    // a deliberate in-process signal is not an error. Derived may shadow
    // with void handle_info(const char*, State&).
    void handle_info(const char* /*info*/, StateT& /*s*/) noexcept {}

    // ---- Config service: LogLevelPush (#386) ----------------------------
    //
    // Framework-provided handle_cast overload for the supervisor's
    // runtime log-level push. A reporting node's main.cc activates this
    // by register_cast<platform_runtime_LogLevelPush>(binding, node) on
    // its config-service binding; the cast then lands here on the node
    // thread (no per-FC code). The enum is ordinal-aligned with
    // LogLevel, so applying it is a static_cast + set_level on the
    // process-wide logger. Lives on the base — not the generated daemon
    // — so every node gets identical, correct behaviour; the app only
    // flips the receiver on (the `reporting` conditional in the
    // template), it never implements the handler.
    void handle_cast(const platform_runtime_LogLevelPush& push,
                     StateT& /*s*/) noexcept {
        // Shared with GenRunnable's inline handler — one source of truth.
        ::theia::runtime::apply_log_level_push(this->log(), push);
    }

    // ---- Config service: TraceControlPush (#403) ------------------------
    //
    // Framework-provided handle_cast overload for the supervisor's trace
    // control push (rf → com ConfigureTrace → supervisor → here). Same
    // shape + wiring as LogLevelPush: a reporting node's main.cc
    // register_cast<platform_runtime_TraceControlPush>'s it, the cast
    // lands here on the node thread, and we flip the node's Tracer kind
    // filter. The runtime TraceKind ordinals (TK_*) are aligned with
    // theia::runtime::TraceKind, so it's a static_cast. enabled toggles the
    // kind bit; an empty kind mask means "all kinds" (master on).
    void handle_cast(const platform_runtime_TraceControlPush& push,
                     StateT& /*s*/) noexcept {
        // Shared with GenRunnable's inline handler — one source of truth.
        ::theia::runtime::apply_trace_control_push(
            ::theia::runtime::tracer_for(Derived::kNodeName), this->log(), push);
    }

    // ---- Config service: ConfigUpdated (services/per) -------------------
    //
    // The THIRD framework config-service cast (sibling of LogLevelPush /
    // TraceControlPush). services/per casts this to a node when its watched
    // config changes; a node's main.cc activates it by
    // register_cast<platform_runtime_ConfigUpdated>(binding, node) on the
    // config-service binding. Unlike trace/log, the PAYLOAD is node-specific —
    // `push.config` is the serialized <Node>Config the node must unpack with
    // its OWN codec — so the base can't decode it. The framework therefore
    // does the GENERIC part (stash the raw bytes + digest + changed mask so the
    // node can read them, and emit a uniform log line) and hands off to a
    // Derived-overridable hook:
    //
    //   void on_config_update(const platform_runtime_ConfigUpdated& cfg,
    //                         State& state);
    //
    // The default hook is a no-op (name-hiding picks Derived's once it defines
    // one — same trick as init()/handle_info). A node that wants live config
    // overrides on_config_update, ParseFromString/nanopb-decodes cfg.config
    // into its config type, swaps its held shared_ptr<const Config>, and honors
    // the per-field STATIC/HOT_RELOAD/RESTART_REQUIRED class. A node that
    // doesn't override still compiles + ignores the push (e.g. config it only
    // reads at boot via get_config()).
    void handle_cast(const platform_runtime_ConfigUpdated& push,
                     StateT& s) noexcept {
        const std::size_t clen = push.config.size;
        this->log().info(std::string("config update: ") +
                         std::to_string(clen) + "B digest=" +
                         (push.digest[0] ? push.digest : "(none)") +
                         " changed=" + std::to_string(push.changed_count) +
                         " (services/per push)");
        // Hand the typed snapshot to the node. Default below is a no-op; a
        // Derived on_config_update shadows it (overload resolution / name
        // hiding picks Derived's). Exceptions in the app hook must not kill the
        // node thread — the cast path is noexcept.
        try {
            static_cast<Derived*>(this)->on_config_update(push, s);
        } catch (...) {
            this->log().warn("on_config_update threw — config not applied");
        }
    }

    // Default config hook — no-op (a node that only reads config at boot, or
    // doesn't use services/per, ignores live pushes). Derived shadows it.
    void on_config_update(const platform_runtime_ConfigUpdated& /*cfg*/,
                          StateT& /*s*/) noexcept {}

protected:
    void dispatch_info_(const char* info) override {
        auto& tr = ::theia::runtime::tracer_for(Derived::kNodeName);
        if (tr.enabled()) {
            tr.emit(::theia::runtime::TraceEvent::Info, info,
                    /*corr_id=*/0, nullptr, 0);
        }
        static_cast<Derived*>(this)->handle_info(info, state_);
    }
    void dispatch_terminate_(const char* reason) override {
        auto& tr = ::theia::runtime::tracer_for(Derived::kNodeName);
        if (tr.enabled()) {
            tr.emit(::theia::runtime::TraceEvent::Terminate,
                    reason, /*corr_id=*/0, nullptr, 0);
        }
        static_cast<Derived*>(this)->terminate(reason, state_);
    }

    // OTP init/1: forward to Derived::init(state) on the node thread,
    // once, before the first message (called from GenServerBase::loop_).
    // gen-app emits Derived::init(State&) into every node's impl (empty
    // by default), so this resolves to a real method.
    //
    // Guarded with `if constexpr`: a GenStateM-derived node's `init` has
    // a DIFFERENT signature (init(DataT&) -> StateT, the FSM initial
    // state) and is run from start_statem(), not here — GenStateM also
    // overrides dispatch_init_ to no-op, but this base body is still
    // instantiated (it's a virtual), so it must stay well-formed when
    // Derived::init(State&) isn't callable.
    void dispatch_init_() override {
        // Detect a callable `derived.init(StateT&)` via a SFINAE call
        // expression (robust to overloaded init names, unlike taking
        // &Derived::init). False for GenStateM's init(DataT&) -> StateT.
        if constexpr (_has_state_init<Derived>(0)) {
            static_cast<Derived*>(this)->init(state_);
        }
    }

    template <typename D>
    static constexpr auto _has_state_init(int)
        -> decltype(std::declval<D&>().init(std::declval<StateT&>()), true) {
        return true;
    }
    template <typename D>
    static constexpr bool _has_state_init(...) { return false; }

    // Reporting gate (#401): tell this node-type's Tracer whether it's a
    // reporting node, so emit() only submits to the collector bus for
    // reporting nodes (parallels the reporting-gated NodeTraceCtl config
    // receiver). Runs from the ctor body — Derived is complete at
    // instantiation. gen-app daemons all declare `kReporting`; other
    // GenServer users (test fixtures, ad-hoc nodes) may not, so detect
    // it and default to false (safe: no bus traffic from an unmarked
    // node). Idempotent — the per-node-type Tracer is a singleton.
    template <typename D>
    static auto reporting_of_(int) -> decltype(D::kReporting, bool{}) {
        return D::kReporting;
    }
    template <typename D>
    static bool reporting_of_(...) { return false; }
    void mark_reporting_() {
        ::theia::runtime::tracer_for(Derived::kNodeName)
            .set_reporting(reporting_of_<Derived>(0));
    }

    State state_{};
};

// ---- Free functions: call / cast / post_info ---------------------------

// cast — async, no reply. Caller returns immediately. The server's
// handle_cast(msg, state) is invoked on its own thread.
//
// Trace points (all gated by runtime Tracer::enabled):
//   - producer side: Send       (just before enqueue)
//   - node thread:   Recv       (lambda picked up from mailbox)
//                    Dispatch   (about to call handle_cast)
//                    DispatchDone (handle_cast returned)
//
// Trace payload for Send/Recv: the message encoded via encode_for_trace.
// pb_encode is only run when the tracer is enabled.
template <typename Server, typename Msg>
void cast(Server& server, Msg msg) {
    auto& tr = ::theia::runtime::tracer_for(Server::kNodeName);
    // Synthetic correlation id pairs Send (producer side) with Recv/
    // Dispatch/DispatchDone (consumer side) for this one cast. The
    // lambda captures `corr` so the consumer-side events use the same id.
    uint32_t corr = tr.enabled()
        ? ::theia::runtime::next_trace_corr_id() : 0;
    if (tr.enabled()) {
        uint8_t scratch[256];
        uint16_t n = ::theia::runtime::encode_for_trace(msg, scratch,
            static_cast<uint16_t>(sizeof(scratch)));
        tr.emit(::theia::runtime::TraceEvent::Send,
                ::theia::runtime::msg_type_name<Msg>(), corr, scratch, n);
    }
    auto fn = [m = std::move(msg), corr](GenServerBase* base) {
        auto* self = static_cast<Server*>(base);
        auto& tr2 = ::theia::runtime::tracer_for(Server::kNodeName);
        if (tr2.enabled()) {
            uint8_t scratch[256];
            uint16_t n = ::theia::runtime::encode_for_trace(m, scratch,
                static_cast<uint16_t>(sizeof(scratch)));
            tr2.emit(::theia::runtime::TraceEvent::Recv,
                     ::theia::runtime::msg_type_name<Msg>(),
                     corr, scratch, n);
            tr2.emit(::theia::runtime::TraceEvent::Dispatch,
                     ::theia::runtime::msg_type_name<Msg>(),
                     corr, nullptr, 0);
        }
        self->handle_cast(m, self->state());
        if (tr2.enabled()) {
            tr2.emit(::theia::runtime::TraceEvent::DispatchDone,
                     ::theia::runtime::msg_type_name<Msg>(),
                     corr, nullptr, 0);
        }
    };
    // Keep-latest for a [conflate] type on the LOCAL path too: if this msg-type's
    // service_id was marked conflatable on the node (register_cast conflate=true),
    // route through enqueue_conflated so a same-process periodic feed keeps only
    // the newest pending cast — matching the mux/TIPC path. Non-conflated types
    // (the default) take the plain FIFO enqueue. The service_id key only exists
    // for a type with a RemoteCodec (the ones that can cross a wire / be a
    // receiver port); a codec-less local-only type is never conflatable, so guard
    // the key lookup with the SFINAE detector to keep cast<T> compiling for both.
    if (::theia::runtime::has_remote_codec_<Msg>::value) {
        const uint16_t key = ::theia::runtime::codec_service_id<Msg>();
        if (key && server.is_conflated(key)) {
            server.enqueue_conflated(key, std::move(fn));
            return;
        }
    }
    server.enqueue(std::move(fn));
}

// ---- Async-call API (mirrors OTP send_request / wait_response /
//      receive_response / check_response).
//
// send_request is NOT a cast — server runs handle_call. The caller
// just doesn't block. Returns a typed RequestId; the caller picks
// when (and how) to collect the reply.

// Move-only typed handle for one outstanding async call. Holds the
// std::future the server's handle_call lambda will fulfill, plus the
// ACT the caller attached. Acts like OTP's request_id() — opaque-ish,
// passed back to wait_response / receive_response / check_response.
template <typename Reply, typename Act>
class RequestId {
public:
    RequestId() = default;
    RequestId(RequestId&&) noexcept = default;
    RequestId& operator=(RequestId&&) noexcept = default;
    RequestId(const RequestId&) = delete;
    RequestId& operator=(const RequestId&) = delete;

    bool valid() const noexcept { return future_.valid(); }
    const Act& act() const noexcept { return act_; }

    // Public so the RemoteRef-aware overload in NodeRef.hh can bridge
    // from a std::future<Reply> + Act into a RequestId. The local
    // send_request<Reply>(Server&, Req, Act) overload below remains a
    // friend so it keeps using this constructor too.
    RequestId(std::future<Reply> fut, Act act)
        : future_(std::move(fut)), act_(std::move(act)) {}

private:
    template <typename R, typename Server, typename Req, typename A>
    friend RequestId<R, A> send_request(
        Server&, Req, A, std::shared_ptr<NotifyHook>);

    template <typename R, typename A>
    friend CallResult<R, A> wait_response(RequestId<R, A>&, int);
    template <typename R, typename A>
    friend CallResult<R, A> receive_response(RequestId<R, A>&&, int);
    template <typename R, typename A>
    friend CheckResult<R, A> check_response(RequestId<R, A>&);
    template <typename R, typename A>
    friend class RequestIdCollection;

    std::future<Reply> future_;
    Act                act_{};
};

// ---- NotifyHook: shared rendezvous between many in-flight requests
//                  and a single waiter (a RequestIdCollection).
//
// Each request's dispatch lambda, on completion, increments
// `ready_count` under `mu` and notifies `cv`. The collection's
// wait_one() blocks on the cv with a predicate (`ready_count > 0`).
// Reactive vs. the previous poll-with-sleep design.

struct NotifyHook {
    std::mutex                mu;
    std::condition_variable   cv;
    std::atomic<size_t>       ready_count{0};
};

// send_request — async send. Enqueues a lambda on the server that
// will run handle_call when its thread picks it up; returns immediately
// with a RequestId tying together the future + the caller's ACT.
//
// Optional `hook` (default null) makes this request reactive: the
// dispatch lambda, after fulfilling its promise, will notify the hook
// so a RequestIdCollection can wake without polling. Public callers
// pass nullptr (or rely on the default); RequestIdCollection's own
// send_request<Reply>() passes its hook in.
template <typename Reply, typename Server, typename Req, typename Act>
RequestId<Reply, Act> send_request(
    Server& server, Req req, Act act,
    std::shared_ptr<NotifyHook> hook) {
    auto& tr = ::theia::runtime::tracer_for(Server::kNodeName);
    uint32_t corr = tr.enabled()
        ? ::theia::runtime::next_trace_corr_id() : 0;
    if (tr.enabled()) {
        uint8_t scratch[256];
        uint16_t n = ::theia::runtime::encode_for_trace(
            req, scratch, static_cast<uint16_t>(sizeof(scratch)));
        tr.emit(::theia::runtime::TraceEvent::Send,
                ::theia::runtime::msg_type_name<Req>(), corr, scratch, n);
    }
    auto promise = std::make_shared<std::promise<Reply>>();
    auto future  = promise->get_future();
    server.enqueue([req = std::move(req), promise, corr, hook](
                        GenServerBase* base) {
        try {
            auto* self = static_cast<Server*>(base);
            auto& tr2 = ::theia::runtime::tracer_for(Server::kNodeName);
            if (tr2.enabled()) {
                uint8_t scratch[256];
                uint16_t n = ::theia::runtime::encode_for_trace(
                    req, scratch,
                    static_cast<uint16_t>(sizeof(scratch)));
                tr2.emit(::theia::runtime::TraceEvent::Recv,
                         ::theia::runtime::msg_type_name<Req>(),
                         corr, scratch, n);
                tr2.emit(::theia::runtime::TraceEvent::Dispatch,
                         ::theia::runtime::msg_type_name<Req>(),
                         corr, nullptr, 0);
            }
            Reply r = self->handle_call(req, self->state());
            if (tr2.enabled()) {
                tr2.emit(::theia::runtime::TraceEvent::DispatchDone,
                         ::theia::runtime::msg_type_name<Req>(),
                         corr, nullptr, 0);
            }
            promise->set_value(std::move(r));
            // Reactive wake-up for the collection that owns this hook.
            // shared_ptr's lifetime keeps the cv alive even if the
            // collection is destroyed mid-flight — the notify is a
            // no-op then.
            if (hook) {
                {
                    std::lock_guard<std::mutex> lk(hook->mu);
                    hook->ready_count.fetch_add(1, std::memory_order_release);
                }
                hook->cv.notify_one();
            }
        } catch (...) {
            promise->set_exception(std::current_exception());
            // Same notify on the exception path — the collection
            // needs to wake to surface the error, not hang.
            if (hook) {
                {
                    std::lock_guard<std::mutex> lk(hook->mu);
                    hook->ready_count.fetch_add(1, std::memory_order_release);
                }
                hook->cv.notify_one();
            }
        }
    });
    return RequestId<Reply, Act>(std::move(future), std::move(act));
}

// Helper that consumes a ready future and packages it as CallResult.
// Used by wait_response / receive_response after they've established
// readiness via wait_for(...).
template <typename Reply, typename Act>
static CallResult<Reply, Act> consume_ready_(std::future<Reply>& fut,
                                              Act act) {
    try {
        Reply r = fut.get();
        return CallResult<Reply, Act>{
            CallTag::Reply, std::move(act), std::move(r), {}};
    } catch (const std::exception& e) {
        return CallResult<Reply, Act>{
            CallTag::Error, std::move(act), Reply{}, e.what()};
    } catch (...) {
        return CallResult<Reply, Act>{
            CallTag::Error, std::move(act), Reply{}, "unknown exception"};
    }
}

// wait_response — block up to timeout for the reply. Does NOT abandon
// on timeout: `rid` stays valid and you can call wait_response again
// later. Mirrors OTP gen_server:wait_response/2.
template <typename Reply, typename Act>
CallResult<Reply, Act> wait_response(RequestId<Reply, Act>& rid,
                                      int timeout_ms) {
    if (!rid.future_.valid()) {
        return CallResult<Reply, Act>{
            CallTag::Error, rid.act_, Reply{}, "invalid request id"};
    }
    auto status = rid.future_.wait_for(
        std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
        // Don't consume — caller may retry. Echo the act so the caller
        // can still route on the timeout path if it chooses.
        return CallResult<Reply, Act>{
            CallTag::Timeout, rid.act_, Reply{}, {}};
    }
    return consume_ready_(rid.future_, rid.act_);
}

// receive_response — block up to timeout, ABANDON on timeout. Takes
// the RequestId by rvalue: it's consumed regardless of outcome. Late
// replies arriving after the timeout fall on a shared_ptr<promise>
// whose future side has been destroyed — set_value() just discards
// them harmlessly. Mirrors OTP gen_server:receive_response/2.
template <typename Reply, typename Act>
CallResult<Reply, Act> receive_response(RequestId<Reply, Act>&& rid,
                                         int timeout_ms) {
    if (!rid.future_.valid()) {
        return CallResult<Reply, Act>{
            CallTag::Error, std::move(rid.act_), Reply{},
            "invalid request id"};
    }
    auto status = rid.future_.wait_for(
        std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
        // Move out the act before letting rid destruct.
        return CallResult<Reply, Act>{
            CallTag::Timeout, std::move(rid.act_), Reply{}, {}};
    }
    return consume_ready_(rid.future_, std::move(rid.act_));
}

// check_response — non-blocking poll. NoReply if not yet ready.
// Doesn't consume the request on NoReply (caller polls again later);
// does consume on Reply/Error. Useful from inside handle_info to
// multiplex many outstanding async calls.

template <typename Reply, typename Act>
struct CheckResult {
    CheckTag    tag;
    Act         act;
    Reply       reply;
    std::string error;
};

template <typename Reply, typename Act>
CheckResult<Reply, Act> check_response(RequestId<Reply, Act>& rid) {
    if (!rid.future_.valid()) {
        return CheckResult<Reply, Act>{
            CheckTag::Error, rid.act_, Reply{}, "invalid request id"};
    }
    auto status = rid.future_.wait_for(std::chrono::milliseconds(0));
    if (status != std::future_status::ready) {
        return CheckResult<Reply, Act>{
            CheckTag::NoReply, rid.act_, Reply{}, {}};
    }
    try {
        Reply r = rid.future_.get();
        return CheckResult<Reply, Act>{
            CheckTag::Reply, rid.act_, std::move(r), {}};
    } catch (const std::exception& e) {
        return CheckResult<Reply, Act>{
            CheckTag::Error, rid.act_, Reply{}, e.what()};
    }
}

// ---- RequestIdCollection -------------------------------------------------
//
// Mirrors OTP's request_id_collection. The point of the abstraction:
// you fire N parallel send_requests, then collect their replies via
// wait_one() in arrival order, regardless of which finished first.
//
// Implementation: linear poll of all outstanding requests with a
// per-iteration sleep. Fine for handfuls of in-flight calls; for
// large fan-out we'd swap in a condition_variable that the send_request
// promise fulfillment notifies. Not needed for the demo.

template <typename Reply, typename Act>
class RequestIdCollection {
public:
    RequestIdCollection()
        : hook_(std::make_shared<NotifyHook>()) {}

    // Reactive send_request: routes through the underlying free
    // function with this collection's NotifyHook attached, so the
    // promise's fulfillment wakes wait_one() directly. This is the
    // path stress-test code should take when collecting N parallel
    // requests.
    template <typename Server, typename Req>
    void send_request(Server& server, Req req, Act act) {
        ids_.push_back(
            ::theia::runtime::send_request<Reply>(
                server, std::move(req), std::move(act), hook_));
    }

    // Legacy add(): keeps callers that already constructed a RequestId
    // working. The added rid is NOT wired to the cv hook (its promise
    // was sealed before add() ran), so wait_one() falls back to a
    // brief poll for it. Mixing add() and send_request() is fine —
    // the cv wakes for reactive ones; the poll catches the rest.
    void add(RequestId<Reply, Act>&& rid) {
        legacy_polled_count_ += 1;
        ids_.push_back(std::move(rid));
    }

    size_t size() const noexcept { return ids_.size(); }
    bool empty() const noexcept { return ids_.empty(); }

    // Wait for ANY one request in the collection to complete.
    //
    // Reactive path (all rids came through send_request() above):
    //   block on hook_->cv until ready_count > 0, then sweep ids_
    //   for the ready one. Wake-up is immediate when a promise fires.
    //
    // Polling fallback (legacy add()-ed rids in the mix):
    //   short poll-loop sweeps ids_ for ready ones; cv wake is still
    //   honored for the reactive subset.
    CallResult<Reply, Act> wait_one(int timeout_ms) {
        if (ids_.empty()) {
            return CallResult<Reply, Act>{
                CallTag::Error, Act{}, Reply{}, "empty collection"};
        }

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            // Quick sweep: any rid already ready? (Catches both
            // reactive arrivals after the previous wake, AND legacy
            // polled rids whose futures resolved between cv waits.)
            for (auto it = ids_.begin(); it != ids_.end(); ++it) {
                if (it->future_.wait_for(std::chrono::milliseconds(0))
                    == std::future_status::ready) {
                    auto act = std::move(it->act_);
                    auto fut = std::move(it->future_);
                    if (legacy_polled_count_ > 0) {
                        // Conservative decrement — we don't track which
                        // specific rid is legacy vs reactive, so just
                        // assume polled until we run out. Worst case:
                        // we poll a few extra times.
                        --legacy_polled_count_;
                    } else {
                        // Reactive rid consumed: balance ready_count.
                        hook_->ready_count.fetch_sub(
                            1, std::memory_order_acquire);
                    }
                    ids_.erase(it);
                    return consume_ready_(fut, std::move(act));
                }
            }

            // Reactive wait: block on cv until a promise fulfills or
            // the deadline expires. If the collection has legacy
            // polled rids in it, cap the cv wait so we re-sweep.
            std::unique_lock<std::mutex> lk(hook_->mu);
            auto cv_deadline = (legacy_polled_count_ > 0)
                ? std::min(deadline,
                           std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(2))
                : deadline;
            hook_->cv.wait_until(lk, cv_deadline, [this]{
                return hook_->ready_count.load(
                    std::memory_order_acquire) > 0;
            });
        }
        return CallResult<Reply, Act>{
            CallTag::Timeout, Act{}, Reply{}, {}};
    }

private:
    std::vector<RequestId<Reply, Act>> ids_;
    std::shared_ptr<NotifyHook>        hook_;
    size_t                              legacy_polled_count_{0};
};

// post_info — async string notification routed to handle_info. The
// content is captured by value via std::string so the caller doesn't
// have to keep the original buffer alive.
inline void post_info(GenServerBase& server, std::string info) {
    server.enqueue([info = std::move(info)](GenServerBase* base) {
        base->dispatch_info(info.c_str());
    });
}

// call — synchronous request/reply. OTP-equivalent identity:
//   call(s, req, act, t) ≡ receive_response(send_request(s, req, act), t)
// Block-and-abandon-on-timeout, single-shot. The composition is
// pulled out so callers see how the sync API is layered on the
// async primitive (as Erlang documents in gen_server's docstring
// for send_request).
template <typename Reply, typename Server, typename Req, typename Act>
CallResult<Reply, Act> call(Server& server,
                             Req req,
                             Act act,
                             int timeout_ms) {
    auto& tr = ::theia::runtime::tracer_for(Server::kNodeName);
    // CallWait/CallResume pair the sync-caller's block on the future
    // with its unblock. The collector reads the trace stream and
    // computes wait time = resume.ts - wait.ts. Independent of the
    // send_request's own Send/Recv corr_id (that pair traces the
    // request lifecycle; this pair traces the CALLER's wait).
    uint32_t wait_corr = tr.enabled()
        ? ::theia::runtime::next_trace_corr_id() : 0;
    if (tr.enabled()) {
        tr.emit(::theia::runtime::TraceEvent::CallWait,
                ::theia::runtime::msg_type_name<Req>(),
                wait_corr, nullptr, 0);
    }
    auto result = receive_response(
        send_request<Reply>(server, std::move(req), std::move(act)),
        timeout_ms);
    if (tr.enabled()) {
        tr.emit(::theia::runtime::TraceEvent::CallResume,
                ::theia::runtime::msg_type_name<Req>(),
                wait_corr, nullptr, 0);
    }
    return result;
}

// call_and_dispatch — convenience that runs call() and then routes the
// CallResult to the caller's handle_call_result/error/timeout overloads.
// `Caller` is typically the calling GenServer subclass; `caller_state`
// is its mutable State reference (which the caller already holds on
// its own thread since this is invoked from inside one of its handlers).
template <typename Reply, typename Caller, typename Server,
          typename Req, typename Act>
void call_and_dispatch(Caller& caller,
                       Server& server,
                       Req req,
                       Act act,
                       int timeout_ms) {
    auto result = call<Reply>(server, std::move(req), std::move(act),
                              timeout_ms);
    switch (result.tag) {
        case CallTag::Reply:
            caller.handle_call_result(result.reply, result.act,
                                       caller.state());
            break;
        case CallTag::Error:
            caller.handle_call_error(result.error, result.act,
                                      caller.state());
            break;
        case CallTag::Timeout:
            caller.handle_call_timeout(result.act, caller.state());
            break;
    }
}

}  // namespace runtime
}  // namespace theia
