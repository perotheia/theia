// Trace panel — Wireshark-style. Spec:
// docs/tasks/BACKLOG/GUI-trace-panel-wireshark-style.md
//
// Layout (top-down):
//   filter [substring or key:value tokens] [Clear]
//   wxListCtrl (virtual)  — TIME | MACHINE | KIND | CHILD | PARENT | DETAIL
//   wxTreeCtrl            — lazy-decoded selection: Header / Subject /
//                            Cause / Tombstone / Raw groups
//
// Top list is wxLC_VIRTUAL so we only format the rows wx asks
// about. Storage is a deque<EventRow> capped at 5000 (kRingCapacity).
//
// Tombstones aren't a separate tab — they show up here with
// kind=3 / tombstone_path set, and the Tombstone tree group
// renders the file's tail.

#include "sup_gui/panels.h"

#include "supervisor.pb.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/treectrl.h>
#include <wx/splitter.h>
#include <wx/textctrl.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace sup_gui {

namespace {

constexpr uint16_t kTagEvent      = 0x0001;
constexpr size_t   kRingCapacity  = 5000;

struct EventRow {
    int64_t      t_ms_local{};
    std::string  machine;
    uint32_t     kind{};
    std::string  child;
    std::string  parent;
    int32_t      pid{};
    int32_t      exit_code{};
    std::string  strategy;
    std::string  tombstone_path;
    std::string  detail;
    std::string  raw_payload;
};

const char* kind_name(uint32_t k) {
    switch (k) {
        case 0: return "STARTED";
        case 1: return "EXITED";
        case 2: return "RESTART_CASCADE";
        case 3: return "TOMBSTONE";
        case 4: return "ESCALATION";
        case 5: return "TREE_CHANGED";
        default: return "?";
    }
}

wxColour kind_color(uint32_t k) {
    switch (k) {
        case 0: return wxColour(0xE0, 0xF5, 0xE0);
        case 1: return wxColour(0xFF, 0xF6, 0xC0);
        case 2: return wxColour(0xFF, 0xE0, 0xB0);
        case 3: return wxColour(0xFF, 0xC0, 0xC0);
        case 4: return wxColour(0xD8, 0xB6, 0xFF);
        default: return *wxWHITE;
    }
}

wxString hex_dump(const std::string& bytes) {
    wxString out;
    char tmp[8];
    for (size_t off = 0; off < bytes.size(); off += 16) {
        std::snprintf(tmp, sizeof(tmp), "%04zx  ", off);
        out << tmp;
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < bytes.size()) {
                std::snprintf(tmp, sizeof(tmp), "%02x ",
                              static_cast<unsigned char>(bytes[off + i]));
                out << tmp;
            } else out << "   ";
            if (i == 7) out << " ";
        }
        out << " ";
        for (size_t i = 0; i < 16 && off + i < bytes.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(bytes[off + i]);
            out << (c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
        }
        out << "\n";
    }
    return out;
}

wxString tail_file(const std::string& path, int max_lines = 50) {
    if (path.empty()) return wxString("(no tombstone)");
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return wxString::Format("(cannot open %s: %s)",
                                      path.c_str(), std::strerror(errno));
    std::deque<std::string> lines;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f)) {
        lines.emplace_back(buf);
        if (static_cast<int>(lines.size()) > max_lines) lines.pop_front();
    }
    std::fclose(f);
    std::string out;
    for (const auto& l : lines) out += l;
    return wxString::FromUTF8(out.c_str(), out.size());
}

}  // namespace


// Virtual list bound to a deque<EventRow> via a separate visible[] index.
class TraceListCtrl : public wxListCtrl {
public:
    TraceListCtrl(wxWindow* parent,
                   const std::deque<EventRow>* rows,
                   const std::vector<size_t>* visible,
                   std::mutex* mtx)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                      wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL),
          rows_(rows), visible_(visible), mtx_(mtx) {
        AppendColumn("Time",    wxLIST_FORMAT_LEFT,  140);
        AppendColumn("Machine", wxLIST_FORMAT_LEFT,   90);
        AppendColumn("Kind",    wxLIST_FORMAT_LEFT,  130);
        AppendColumn("Child",   wxLIST_FORMAT_LEFT,  160);
        AppendColumn("Parent",  wxLIST_FORMAT_LEFT,  120);
        AppendColumn("Detail",  wxLIST_FORMAT_LEFT,  400);
    }

protected:
    wxString OnGetItemText(long item, long col) const override {
        std::lock_guard<std::mutex> lk(*mtx_);
        if (item < 0 || static_cast<size_t>(item) >= visible_->size())
            return wxString();
        const size_t idx = (*visible_)[item];
        if (idx >= rows_->size()) return wxString();
        const EventRow& r = (*rows_)[idx];

        switch (col) {
            case 0: {
                time_t t  = static_cast<time_t>(r.t_ms_local / 1000);
                int    fr = static_cast<int>(r.t_ms_local % 1000);
                struct tm lt;
                localtime_r(&t, &lt);
                return wxString::Format("%02d:%02d:%02d.%03d",
                                         lt.tm_hour, lt.tm_min, lt.tm_sec, fr);
            }
            case 1: return wxString::FromUTF8(r.machine.c_str(), r.machine.size());
            case 2: return wxString::FromAscii(kind_name(r.kind));
            case 3: return wxString::FromUTF8(r.child.c_str(), r.child.size());
            case 4: return wxString::FromUTF8(r.parent.c_str(), r.parent.size());
            case 5: {
                wxString d = wxString::FromUTF8(r.detail.c_str(), r.detail.size());
                if (!r.tombstone_path.empty()) {
                    if (!d.IsEmpty()) d += " — ";
                    d += "tombstone=";
                    d += wxString::FromUTF8(r.tombstone_path.c_str(),
                                              r.tombstone_path.size());
                }
                if (r.exit_code != 0) {
                    if (!d.IsEmpty()) d += " — ";
                    d += wxString::Format("exit=%d", r.exit_code);
                }
                return d;
            }
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
        attr.SetBackgroundColour(kind_color((*rows_)[idx].kind));
        return &attr;
    }

private:
    const std::deque<EventRow>* rows_;
    const std::vector<size_t>*  visible_;
    std::mutex*                 mtx_;
};


// Panel-internal state. Kept out of panels.h to avoid a header
// churn ripple in this commit; the old `list_` member in
// TracePanel is repurposed to hold the new virtual list ptr.
struct TracePanelImpl {
    wxSplitterWindow*     split{nullptr};
    wxTextCtrl*           filter_input{nullptr};
    TraceListCtrl*        list{nullptr};
    wxTreeCtrl*           tree{nullptr};

    std::deque<EventRow>  ring;
    std::vector<size_t>   visible;
    std::string           current_filter;
    std::mutex            mtx;

    void apply_filter() {  // caller holds mtx
        visible.clear();
        if (current_filter.empty()) {
            for (size_t i = 0; i < ring.size(); ++i) visible.push_back(i);
            return;
        }
        // Tokenize on whitespace; each token is "[key:]substring" with
        // case-insensitive match.
        std::vector<std::pair<std::string,std::string>> terms;
        {
            std::string cur;
            for (char c : current_filter) {
                if (c == ' ' || c == '\t') {
                    if (!cur.empty()) {
                        terms.emplace_back(std::string(), cur);
                        cur.clear();
                    }
                } else cur.push_back(c);
            }
            if (!cur.empty()) terms.emplace_back(std::string(), cur);
        }
        for (auto& kv : terms) {
            size_t colon = kv.second.find(':');
            if (colon != std::string::npos) {
                kv.first  = kv.second.substr(0, colon);
                kv.second = kv.second.substr(colon + 1);
            }
            for (auto& c : kv.first)  c = static_cast<char>(std::tolower(c));
            for (auto& c : kv.second) c = static_cast<char>(std::tolower(c));
        }
        auto ci_find = [](const std::string& hay, const std::string& needle) {
            if (needle.empty()) return true;
            std::string h; h.reserve(hay.size());
            for (char c : hay) h.push_back(static_cast<char>(std::tolower(c)));
            return h.find(needle) != std::string::npos;
        };
        for (size_t i = 0; i < ring.size(); ++i) {
            const EventRow& r = ring[i];
            bool ok = true;
            for (const auto& t : terms) {
                bool m = false;
                if (t.first.empty() || t.first == "detail")
                    m = ci_find(r.detail, t.second);
                else if (t.first == "kind") {
                    std::string k = kind_name(r.kind);
                    for (auto& c : k) c = static_cast<char>(std::tolower(c));
                    m = k.find(t.second) != std::string::npos;
                }
                else if (t.first == "child")   m = ci_find(r.child,   t.second);
                else if (t.first == "parent")  m = ci_find(r.parent,  t.second);
                else if (t.first == "machine") m = ci_find(r.machine, t.second);
                if (!m) { ok = false; break; }
            }
            if (ok) visible.push_back(i);
        }
    }

    void rebuild_tree(const EventRow& r) {
        tree->DeleteAllItems();
        wxTreeItemId root = tree->AddRoot(
            wxString::Format("event: %s", kind_name(r.kind)));

        wxTreeItemId hdr = tree->AppendItem(root, "Header");
        {
            time_t t  = static_cast<time_t>(r.t_ms_local / 1000);
            int    fr = static_cast<int>(r.t_ms_local % 1000);
            struct tm lt;
            localtime_r(&t, &lt);
            tree->AppendItem(hdr,
                wxString::Format("timestamp:  %02d:%02d:%02d.%03d  (%lld ms)",
                    lt.tm_hour, lt.tm_min, lt.tm_sec, fr,
                    static_cast<long long>(r.t_ms_local)));
            tree->AppendItem(hdr,
                wxString::Format("machine:    %s", r.machine.c_str()));
            tree->AppendItem(hdr,
                wxString::Format("kind:       %s (%u)",
                                  kind_name(r.kind), r.kind));
        }

        wxTreeItemId subj = tree->AppendItem(root, "Subject");
        tree->AppendItem(subj, wxString::Format("child:       %s",  r.child.c_str()));
        tree->AppendItem(subj, wxString::Format("supervisor:  %s",  r.parent.c_str()));
        tree->AppendItem(subj, wxString::Format("pid:         %d",  r.pid));

        if (r.exit_code != 0 || !r.strategy.empty() || !r.detail.empty()) {
            wxTreeItemId cause = tree->AppendItem(root, "Cause");
            tree->AppendItem(cause,
                wxString::Format("exit_code: %d", r.exit_code));
            if (!r.strategy.empty())
                tree->AppendItem(cause,
                    wxString::Format("strategy:  %s", r.strategy.c_str()));
            if (!r.detail.empty())
                tree->AppendItem(cause,
                    wxString::Format("detail:    %s", r.detail.c_str()));
        }

        if (!r.tombstone_path.empty()) {
            wxTreeItemId tomb = tree->AppendItem(root, "Tombstone");
            tree->AppendItem(tomb,
                wxString::Format("path: %s", r.tombstone_path.c_str()));
            tree->AppendItem(tomb, "tail:");
            wxString tail = tail_file(r.tombstone_path);
            wxString line;
            for (size_t i = 0; i < tail.size(); ++i) {
                if (tail[i] == '\n') {
                    tree->AppendItem(tomb, line);
                    line.Clear();
                } else line += tail[i];
            }
            if (!line.IsEmpty()) tree->AppendItem(tomb, line);
        }

        if (!r.raw_payload.empty()) {
            wxTreeItemId raw = tree->AppendItem(root, "Raw payload");
            tree->AppendItem(raw,
                wxString::Format("size: %zu bytes", r.raw_payload.size()));
            wxString dump = hex_dump(r.raw_payload);
            wxString line;
            for (size_t i = 0; i < dump.size(); ++i) {
                if (dump[i] == '\n') {
                    tree->AppendItem(raw, line);
                    line.Clear();
                } else line += dump[i];
            }
        }

        tree->Expand(root);
        tree->Expand(hdr);
        tree->Expand(subj);
    }
};


TracePanel::TracePanel(wxWindow* parent) : PanelBase(parent) {
    impl_ = new TracePanelImpl();

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* top = new wxBoxSizer(wxHORIZONTAL);
    top->Add(new wxStaticText(this, wxID_ANY, "filter:"),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    impl_->filter_input = new wxTextCtrl(this, wxID_ANY, "",
                                          wxDefaultPosition, wxDefaultSize,
                                          wxTE_PROCESS_ENTER);
    top->Add(impl_->filter_input, 1, wxRIGHT, 4);
    auto* clear_btn = new wxButton(this, wxID_ANY, "Clear");
    top->Add(clear_btn, 0);
    sizer->Add(top, 0, wxEXPAND | wxALL, 4);

    impl_->split = new wxSplitterWindow(this, wxID_ANY,
                                          wxDefaultPosition, wxDefaultSize,
                                          wxSP_LIVE_UPDATE | wxSP_3DBORDER);
    impl_->split->SetMinimumPaneSize(80);
    impl_->list = new TraceListCtrl(impl_->split, &impl_->ring,
                                      &impl_->visible, &impl_->mtx);
    impl_->tree = new wxTreeCtrl(impl_->split, wxID_ANY,
                                   wxDefaultPosition, wxDefaultSize,
                                   wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT);
    impl_->split->SplitHorizontally(impl_->list, impl_->tree, -260);
    sizer->Add(impl_->split, 1, wxEXPAND);
    SetSizer(sizer);

    list_ = impl_->list;   // panels.h's TracePanel::list_ retained
                            // for any code that still grabs it.

    impl_->filter_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->current_filter = impl_->filter_input->GetValue().ToStdString();
        impl_->apply_filter();
        const long n = static_cast<long>(impl_->visible.size());
        impl_->list->SetItemCount(n);
        impl_->list->Refresh();
    });

    clear_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        impl_->filter_input->SetValue("");
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->current_filter.clear();
        impl_->apply_filter();
        const long n = static_cast<long>(impl_->visible.size());
        impl_->list->SetItemCount(n);
        impl_->list->Refresh();
    });

    impl_->list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& evt) {
        const long idx = evt.GetIndex();
        EventRow row_copy;
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            if (idx < 0 ||
                static_cast<size_t>(idx) >= impl_->visible.size()) return;
            const size_t ring_idx = impl_->visible[idx];
            if (ring_idx >= impl_->ring.size()) return;
            row_copy = impl_->ring[ring_idx];
        }
        impl_->rebuild_tree(row_copy);
    });
}


void TracePanel::on_frame(const std::string& machine_name,
                            uint16_t tag,
                            const std::string& payload) {
    if (tag != kTagEvent) return;
    if (!impl_) return;

    system_supervisor::SupervisionEvent ev;
    if (!ev.ParseFromString(payload)) return;

    EventRow r;
    r.t_ms_local      = static_cast<int64_t>(ev.timestamp_ms());
    r.machine         = machine_name;
    r.kind            = ev.kind();
    r.child           = ev.child_name();
    r.parent          = ev.supervisor_name();
    r.pid             = ev.pid();
    r.exit_code       = ev.exit_code();
    r.strategy        = ev.strategy();
    r.tombstone_path  = ev.tombstone_path();
    r.detail          = ev.detail();
    r.raw_payload     = payload;

    long new_count = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->ring.push_back(std::move(r));
        while (impl_->ring.size() > kRingCapacity) impl_->ring.pop_front();
        impl_->apply_filter();
        new_count = static_cast<long>(impl_->visible.size());
    }
    impl_->list->SetItemCount(new_count);
    impl_->list->Refresh();
    if (new_count > 0) impl_->list->EnsureVisible(new_count - 1);
}

}  // namespace sup_gui
