#include "TimerService.hh"

namespace theia {
namespace runtime {

TimerService::TimerService() {
    thread_ = std::thread([this]{ this->loop_(); });
}

TimerService::~TimerService() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        running_.store(false);
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

TimerRef TimerService::send_after(int delay_ms,
                                   GenServerBase& dest,
                                   std::string msg) {
    auto e = std::make_shared<TimerEntry>();
    e->deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(delay_ms);
    e->dest = &dest;
    e->msg  = std::move(msg);
    e->seq  = next_seq_.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(mu_);
        heap_.push(e);
        // Wake the timer thread so it can re-evaluate the head: this
        // new entry might be the earliest.
        cv_.notify_one();
    }
    return TimerRef(e);
}

TimerRef TimerService::send_after_lambda(int delay_ms,
                                          std::function<void()> action) {
    auto e = std::make_shared<TimerEntry>();
    e->deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(delay_ms);
    e->action = std::move(action);
    e->seq    = next_seq_.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(mu_);
        heap_.push(e);
        cv_.notify_one();
    }
    return TimerRef(e);
}

int TimerService::cancel_timer(TimerRef ref) {
    if (!ref.valid()) return -1;
    auto e = ref.entry_;

    // Strict cancel:
    //   1. Try to flip Pending → Cancelled. If we win, the timer
    //      thread will see Cancelled when it pops and skip firing.
    //   2. If we lose because state is already Firing, take the
    //      fire_mu to wait out the in-progress fire, then return -1
    //      (the message has been posted).
    //   3. If state is Done or already Cancelled, return -1.
    TimerEntry::State expected = TimerEntry::State::Pending;
    if (e->state.compare_exchange_strong(expected,
                                          TimerEntry::State::Cancelled)) {
        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             e->deadline - now).count();
        if (remaining < 0) remaining = 0;
        // Wake the timer thread so it can re-pop the heap and drop
        // this entry instead of sleeping on a now-stale deadline.
        cv_.notify_one();
        return static_cast<int>(remaining);
    }

    // Lost the race or never was Pending. If currently Firing, block
    // on fire_mu until the fire finishes.
    if (expected == TimerEntry::State::Firing) {
        std::lock_guard<std::mutex> lk(e->fire_mu);
        // After we hold the lock, the fire either completed (state ==
        // Done) or saw our late Cancelled (impossible: we already lost
        // the CAS). Either way, the message has been posted by now.
    }
    return -1;
}

void TimerService::loop_() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_.load()) {
        if (heap_.empty()) {
            cv_.wait(lk, [this]{
                return !running_.load() || !heap_.empty();
            });
            continue;
        }

        auto top = heap_.top();

        // Skip cancelled entries lazily; they accumulate in the heap
        // when cancel_timer flips state without removing the entry.
        if (top->state.load() == TimerEntry::State::Cancelled) {
            heap_.pop();
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (top->deadline > now) {
            // Sleep until the deadline OR until something new arrives
            // (added timer, cancellation) — wait_until handles both.
            cv_.wait_until(lk, top->deadline);
            continue;
        }

        // Time to fire. Pop, attempt to flip Pending → Firing.
        heap_.pop();
        TimerEntry::State expected = TimerEntry::State::Pending;
        if (!top->state.compare_exchange_strong(expected,
                                                 TimerEntry::State::Firing)) {
            // Cancelled in flight. Skip.
            continue;
        }

        // Release the heap mutex BEFORE posting / running the action
        // (these acquire other locks; holding ours across them is
        // unnecessary and could deadlock if the action touched this
        // service). Take the per-timer fire_mu so strict cancel can
        // block until fire completes.
        {
            std::lock_guard<std::mutex> fire_lk(top->fire_mu);
            lk.unlock();
            if (top->action) {
                top->action();
            } else if (top->dest) {
                post_info(*top->dest, std::move(top->msg));
            }
            top->state.store(TimerEntry::State::Done);
            lk.lock();
        }
    }
}

// ---- process-wide TimerService accessor (mirrors process_logger) ---------

namespace {
// Non-owning. Set once on the main thread before nodes start; read from
// node threads via process_timers(). Set-once, so no lock needed.
TimerService*& process_timers_slot() noexcept {
    static TimerService* slot = nullptr;
    return slot;
}
}  // namespace

void set_process_timers(TimerService* timers) noexcept {
    process_timers_slot() = timers;
}

TimerService& process_timers() noexcept {
    TimerService* slot = process_timers_slot();
    // A node that uses timers without main publishing one is a wiring
    // bug (the generated main publishes whenever any node requires_timers).
    // Abort loudly rather than UB-deref a null.
    if (!slot) {
        std::fprintf(stderr,
            "[runtime] FATAL: process_timers() before set_process_timers() "
            "— a requires_timers node ran without a published TimerService\n");
        std::abort();
    }
    return *slot;
}

}  // namespace runtime
}  // namespace theia
