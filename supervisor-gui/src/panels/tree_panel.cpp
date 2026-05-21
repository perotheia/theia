// Tree panel — placeholder for the supervisor tree (observer_app_wx.erl).
// TODO: render a wxTreeCtrl from TreeSnapshot messages; colour by state.

#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/treectrl.h>

namespace sup_gui {

TreePanel::TreePanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* label = new wxStaticText(this, wxID_ANY,
        "Tree panel — TODO: render supervisor tree from TreeSnapshot.");
    sizer->Add(label, 0, wxALL, 8);
    SetSizer(sizer);
}

void TreePanel::on_frame(uint16_t, const std::string&) {
    // Will decode TreeSnapshot (tag 0x0003) here and rebuild the tree.
}

}  // namespace sup_gui
