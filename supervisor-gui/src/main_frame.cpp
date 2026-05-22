#include "sup_gui/main_frame.h"
#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/statusbr.h>
#include <wx/timer.h>

#include <utility>

namespace sup_gui {

wxDEFINE_EVENT(EVT_SUP_FRAME, wxThreadEvent);

enum : int {
    ID_STATUS_TIMER = wxID_HIGHEST + 1,
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_CLOSE(MainFrame::on_close)
    EVT_TIMER(ID_STATUS_TIMER, MainFrame::on_status_tick)
wxEND_EVENT_TABLE()

namespace {
struct FrameMsg {
    std::string machine_name;
    std::string payload;
};
}  // namespace

MainFrame::MainFrame(std::vector<MachineEndpoint> machines)
    : wxFrame(nullptr, wxID_ANY, "supervisor-gui",
              wxDefaultPosition, wxSize(1280, 800)) {

    notebook_ = new wxNotebook(this, wxID_ANY);
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
    SetStatusText("connecting…", 0);
    SetStatusText("", 1);

    Bind(EVT_SUP_FRAME, &MainFrame::on_sup_frame, this);

    status_timer_ = new wxTimer(this, ID_STATUS_TIMER);
    status_timer_->Start(500);

    // One GrpcClient per machine.
    for (auto& m : machines) {
        std::string host_port = m.address + ":" + std::to_string(m.port);
        clients_.emplace_back(new GrpcClient(
            m.name, host_port,
            [this](const std::string& mn, uint16_t tag, std::string payload) {
                post_frame_from_thread(mn, tag, std::move(payload));
            }));
        clients_.back()->start();
    }
}

MainFrame::~MainFrame() {
    for (auto& c : clients_) {
        if (c) c->stop();
    }
}

void MainFrame::post_frame_from_thread(const std::string& machine_name,
                                       uint16_t tag,
                                       std::string payload) {
    auto* evt = new wxThreadEvent(EVT_SUP_FRAME);
    evt->SetInt(static_cast<int>(tag));
    FrameMsg msg;
    msg.machine_name = machine_name;
    msg.payload      = std::move(payload);
    evt->SetPayload(msg);
    wxQueueEvent(this, evt);
}

void MainFrame::on_sup_frame(wxThreadEvent& evt) {
    const uint16_t tag = static_cast<uint16_t>(evt.GetInt());
    const FrameMsg msg = evt.GetPayload<FrameMsg>();

    if (tag == 0x0002) {
        last_heartbeat_.store(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    system_panel_ ->on_frame(msg.machine_name, tag, msg.payload);
    load_charts_  ->on_frame(msg.machine_name, tag, msg.payload);
    applications_ ->on_frame(msg.machine_name, tag, msg.payload);
    processes_    ->on_frame(msg.machine_name, tag, msg.payload);
    trace_        ->on_frame(msg.machine_name, tag, msg.payload);
}

void MainFrame::on_close(wxCloseEvent& /*evt*/) {
    for (auto& c : clients_) {
        if (c) c->stop();
    }
    if (status_timer_) status_timer_->Stop();
    Destroy();
}

void MainFrame::on_status_tick(wxTimerEvent&) {
    size_t connected = 0;
    for (const auto& c : clients_) {
        if (c && c->is_connected()) ++connected;
    }
    if (clients_.empty()) {
        SetStatusText("no machines configured", 0);
        SetStatusText("", 1);
        return;
    }
    wxString status;
    if (connected == clients_.size()) {
        status.Printf("connected (%zu/%zu)", connected, clients_.size());
    } else if (connected == 0) {
        status.Printf("disconnected (0/%zu)", clients_.size());
    } else {
        status.Printf("partial (%zu/%zu)", connected, clients_.size());
    }
    SetStatusText(status, 0);

    if (last_heartbeat_.load() == 0) {
        SetStatusText("no heartbeat yet", 1);
    } else {
        auto now = std::chrono::steady_clock::now();
        auto hb  = std::chrono::steady_clock::time_point(
                       std::chrono::steady_clock::duration(last_heartbeat_.load()));
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - hb).count();
        SetStatusText(
            wxString::Format("hb age %lld ms", static_cast<long long>(age_ms)), 1);
    }
}

}  // namespace sup_gui
