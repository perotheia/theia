// Log panel — the logcat firehose. The answer to "what is the rig doing".
//
// DATA SOURCE: live node LOG LINES from com's LogForwarder LogStream (:7711,
// tag 0x0007) — the SAME lines `tdb logcat` shows. A SEPARATE stream + port from
// SupervisorView/TraceStream, fed to on_frame by grpc_client::run_log().
//
// Layout (top-down):
//   [filter substring]  level≥[choice]  [x] follow  [Clear]
//   wxListCtrl (virtual) — TIME | NODE | LVL | LINE
//
// Virtual list (wxLC_VIRTUAL) so only visible rows are formatted; storage is a
// deque capped at kRingCapacity. Mirrors trace_panel.cpp minus the decode tree.

#include "sup_gui/panels.h"

#include "supervisor_bridge.pb.h"   // services.com.LogRecord

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

constexpr uint16_t kTagLogRecord = 0x0007;
constexpr size_t   kRingCapacity = 8000;

// LogLevel ordinal → short name (system.services.log.LogLevel, same as tdb).
const char* level_name(uint32_t l) {
    switch (l) {
        case 0: return "VERB";
        case 1: return "DEBUG";
        case 2: return "INFO";
        case 3: return "WARN";
        case 4: return "ERROR";
        case 5: return "FATAL";
        default: return "?";
    }
}

// Row colouring by level — warnings amber, errors/fatal red-ish, info plain.
wxColour level_color(uint32_t l) {
    switch (l) {
        case 3:  return wxColour(255, 248, 220);   // WARN  — pale amber
        case 4:  return wxColour(255, 228, 225);   // ERROR — pale red
        case 5:  return wxColour(255, 205, 210);   // FATAL — deeper red
        default: return wxColour(255, 255, 255);   // VERB/DEBUG/INFO — white
    }
}

struct LogRow {
    int64_t      t_ms_local{};   // ts_ns/1e6
    std::string  machine;
    std::string  node;
    uint32_t     level{};
    std::string  line;
};

// Virtual list: format only the rows wx asks about.
class LogListCtrl : public wxListCtrl {
public:
    LogListCtrl(wxWindow* parent,
                const std::deque<LogRow>* rows,
                const std::vector<size_t>* visible,
                std::mutex* mtx)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL),
          rows_(rows), visible_(visible), mtx_(mtx) {
        AppendColumn("Time",  wxLIST_FORMAT_LEFT, 130);
        AppendColumn("Node",  wxLIST_FORMAT_LEFT, 150);
        AppendColumn("Lvl",   wxLIST_FORMAT_LEFT,  60);
        AppendColumn("Line",  wxLIST_FORMAT_LEFT, 900);
    }

protected:
    wxString OnGetItemText(long item, long col) const override {
        std::lock_guard<std::mutex> lk(*mtx_);
        if (item < 0 || static_cast<size_t>(item) >= visible_->size())
            return wxString();
        const size_t idx = (*visible_)[item];
        if (idx >= rows_->size()) return wxString();
        const LogRow& r = (*rows_)[idx];
        switch (col) {
            case 0: {
                time_t t  = static_cast<time_t>(r.t_ms_local / 1000);
                int    fr = static_cast<int>(r.t_ms_local % 1000);
                struct tm lt;
                localtime_r(&t, &lt);
                return wxString::Format("%02d:%02d:%02d.%03d",
                                        lt.tm_hour, lt.tm_min, lt.tm_sec, fr);
            }
            case 1: {
                // prefix the machine when multi-machine, else just the node
                if (!r.machine.empty())
                    return wxString::FromUTF8((r.machine + "/" + r.node).c_str());
                return wxString::FromUTF8(r.node.c_str(), r.node.size());
            }
            case 2: return wxString::FromAscii(level_name(r.level));
            case 3: return wxString::FromUTF8(r.line.c_str(), r.line.size());
        }
        return wxString();
    }

    wxListItemAttr* OnGetItemAttr(long item) const override {
        std::lock_guard<std::mutex> lk(*mtx_);
        if (item < 0 || static_cast<size_t>(item) >= visible_->size())
            return nullptr;
        const size_t idx = (*visible_)[item];
        if (idx >= rows_->size()) return nullptr;
        static thread_local wxListItemAttr attr;
        attr.SetBackgroundColour(level_color((*rows_)[idx].level));
        return &attr;
    }

private:
    const std::deque<LogRow>*   rows_;
    const std::vector<size_t>*  visible_;
    std::mutex*                 mtx_;
};

}  // namespace

struct LogPanelImpl {
    LogListCtrl*  list{nullptr};
    wxTextCtrl*   filter_input{nullptr};
    wxChoice*     level_choice{nullptr};
    wxCheckBox*   follow{nullptr};

    std::mutex            mtx;
    std::deque<LogRow>    rows;       // ring (cap kRingCapacity)
    std::vector<size_t>   visible;    // indices into rows passing the filter
    std::string           filter;     // lowercased substring (node or line)
    uint32_t              level_min{0};

    // Does row pass the current filter (level + substring)? Caller holds mtx.
    bool passes(const LogRow& r) const {
        if (r.level < level_min) return false;
        if (filter.empty()) return true;
        // case-insensitive substring over node + line
        auto has = [&](const std::string& s) {
            std::string lo(s);
            std::transform(lo.begin(), lo.end(), lo.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            return lo.find(filter) != std::string::npos;
        };
        return has(r.node) || has(r.line);
    }

    void rebuild_visible() {
        visible.clear();
        for (size_t i = 0; i < rows.size(); ++i)
            if (passes(rows[i])) visible.push_back(i);
    }
};

LogPanel::LogPanel(wxWindow* parent) : PanelBase(parent) {
    impl_ = new LogPanelImpl();
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // --- filter bar ---
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    impl_->filter_input = new wxTextCtrl(this, wxID_ANY, "",
        wxDefaultPosition, wxSize(280, -1), wxTE_PROCESS_ENTER);
    impl_->filter_input->SetHint("filter (node or text substring)");
    bar->Add(impl_->filter_input, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);

    bar->Add(new wxStaticText(this, wxID_ANY, "level ≥"),
             0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    impl_->level_choice = new wxChoice(this, wxID_ANY);
    for (const char* n : {"VERB", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"})
        impl_->level_choice->Append(n);
    impl_->level_choice->SetSelection(0);
    bar->Add(impl_->level_choice, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);

    impl_->follow = new wxCheckBox(this, wxID_ANY, "follow");
    impl_->follow->SetValue(true);
    bar->Add(impl_->follow, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);

    auto* clear = new wxButton(this, wxID_ANY, "Clear");
    bar->Add(clear, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    sizer->Add(bar, 0, wxEXPAND);

    // --- the virtual list ---
    impl_->list = new LogListCtrl(this, &impl_->rows, &impl_->visible, &impl_->mtx);
    // Floor the list's preferred size: in a nested-splitter pane squeezed near
    // zero (a transient allocate / a hard shrink) wx can compute a NEGATIVE
    // preferred size for the virtual list, which GTK rejects in its scrollbar
    // (`gtk_box_gadget_distribute: size >= 0`). A small floor keeps it >= 0.
    impl_->list->SetMinSize(wxSize(80, 40));
    sizer->Add(impl_->list, 1, wxEXPAND | wxALL, 2);
    SetSizer(sizer);

    // --- events: re-filter on change ---
    auto refilter = [this]() {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        wxString f = impl_->filter_input->GetValue().Lower();
        impl_->filter.assign(f.utf8_str());
        impl_->level_min = static_cast<uint32_t>(impl_->level_choice->GetSelection());
        impl_->rebuild_visible();
        impl_->list->SetItemCount(static_cast<long>(impl_->visible.size()));
        impl_->list->Refresh();
    };
    impl_->filter_input->Bind(wxEVT_TEXT, [refilter](wxCommandEvent&){ refilter(); });
    impl_->level_choice->Bind(wxEVT_CHOICE, [refilter](wxCommandEvent&){ refilter(); });
    clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->rows.clear();
        impl_->visible.clear();
        impl_->list->SetItemCount(0);
        impl_->list->Refresh();
    });
}

void LogPanel::on_frame(const std::string& machine_name, uint16_t tag,
                        const std::string& payload) {
    if (tag != kTagLogRecord) return;
    ::services::com::LogRecord rec;
    if (!rec.ParseFromString(payload)) return;

    LogRow row;
    row.t_ms_local = static_cast<int64_t>(rec.ts_ns() / 1000000ULL);
    row.machine    = machine_name;
    row.node       = rec.node();
    row.level      = rec.level();
    row.line       = rec.line();

    bool follow;
    long new_count;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->rows.push_back(std::move(row));
        if (impl_->rows.size() > kRingCapacity) {
            impl_->rows.pop_front();
            impl_->rebuild_visible();   // indices shifted — rebuild
        } else if (impl_->passes(impl_->rows.back())) {
            impl_->visible.push_back(impl_->rows.size() - 1);
        }
        new_count = static_cast<long>(impl_->visible.size());
        follow = impl_->follow->GetValue();
    }
    impl_->list->SetItemCount(new_count);
    if (follow && new_count > 0)
        impl_->list->EnsureVisible(new_count - 1);
}

}  // namespace sup_gui
