#include "sup_gui/main_frame.h"
#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/statusbr.h>
#include <wx/timer.h>

namespace sup_gui {

wxDEFINE_EVENT(EVT_SUP_FRAME, wxThreadEvent);

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
    // Order matches OTP observer_wx.erl tab order.
    system_panel_  = new SystemPanel       (notebook_);
    load_charts_   = new LoadChartsPanel   (notebook_);
    applications_  = new ApplicationsPanel (notebook_);
    processes_     = new ProcessesPanel    (notebook_);
    trace_         = new TracePanel        (notebook_);
    notebook_->AddPage(system_panel_, "System");
    notebook_->AddPage(load_charts_,  "Load Charts");
    notebook_->AddPage(applications_, "Applications");
    notebook_->AddPage(processes_,    "Processes");
    notebook_->AddPage(trace_,        "Trace");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(notebook_, 1, wxEXPAND);
    SetSizer(sizer);

    CreateStatusBar(2);
    SetStatusText("no transport — waiting on services/com", 0);
    SetStatusText("", 1);

    Bind(EVT_SUP_FRAME, &MainFrame::on_sup_frame, this);

    status_timer_ = new wxTimer(this, ID_STATUS_TIMER);
    status_timer_->Start(500);

    // Transport intentionally absent until services/com is up.
    // Phase 6 wires a gRPC client here; each frame it emits will fan
    // through post_frame_from_thread() into the existing panel
    // dispatch via wxQueueEvent(EVT_SUP_FRAME).
}

MainFrame::~MainFrame() = default;

void MainFrame::on_sup_frame(wxThreadEvent& evt) {
    // Once the gRPC client is up (phase 6), it posts ThreadEvents with
    // payload = (machine_name, tag, payload). Panels filter and decode.
    // Until then, this handler exists but is never invoked.
    (void)evt;
}

void MainFrame::on_close(wxCloseEvent& /*evt*/) {
    if (status_timer_) status_timer_->Stop();
    Destroy();
}

void MainFrame::on_status_tick(wxTimerEvent&) {
    // Empty status until the transport is wired. The text shown at
    // construction stays; nothing to refresh.
}

}  // namespace sup_gui
