// PgDrops: a dropped pg message is counted per type and in total, never silently;
// the heartbeat carries the total (HeartbeatReport.pg_drops).
#include "PgDrops.hh"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
    auto& d = theia::runtime::PgDrops::get();
    CHECK(d.total() == 0);
    theia::runtime::note_pg_drop("system_a_Big", "encode", 70000);
    theia::runtime::note_pg_drop("system_a_Big", "encode", 70000);   // counted, not logged again
    theia::runtime::note_pg_drop("system_b_Bad", "decode", 2);
    CHECK(d.total() == 3);
    const auto per = d.per_type();
    CHECK(per.size() == 2 && per.at("system_a_Big") == 2 && per.at("system_b_Bad") == 1);
    theia::runtime::note_pg_drop(nullptr, nullptr, 0);                // no crash on missing names
    CHECK(d.total() == 4 && d.per_type().at("?") == 1);
    std::printf(fails ? "%d FAILED\n" : "pg_drops: ok\n", fails);
    return fails ? 1 : 0;
}
