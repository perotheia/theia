// Load Charts panel — observer_perf_wx.erl analogue (observer dashboard layout).
//
// Per machine, 60s rolling window, laid out as three zones like the Erlang
// observer "Load Charts" tab:
//
//   TOP (full width)  — CPU / "Scheduler" Utilization %: ONE coloured polyline
//                       per FC (ChildState.cpu_pct, hundredths → %), with a
//                       per-series colour legend along the bottom.
//   BOTTOM-LEFT       — Memory Usage (MB): one coloured line per FC
//                       (ChildState.rss_kb → MB), same colour order as the top.
//   BOTTOM-RIGHT      — GPU load + mem: placeholder ("awaiting shwa feed") on
//                       one graph (two lines once the shwa nvidia-smi/jtop feed
//                       is wired). Replaces observer's IO-usage graph.
//
// Fed by TreeSnapshot (tag 0x0003): the same per-process CPU%/RSS the
// Processes/Applications panels consume, time-series'd. The supervisor-health
// strips (workers/restarts/tombstones/uptime) moved to the System tab's
// "Supervisor Statistics" box and are no longer plotted here.
//
// The per-process series is keyed machine → child → deque<ProcSample>; a child
// gone from a snapshot stops getting points and ages out of the 60s window.
//
// wxGraphicsContext for the lines, wxDC for the axes — same pattern observer's
// perf panel uses.

#include "sup_gui/panels.h"

#include "supervisor.pb.h"

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

constexpr uint16_t kTagTree        = 0x0003;
constexpr int      kStripPadding   = 4;
constexpr int      kWindowSeconds  = 60;
constexpr int      kRefreshMs      = 1000;
constexpr int      kMinDashHeight  = 260;   // min height of one machine's 3-zone dashboard

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

// Grid: 6 columns (time, 10s each over the 60s window) × 4 rows (value).
constexpr int kGridCols = 6;
constexpr int kGridRows = 4;

// Round a raw max up to a "nice" ceiling so the Y axis lands on round numbers
// and divides cleanly by kGridRows: 0.89 → 1.0 (mid 0.5), 17.21 → 20 (mid 10).
// Picks the smallest of {1,2,2.5,5}×10^k that is ≥ v. Returns ≥ 1.
double nice_ceiling(double v) {
    if (v <= 0.0) return 1.0;
    const double exp10 = std::floor(std::log10(v));
    const double base  = std::pow(10.0, exp10);
    const double frac  = v / base;                 // in [1, 10)
    double nice;
    if      (frac <= 1.0) nice = 1.0;
    else if (frac <= 2.0) nice = 2.0;
    else if (frac <= 2.5) nice = 2.5;
    else if (frac <= 5.0) nice = 5.0;
    else                  nice = 10.0;
    return nice * base;
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

        std::map<std::string,
                 std::map<std::string, std::deque<ProcSample>>> psnap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            psnap = procs_;
        }

        if (psnap.empty()) {
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText("Waiting for supervisor data…", 20, 20);
            return;
        }

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;

        const wxSize sz = GetClientSize();
        const int    w  = sz.GetWidth();
        const int    h  = sz.GetHeight();

        std::vector<std::string> machines;
        for (const auto& kv : psnap) machines.push_back(kv.first);
        std::sort(machines.begin(), machines.end());

        // Each machine gets its own three-zone dashboard, stacked vertically.
        const int dash_h = std::max(kMinDashHeight,
            (h - kStripPadding) / std::max<int>(1, machines.size())
                - kStripPadding);

        int y = kStripPadding;
        for (const auto& m : machines) {
            wxFont hdr = GetFont();
            hdr.MakeBold();
            dc.SetFont(hdr);
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText(wxString::FromUTF8(m.c_str(), m.size()), 8, y);
            dc.SetFont(GetFont());
            const int top = y + 18;

            const auto& by_child = psnap[m];

            // Zone geometry: top = full-width CPU; bottom split left/right.
            const int gap     = 8;
            const int top_h   = (dash_h - 18 - gap) * 55 / 100;   // ~55% tall
            const int bot_h   = (dash_h - 18 - gap) - top_h;
            const int bot_y   = top + top_h + gap;
            const int half_w  = (w - 3 * gap) / 2;

            // TOP — CPU / Scheduler Utilization % (full width), with legend.
            draw_proc_graph(*gc, dc, by_child,
                            wxRect(gap, top, w - 2 * gap, top_h),
                            "CPU / Scheduler Utilization (%)", "",
                            [](const ProcSample& p) { return p.cpu_pct; },
                            /*legend=*/true);

            // BOTTOM-LEFT — Memory Usage (MB).
            draw_proc_graph(*gc, dc, by_child,
                            wxRect(gap, bot_y, half_w, bot_h),
                            "Memory Usage (MB)", "",
                            [](const ProcSample& p) { return p.rss_mb; },
                            /*legend=*/false);

            // BOTTOM-RIGHT — GPU load + mem (placeholder until shwa feed).
            draw_gpu_placeholder(*gc, dc,
                            wxRect(gap + half_w + gap, bot_y, half_w, bot_h));

            y += dash_h + kStripPadding;
        }
    }

    // Draw the grid + axis labels inside a plot box: kGridCols vertical lines
    // (time, 10s each over the 60s window) and kGridRows horizontal lines
    // (value, ymax/kGridRows apart). Y labels sit just left-inside each
    // horizontal line (top = ymax, mid = ymax/2, etc.); the bottom row is 0.
    // `unit` is appended to each Y label ("", " MB", " %").
    void draw_grid(wxGraphicsContext& gc, wxDC& dc, const wxRect& box,
                   double ymax, const char* unit) {
        gc.SetPen(wxPen(wxColour(0xE4, 0xE4, 0xE4), 1));   // light grey lines
        for (int c = 1; c < kGridCols; ++c) {
            const double x = box.x + (double)box.width * c / kGridCols;
            wxGraphicsPath p = gc.CreatePath();
            p.MoveToPoint(x, box.y);
            p.AddLineToPoint(x, box.y + box.height);
            gc.StrokePath(p);
        }
        dc.SetTextForeground(wxColour(0x90, 0x90, 0x90));
        for (int rr = 1; rr < kGridRows; ++rr) {
            const double y = box.y + (double)box.height * rr / kGridRows;
            wxGraphicsPath p = gc.CreatePath();
            p.MoveToPoint(box.x, y);
            p.AddLineToPoint(box.x + box.width, y);
            gc.StrokePath(p);
        }
        // Y labels at each horizontal level (incl. top=ymax, bottom=0).
        for (int rr = 0; rr <= kGridRows; ++rr) {
            const double v = ymax * (kGridRows - rr) / kGridRows;
            const int    ly = box.y + box.height * rr / kGridRows;
            const int    off = (rr == 0) ? 1 : (rr == kGridRows ? -11 : -6);
            dc.DrawText(wxString::Format("%g%s", v, unit), box.x + 2, ly + off);
        }
    }

    // Multi-series graph into an arbitrary rect: one coloured polyline per
    // child, Y=0-based autoscale (rounded up to a nice ceiling) over the shared
    // 60s window, on a 6×4 grid. Draws a title above the plot box and, when
    // `legend`, a packed colour key bottom-right.
    template <typename Fn>
    void draw_proc_graph(wxGraphicsContext& gc, wxDC& dc,
                         const std::map<std::string, std::deque<ProcSample>>& by_child,
                         const wxRect& r, const char* title, const char* unit,
                         Fn value_fn, bool legend) {
        // Title above the box.
        dc.SetTextForeground(wxColour(0x20, 0x20, 0x20));
        dc.DrawText(title, r.x, r.y);
        const int box_y = r.y + 16;
        const int box_h = std::max(r.height - 16, 24);
        const wxRect box(r.x, box_y, r.width, box_h);

        gc.SetPen(wxPen(wxColour(0xC0, 0xC0, 0xC0), 1));
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.DrawRectangle(box.x, box.y, box.width, box.height);

        // Shared window + Y autoscale across every series.
        int64_t t_max = 0;
        double  raw_ymax = 0.0;
        for (const auto& kv : by_child) {
            for (const auto& p : kv.second) {
                t_max = std::max(t_max, p.t_ms);
                raw_ymax = std::max(raw_ymax, value_fn(p));
            }
        }
        // Nice ceiling so the axis lands on round numbers and the grid rows are
        // even (0.89 → 1.0 / mid 0.5; 17.21 → 20 / mid 10).
        const double ymax = nice_ceiling(raw_ymax);

        draw_grid(gc, dc, box, ymax, unit);

        if (t_max == 0) {
            dc.SetTextForeground(wxColour(0x80, 0x80, 0x80));
            dc.DrawText("(no data)", box.x + 8, box.y + box.height / 2 - 6);
            return;
        }

        const int64_t t_min = t_max -
            std::chrono::milliseconds(
                std::chrono::seconds(kWindowSeconds)).count();
        const double  t_span = static_cast<double>(t_max - t_min);
        auto px_x = [&](int64_t t) {
            return box.x + ((t - t_min) / t_span) * box.width;
        };
        auto px_y = [&](double v) {
            return box.y + box.height - (v / ymax) * box.height;
        };

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

        if (!legend) return;

        // Legend, packed into as many columns as the box height allows so a
        // wide FC count doesn't overrun. Swatch + name per row; columns fill
        // top-to-bottom then wrap right. Same colour order as the lines.
        constexpr int kRowH = 13, kColW = 80;
        const int rows = std::max(1, (box.height - 4) / kRowH);
        const int cols = (static_cast<int>(by_child.size()) + rows - 1) / rows;
        const int lx0  = box.x + box.width - cols * kColW - 4;
        idx = 0;
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (const auto& kv : by_child) {
            const wxColour col = series_colour(idx);
            const int rr = static_cast<int>(idx) % rows;
            const int cc = static_cast<int>(idx) / rows;
            const int lx = lx0 + cc * kColW;
            const int ly = box.y + 2 + rr * kRowH;
            dc.SetBrush(wxBrush(col));
            dc.DrawRectangle(lx, ly + 2, 8, 8);
            dc.SetTextForeground(col);
            dc.DrawText(wxString::FromUTF8(kv.first.c_str(), kv.first.size()),
                        lx + 11, ly);
            ++idx;
        }
    }

    // GPU load + mem placeholder (bottom-right). Mirrors draw_proc_graph's
    // title + empty plot box, with a centred "awaiting shwa feed" note and a
    // two-line legend (GPU load / GPU mem) so the eventual shwa wiring drops in.
    void draw_gpu_placeholder(wxGraphicsContext& gc, wxDC& dc, const wxRect& r) {
        dc.SetTextForeground(wxColour(0x20, 0x20, 0x20));
        dc.DrawText("GPU load + mem (%)", r.x, r.y);
        const int box_y = r.y + 16;
        const int box_h = std::max(r.height - 16, 24);

        gc.SetPen(wxPen(wxColour(0xC0, 0xC0, 0xC0), 1));
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.DrawRectangle(r.x, box_y, r.width, box_h);

        // Same 6×4 grid as the live graphs (0..100% scale) so the panels match
        // even before the shwa feed lands.
        draw_grid(gc, dc, wxRect(r.x, box_y, r.width, box_h), 100.0, "");

        dc.SetTextForeground(wxColour(0x80, 0x80, 0x80));
        dc.DrawText("awaiting shwa feed (nvidia-smi / jtop)",
                    r.x + 10, box_y + box_h / 2 - 6);

        // Two-line colour key so the layout already reads as a dual-series
        // graph. Distinct from the FC palette: GPU load = magenta, mem = teal.
        struct { const char* name; wxColour col; } keys[] = {
            {"GPU load", wxColour(0xB0, 0x30, 0xB0)},
            {"GPU mem",  wxColour(0x16, 0xA0, 0x85)},
        };
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (int i = 0; i < 2; ++i) {
            const int ly = box_y + 2 + i * 13;
            dc.SetBrush(wxBrush(keys[i].col));
            dc.DrawRectangle(r.x + r.width - 78, ly + 2, 8, 8);
            dc.SetTextForeground(keys[i].col);
            dc.DrawText(keys[i].name, r.x + r.width - 65, ly);
        }
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

    // HealthBeacon (tag 0x0002) is no longer plotted here — supervisor health
    // moved to the System tab's "Supervisor Statistics" box. Load Charts is now
    // purely the per-process CPU/Mem dashboard, fed by TreeSnapshot.

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
