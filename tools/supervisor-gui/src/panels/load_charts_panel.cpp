// Load Charts panel — observer_perf_wx.erl analogue.
//
// Four rolling strips per machine, 60s window:
//   1. Active workers       (active/total)
//   2. Restarts (cumulative)  — total_restarts from HealthBeacon
//   3. Tombstones (cumulative)
//   4. Uptime (s)
//
// Driven by HealthBeacon (tag 0x0002). Aggregate CPU% and aggregate
// RSS need new HealthBeacon fields on the supervisor side; until
// then, these four are what the wire carries.
//
// Repaint: 1Hz timer + immediate redraw on HealthBeacon receipt.
// Data: per-machine deque<Sample> with samples older than
// kWindowSeconds dropped on every push. Snapshot under a lock at
// paint time so the worker thread can keep pushing.
//
// wxGraphicsContext for the lines, wxDC for the axes — same pattern
// observer's perf panel uses.

#include "sup_gui/panels.h"

#include "HealthBeacon.pb.h"

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

namespace sup_gui {

namespace {

constexpr uint16_t kTagHealth      = 0x0002;
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
        const int64_t cutoff = s.t_ms -
            std::chrono::milliseconds(
                std::chrono::seconds(kWindowSeconds)).count();
        while (!q.empty() && q.front().t_ms < cutoff) q.pop_front();
    }

    void repaint() { if (IsShownOnScreen()) Refresh(false); }

private:
    void on_paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(*wxWHITE_BRUSH);
        dc.Clear();

        std::map<std::string, std::deque<Sample>> snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snap = data_;
        }

        if (snap.empty()) {
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText("Waiting for HealthBeacon…", 20, 20);
            return;
        }

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;

        const wxSize sz = GetClientSize();
        const int    w  = sz.GetWidth();

        int y = kStripPadding;
        for (const auto& kv : snap) {
            wxFont hdr = GetFont();
            hdr.MakeBold();
            dc.SetFont(hdr);
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText(wxString::FromUTF8(kv.first.c_str(), kv.first.size()),
                         8, y);
            y += 20;
            dc.SetFont(GetFont());

            const auto& q = kv.second;
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

    mutable std::mutex                          mtx_;
    std::map<std::string, std::deque<Sample>>    data_;
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
    if (tag != kTagHealth) return;
    if (!canvas_) return;

    ::services::supervisor::HealthBeacon hb;
    if (!hb.ParseFromString(payload)) return;

    Sample s;
    s.t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    s.total_workers    = hb.total_workers();
    s.active_workers   = hb.active_workers();
    s.total_restarts   = hb.total_restarts();
    s.total_tombstones = hb.total_tombstones();
    s.uptime_ms        = hb.uptime_ms();

    auto* c = static_cast<LoadChartsCanvas*>(canvas_);
    c->ingest(machine_name, s);
    c->repaint();
}

}  // namespace sup_gui
