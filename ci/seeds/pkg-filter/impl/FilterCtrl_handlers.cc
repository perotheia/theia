// CI SEED — FilterCtrl handler bodies (USER-side, write-once slot of the
// harness's filter package). Consumes the sensor package's SampleStream (pg
// group) and counts what arrives; GetStats exposes the count — the s5 scenario's
// data-flow assertion (received must GROW while the pipeline runs).

#include "lib/FilterCtrl.hh"

namespace ara::filter {

void FilterCtrl::init(FilterCtrlState& /*s*/) {
    // Join the imported package's Sample group — frames land in handle_cast.
    // (A blocking CALL to the supervisor's PG allocator; safe in init(): main
    // wires + starts the mux BEFORE node.start().)
    pg_join<Sample>();
    log().info("filter up — consuming SampleStream");
}

void FilterCtrl::handle_info(const char* /*info*/, FilterCtrlState& /*s*/) {
}

void FilterCtrl::handle_cast(const Sample& msg, FilterCtrlState& s) {
    ++s.received;
    s.last_value = msg.value;
}

FilterStats FilterCtrl::handle_call(
        const FilterEmpty& /*req*/,
        FilterCtrlState& s) {
    FilterStats st{};
    st.received   = s.received;
    st.last_value = s.last_value;
    return st;
}

}  // namespace ara::filter
