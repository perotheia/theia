// Load Charts panel — observer_perf_wx.erl analogue.
//
// Placeholder today: text-only summary. Will host wxGraphicsContext-
// drawn time-series of total_restarts, active_workers/total_workers,
// and aggregate cpu% once /proc/<pid>/stat sampling lands (task #203).

#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/stattext.h>

namespace sup_gui {

LoadChartsPanel::LoadChartsPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* hint  = new wxStaticText(this, wxID_ANY,
        "Load Charts — TODO: time-series of restarts, active workers, "
        "and aggregate CPU once per-pid sampling lands.");
    sizer->Add(hint, 0, wxALL, 8);
    SetSizer(sizer);
}

void LoadChartsPanel::on_frame(const std::string& /*machine_name*/,
                                uint16_t /*tag*/,
                                const std::string& /*payload*/) {
    // Will accumulate HealthBeacon (0x0002) into a rolling window and
    // repaint on a timer.
}

}  // namespace sup_gui
