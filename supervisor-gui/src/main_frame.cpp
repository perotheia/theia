#include "sup_gui/main_frame.h"
#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/statusbr.h>
#include <wx/timer.h>

namespace sup_gui {

wxDEFINE_EVENT(EVT_SUP_FRAME, wxThreadEvent);

// IDs for menu / timer events.
enum : int {
    ID_STATUS_TIMER = wxID_HIGHEST + 1,
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_CLOSE(MainFrame::on_close)
    EVT_TIMER(ID_STATUS_TIMER, MainFrame::on_status_tick)
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "supervisor-gui",
              wxDefaultPosition, wxSize(1280, 800)) {

    notebook_ = new wxNotebook(this, wxID_ANY);
    tree_       = new TreePanel      (notebook_);
    processes_  = new ProcessPanel   (notebook_);
    trace_      = new TracePanel     (notebook_);
    tombstones_ = new TombstonePanel (notebook_);
    perf_       = new PerfPanel      (notebook_);
    notebook_->AddPage(tree_,       "Tree");
    notebook_->AddPage(processes_,  "Processes");
    notebook_->AddPage(trace_,      "Trace");
    notebook_->AddPage(tombstones_, "Tombstones");
    notebook_->AddPage(perf_,       "Perf");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(notebook_, 1, wxEXPAND);
    SetSizer(sizer);

    CreateStatusBar(2);
    SetStatusText("disconnected", 0);
    SetStatusText("",             1);

    // Bind the custom event to our handler.
    Bind(EVT_SUP_FRAME, &MainFrame::on_sup_frame, this);

    // Status timer ticks every 500ms to refresh the "stale" indicator.
    status_timer_ = new wxTimer(this, ID_STATUS_TIMER);
    status_timer_->Start(500);

    // Wire the TIPC client. Address matches services.supervisor.Supervisor
    // in supervisor-gui/system/package.art.
    tipc_ = std::unique_ptr<TipcClient>(new TipcClient(
        0x80020001, 0,
        [this](uint16_t tag, std::string payload) {
            post_frame_from_thread(tag, std::move(payload));
        }));
    tipc_->start();
}

MainFrame::~MainFrame() {
    if (tipc_) tipc_->stop();
}

void MainFrame::post_frame_from_thread(uint16_t tag, std::string payload) {
    auto* evt = new wxThreadEvent(EVT_SUP_FRAME);
    // Stuff tag + payload into the event. wxThreadEvent has SetInt /
    // SetString — sufficient for our binary payload (std::string is
    // byte-clean; wxString conversion would corrupt it, so we go via
    // SetExtraLong as a sentinel and ship the bytes via SetPayload).
    evt->SetInt(static_cast<int>(tag));
    evt->SetPayload(payload);
    wxQueueEvent(this, evt);
}

void MainFrame::on_sup_frame(wxThreadEvent& evt) {
    const uint16_t tag = static_cast<uint16_t>(evt.GetInt());
    const std::string payload = evt.GetPayload<std::string>();

    // 0x0002 = HealthBeacon. Stamp the heartbeat clock; full decode happens
    // in the panels.
    if (tag == 0x0002) {
        last_heartbeat_.store(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    tree_      ->on_frame(tag, payload);
    processes_ ->on_frame(tag, payload);
    trace_     ->on_frame(tag, payload);
    tombstones_->on_frame(tag, payload);
    perf_      ->on_frame(tag, payload);
}

void MainFrame::on_close(wxCloseEvent& evt) {
    if (tipc_) tipc_->stop();
    if (status_timer_) status_timer_->Stop();
    Destroy();
}

void MainFrame::on_status_tick(wxTimerEvent&) {
    if (!tipc_) return;
    if (!tipc_->is_connected()) {
        SetStatusText("disconnected", 0);
        SetStatusText("",             1);
        return;
    }
    auto now    = std::chrono::steady_clock::now();
    auto hb     = std::chrono::steady_clock::time_point(
                    std::chrono::steady_clock::duration(last_heartbeat_.load()));
    auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - hb).count();
    if (last_heartbeat_.load() == 0) {
        SetStatusText("connected (no heartbeat yet)", 0);
    } else if (age_ms > 3500) {
        SetStatusText("stale heartbeat", 0);
    } else {
        SetStatusText("connected", 0);
    }
    SetStatusText(wxString::Format("hb age %lld ms", static_cast<long long>(age_ms)), 1);
}

}  // namespace sup_gui
