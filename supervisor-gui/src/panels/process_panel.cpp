// Process panel — placeholder for the flat process list (observer_pro_wx.erl).
// TODO: render a wxListCtrl with one row per worker from TreeSnapshot.

#include "sup_gui/panels.h"

#include <wx/wx.h>

namespace sup_gui {

ProcessPanel::ProcessPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* label = new wxStaticText(this, wxID_ANY,
        "Process panel — TODO: tabular view of workers from TreeSnapshot.");
    sizer->Add(label, 0, wxALL, 8);
    SetSizer(sizer);
}

void ProcessPanel::on_frame(uint16_t, const std::string&) {}

}  // namespace sup_gui
