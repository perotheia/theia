// Trace panel — analogue of observer_trace_wx.erl + et_wx_viewer.erl.
// Maintains a scrolling list of decoded SupervisionEvent rows so the
// operator can see crashes and restart cascades as they happen.

#include "sup_gui/panels.h"

#include "SupervisionEvent.pb.h"

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <chrono>

namespace sup_gui {

namespace {
const char* kind_name(uint32_t k) {
    switch (k) {
        case 0: return "child_started";
        case 1: return "child_exited";
        case 2: return "restart_cascade";
        case 3: return "tombstone_written";
        case 4: return "escalation";
        case 5: return "tree_changed";
        default: return "?";
    }
}
}  // namespace

TracePanel::TracePanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL);
    list_->InsertColumn(0, "time",        wxLIST_FORMAT_LEFT,  100);
    list_->InsertColumn(1, "kind",        wxLIST_FORMAT_LEFT,  140);
    list_->InsertColumn(2, "child",       wxLIST_FORMAT_LEFT,  200);
    list_->InsertColumn(3, "supervisor",  wxLIST_FORMAT_LEFT,  140);
    list_->InsertColumn(4, "pid",         wxLIST_FORMAT_RIGHT,  80);
    list_->InsertColumn(5, "code",        wxLIST_FORMAT_RIGHT,  60);
    list_->InsertColumn(6, "detail",      wxLIST_FORMAT_LEFT,  400);
    // Non-virtual mode for the skeleton — simpler. Switch to virtual when
    // event rates demand it.
    list_->SetWindowStyleFlag(wxLC_REPORT | wxLC_SINGLE_SEL);

    sizer->Add(list_, 1, wxEXPAND | wxALL, 4);
    SetSizer(sizer);
}

void TracePanel::on_frame(uint16_t tag, const std::string& payload) {
    if (tag != 0x0001) return;  // only SupervisionEvent here
    services::supervisor::SupervisionEvent ev;
    if (!ev.ParseFromString(payload)) return;

    // Format timestamp_ms (epoch ms) as HH:MM:SS.mmm in local time.
    const long long ms = static_cast<long long>(ev.timestamp_ms());
    time_t t  = static_cast<time_t>(ms / 1000);
    int    fr = static_cast<int>(ms % 1000);
    struct tm lt;
    localtime_r(&t, &lt);
    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d.%03d",
                  lt.tm_hour, lt.tm_min, lt.tm_sec, fr);

    long row = list_->InsertItem(list_->GetItemCount(), tbuf);
    list_->SetItem(row, 1, kind_name(ev.kind()));
    list_->SetItem(row, 2, ev.child_name());
    list_->SetItem(row, 3, ev.supervisor_name());
    list_->SetItem(row, 4, wxString::Format("%d", ev.pid()));
    list_->SetItem(row, 5, wxString::Format("%d", ev.exit_code()));
    std::string detail = ev.detail();
    if (!ev.tombstone_path().empty()) {
        if (!detail.empty()) detail += " — ";
        detail += "tombstone=";
        detail += ev.tombstone_path();
    }
    list_->SetItem(row, 6, detail);
    list_->EnsureVisible(row);
}

}  // namespace sup_gui
