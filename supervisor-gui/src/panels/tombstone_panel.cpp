// Tombstone panel — placeholder for the per-crash dump viewer (cdv_*.erl).
// TODO: list tombstones surfaced by tombstone_written events, open one
// in a detail pane with backtrace + /proc/self/maps + status sections.

#include "sup_gui/panels.h"

#include <wx/wx.h>

namespace sup_gui {

TombstonePanel::TombstonePanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* label = new wxStaticText(this, wxID_ANY,
        "Tombstone panel — TODO: list + detail view of libtombstone files.");
    sizer->Add(label, 0, wxALL, 8);
    SetSizer(sizer);
}

void TombstonePanel::on_frame(uint16_t, const std::string&) {}

}  // namespace sup_gui
