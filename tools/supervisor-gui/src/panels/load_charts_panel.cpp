// Load Charts panel — observer_perf_wx.erl analogue.
//
// Per machine, 60s rolling window. Two kinds of strips:
//
//   PER-PROCESS load (from TreeSnapshot, tag 0x0003) — observer's
//   Scheduler-Utilization graph style: ONE coloured polyline per FC,
//   with a legend:
//     1. CPU %   (ChildState.cpu_pct, hundredths → %)
//     2. RSS MB  (ChildState.rss_kb)
//
//   SUPERVISOR health (from HealthBeacon, tag 0x0002) — one line each:
//     3. Active workers
//     4. Restarts (cumulative)
//     5. Tombstones (cumulative)
//     6. Uptime (s)
//
// Both feeds share the same 60s window + per-paint lock-snapshot. The
// per-process series is keyed machine → child → deque<ProcSample>; a
// child gone from a snapshot stops getting points and ages out.
//
// TIPC traffic per node is NOT here yet — the supervisor doesn't sample
// it (SocketInfo.sockets is an unfilled placeholder). Follow-up.
//
// wxGraphicsContext for the lines, wxDC for the axes — same pattern
// observer's perf panel uses.

#include "sup_gui/panels.h"

#include "supervisor.pb.h"

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

constexpr uint16_t kTagHealth      = 0x0002;
constexpr uint16_t kTagTree        = 0x0003;
constexpr int      kStripHeight    = 80;
constexpr int      kStripPadding   = 4;
constexpr int      kWindowSeconds  = 60;
constexpr int      kRefreshMs      = 1000;

struct Sample {
    int64_t  t_ms{};
    uint32_t total_workers{};
    uint32_t active_workers{};
    uint64_t total_restarts{};
    uint64_t total_tombstones{};
    uint64_t uptime_ms{};
};

// One per-process sample (from a ChildState in a TreeSnapshot).
struct ProcSample {
    int64_t  t_ms{};
    double   cpu_pct{};   // already converted from hundredths
    double   rss_mb{};
};

// Stable colour per series index (FC), OTP-perf-style palette. Cycles.
wxColour series_colour(size_t i) {
    static const wxColour kPalette[] = {
        wxColour(0x1E, 0x90, 0xFF),  // dodger blue
        wxColour(0xE6, 0x55, 0x00),  // orange
        wxColour(0x2E, 0xA0, 0x44),  // green
        wxColour(0xB0, 0x30, 0xB0),  // purple
        wxColour(0xC0, 0x39, 0x2B),  // red
        wxColour(0x16, 0xA0, 0x85),  // teal
        wxColour(0x8E, 0x6E, 0x1F),  // olive
        wxColour(0x34, 0x4A, 0x9E),  // indigo
    };
    return kPalette[i % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

}  // namespace


// Subclass-of-wxPanel that owns the data + the paint logic.
// LoadChartsPanel below is a thin shell that creates one of these
// and feeds frames into it.
class LoadChartsCanvas : public wxPanel {
public:
    explicit LoadChartsCanvas(wxWindow* parent) : wxPanel(parent) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);   // wxAutoBufferedPaintDC
        Bind(wxEVT_PAINT, &LoadChartsCanvas::on_paint, this);
    }

    void ingest(const std::string& machine, const Sample& s) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& q = data_[machine];
        q.push_back(s);
        prune(q, s.t_ms);
    }

    // Per-process CPU/RSS points for one machine, parsed from a TreeSnapshot:
    // {child_name → ProcSample}. Each child's series gets ONE point per
    // snapshot; a child absent from this snapshot simply gets none and ages
    // out of its 60s window. Empty series (all points aged out) are dropped.
    void ingest_procs(const std::string& machine,
                      const std::map<std::string, ProcSample>& procs,
                      int64_t t_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& by_child = procs_[machine];
        for (const auto& kv : procs) by_child[kv.first].push_back(kv.second);
        for (auto it = by_child.begin(); it != by_child.end();) {
            prune(it->second, t_ms);
            if (it->second.empty()) it = by_child.erase(it);
            else ++it;
        }
    }

    void repaint() { if (IsShownOnScreen()) Refresh(false); }

private:
    void on_paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(*wxWHITE_BRUSH);
        dc.Clear();

        std::map<std::string, std::deque<Sample>> snap;
        std::map<std::string,
                 std::map<std::string, std::deque<ProcSample>>> psnap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snap  = data_;
            psnap = procs_;
        }

        if (snap.empty() && psnap.empty()) {
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText("Waiting for supervisor data…", 20, 20);
            return;
        }

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;

        const wxSize sz = GetClientSize();
        const int    w  = sz.GetWidth();

        // Union of machine names across both feeds, sorted.
        std::vector<std::string> machines;
        for (const auto& kv : snap)  machines.push_back(kv.first);
        for (const auto& kv : psnap)
            if (snap.find(kv.first) == snap.end())
                machines.push_back(kv.first);
        std::sort(machines.begin(), machines.end());

        int y = kStripPadding;
        for (const auto& m : machines) {
            wxFont hdr = GetFont();
            hdr.MakeBold();
            dc.SetFont(hdr);
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText(wxString::FromUTF8(m.c_str(), m.size()), 8, y);
            y += 20;
            dc.SetFont(GetFont());

            // Per-process CPU% + RSS (one coloured line per FC, with legend).
            auto pit = psnap.find(m);
            if (pit != psnap.end() && !pit->second.empty()) {
                draw_proc_strip(*gc, dc, pit->second, w, y, "CPU %",
                                [](const ProcSample& p) { return p.cpu_pct; });
                y += kStripHeight + kStripPadding;
                draw_proc_strip(*gc, dc, pit->second, w, y, "RSS MB",
                                [](const ProcSample& p) { return p.rss_mb; });
                y += kStripHeight + kStripPadding + 6;
            }

            // Supervisor health (HealthBeacon).
            auto hit = snap.find(m);
            if (hit != snap.end()) {
                const auto& q = hit->second;
                draw_strip(*gc, dc, q, w, y, "active workers",
                           [](const Sample& s) {
                               return static_cast<double>(s.active_workers);
                           });
                y += kStripHeight + kStripPadding;
                draw_strip(*gc, dc, q, w, y, "restarts",
                           [](const Sample& s) {
                               return static_cast<double>(s.total_restarts);
                           });
                y += kStripHeight + kStripPadding;
                draw_strip(*gc, dc, q, w, y, "tombstones",
                           [](const Sample& s) {
                               return static_cast<double>(s.total_tombstones);
                           });
                y += kStripHeight + kStripPadding;
                draw_strip(*gc, dc, q, w, y, "uptime (s)",
                           [](const Sample& s) {
                               return static_cast<double>(s.uptime_ms / 1000);
                           });
                y += kStripHeight + kStripPadding + 6;
            }
            y += 6;
        }
    }

    // Multi-series strip: one coloured polyline per child + a legend. Shares
    // the 60s window + Y=0-based autoscale with draw_strip, but iterates a
    // map<child → series> and colours each line by index.
    template <typename Fn>
    void draw_proc_strip(wxGraphicsContext& gc, wxDC& dc,
                         const std::map<std::string, std::deque<ProcSample>>& by_child,
                         int w, int y, const char* label, Fn value_fn) {
        constexpr int LX = 100;
        constexpr int RX = 8;
        const int     pw = std::max(w - LX - RX, 50);
        const int     ph = kStripHeight;

        dc.SetTextForeground(wxColour(0x40, 0x40, 0x40));
        dc.DrawText(label, 8, y + ph / 2 - 6);

        gc.SetPen(wxPen(wxColour(0xC0, 0xC0, 0xC0), 1));
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.DrawRectangle(LX, y, pw, ph);

        // Shared window + Y autoscale across every series.
        int64_t t_max = 0;
        double  ymax  = 0.0;
        for (const auto& kv : by_child) {
            for (const auto& p : kv.second) {
                t_max = std::max(t_max, p.t_ms);
                ymax  = std::max(ymax, value_fn(p));
            }
        }
        if (t_max == 0) {
            dc.SetTextForeground(wxColour(0x80, 0x80, 0x80));
            dc.DrawText("(no data)", LX + 8, y + ph / 2 - 6);
            return;
        }
        if (ymax <= 0.0) ymax = 1.0;

        const int64_t t_min = t_max -
            std::chrono::milliseconds(
                std::chrono::seconds(kWindowSeconds)).count();
        const double  t_span = static_cast<double>(t_max - t_min);
        auto px_x = [&](int64_t t) { return LX + ((t - t_min) / t_span) * pw; };
        auto px_y = [&](double v) { return y + ph - (v / ymax) * ph; };

        // Y-axis max label (top-left, inside).
        dc.SetTextForeground(wxColour(0x80, 0x80, 0x80));
        dc.DrawText(wxString::Format("%g", ymax), LX + 2, y + 1);

        // One polyline per child.
        size_t idx = 0;
        for (const auto& kv : by_child) {
            const wxColour col = series_colour(idx++);
            gc.SetPen(wxPen(col, 2));
            wxGraphicsPath path = gc.CreatePath();
            bool first = true;
            for (const auto& p : kv.second) {
                const double x = px_x(p.t_ms);
                const double yp = px_y(value_fn(p));
                if (first) { path.MoveToPoint(x, yp); first = false; }
                else        path.AddLineToPoint(x, yp);
            }
            if (!first) gc.StrokePath(path);
        }

        // Legend, packed into as many columns as the strip height allows so a
        // wide FC count doesn't overrun the box. Swatch + name per row; columns
        // fill top-to-bottom then wrap right. Same colour order as the lines.
        constexpr int kRowH = 13, kColW = 80;
        const int rows = std::max(1, (ph - 4) / kRowH);
        const int cols = (static_cast<int>(by_child.size()) + rows - 1) / rows;
        const int lx0  = LX + pw - cols * kColW - 4;
        idx = 0;
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (const auto& kv : by_child) {
            const wxColour col = series_colour(idx);
            const int r  = static_cast<int>(idx) % rows;
            const int c  = static_cast<int>(idx) / rows;
            const int lx = lx0 + c * kColW;
            const int ly = y + 2 + r * kRowH;
            dc.SetBrush(wxBrush(col));
            dc.DrawRectangle(lx, ly + 2, 8, 8);
            dc.SetTextForeground(col);
            dc.DrawText(wxString::FromUTF8(kv.first.c_str(), kv.first.size()),
                        lx + 11, ly);
            ++idx;
        }
    }

    template <typename Fn>
    void draw_strip(wxGraphicsContext& gc, wxDC& dc,
                     const std::deque<Sample>& q, int w, int y,
                     const char* label, Fn value_fn) {
        constexpr int LX = 100;
        constexpr int RX = 8;
        const int     pw = std::max(w - LX - RX, 50);
        const int     ph = kStripHeight;

        dc.SetTextForeground(wxColour(0x40, 0x40, 0x40));
        dc.DrawText(label, 8, y + ph / 2 - 6);

        gc.SetPen(wxPen(wxColour(0xC0, 0xC0, 0xC0), 1));
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.DrawRectangle(LX, y, pw, ph);

        if (q.size() < 2) {
            dc.SetTextForeground(wxColour(0x80, 0x80, 0x80));
            dc.DrawText("(no data)", LX + 8, y + ph / 2 - 6);
            return;
        }

        // Y-range: 0..max in the visible window. min stays 0 since
        // every plotted quantity is non-negative.
        double ymax = 0.0;
        for (const auto& s : q) ymax = std::max(ymax, value_fn(s));
        if (ymax <= 0.0) ymax = 1.0;

        const int64_t t_max = q.back().t_ms;
        const int64_t t_min = t_max -
            std::chrono::milliseconds(
                std::chrono::seconds(kWindowSeconds)).count();
        const double  t_span = static_cast<double>(t_max - t_min);

        auto px_x = [&](int64_t t) {
            return LX + ((t - t_min) / t_span) * pw;
        };
        auto px_y = [&](double v) {
            return y + ph - (v / ymax) * ph;
        };

        gc.SetPen(wxPen(wxColour(0x1E, 0x90, 0xFF), 2));
        wxGraphicsPath path = gc.CreatePath();
        bool first = true;
        for (const auto& s : q) {
            const double x = px_x(s.t_ms);
            const double yp = px_y(value_fn(s));
            if (first) { path.MoveToPoint(x, yp); first = false; }
            else        path.AddLineToPoint(x, yp);
        }
        gc.StrokePath(path);

        // Current value at the upper-right.
        dc.SetTextForeground(*wxBLACK);
        dc.DrawText(wxString::Format("%g", value_fn(q.back())),
                     LX + pw - 60, y + 2);
    }

    // Drop window-expired points off the front of any time-ordered deque.
    template <typename Q>
    static void prune(Q& q, int64_t now_ms) {
        const int64_t cutoff = now_ms -
            std::chrono::milliseconds(
                std::chrono::seconds(kWindowSeconds)).count();
        while (!q.empty() && q.front().t_ms < cutoff) q.pop_front();
    }

    mutable std::mutex                          mtx_;
    std::map<std::string, std::deque<Sample>>    data_;
    // machine → child → per-process CPU/RSS series.
    std::map<std::string,
             std::map<std::string, std::deque<ProcSample>>> procs_;
};


LoadChartsPanel::LoadChartsPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* canvas = new LoadChartsCanvas(this);
    canvas_ = canvas;
    sizer->Add(canvas, 1, wxEXPAND);
    SetSizer(sizer);

    auto* timer = new wxTimer(this);
    Bind(wxEVT_TIMER, [canvas](wxTimerEvent&) { canvas->repaint(); });
    timer->Start(kRefreshMs);
    // wxTimer is owned by `this` (its evtHandler); will outlive the
    // panel cleanly. We don't store a member to keep the diff small.
}


void LoadChartsPanel::on_frame(const std::string& machine_name,
                                uint16_t tag,
                                const std::string& payload) {
    if (!canvas_) return;
    auto* c = static_cast<LoadChartsCanvas*>(canvas_);
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

    if (tag == kTagHealth) {
        ::system_supervisor::HealthBeacon hb;
        if (!hb.ParseFromString(payload)) return;
        Sample s;
        s.t_ms             = now_ms;
        s.total_workers    = hb.total_workers();
        s.active_workers   = hb.active_workers();
        s.total_restarts   = hb.total_restarts();
        s.total_tombstones = hb.total_tombstones();
        s.uptime_ms        = hb.uptime_ms();
        c->ingest(machine_name, s);
        c->repaint();
        return;
    }

    if (tag == kTagTree) {
        // Per-process CPU%/RSS from each running worker (kind 0). The same
        // TreeSnapshot the Processes/Applications panels consume; we keep only
        // the two load dimensions and time-series them.
        ::system_supervisor::TreeSnapshot tree;
        if (!tree.ParseFromString(payload)) return;
        std::map<std::string, ProcSample> procs;
        for (const auto& ch : tree.children()) {
            if (ch.kind() != 0) continue;          // workers only, not supervisors
            if (ch.state() != 2) continue;         // running only (no flatline at 0)
            ProcSample p;
            p.t_ms    = now_ms;
            p.cpu_pct = ch.cpu_pct() / 100.0;      // hundredths → percent
            p.rss_mb  = ch.rss_kb() / 1024.0;
            procs[ch.name()] = p;
        }
        if (!procs.empty()) {
            c->ingest_procs(machine_name, procs, now_ms);
            c->repaint();
        }
        return;
    }
}

}  // namespace sup_gui
