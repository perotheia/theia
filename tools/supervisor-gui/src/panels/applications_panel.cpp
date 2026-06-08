// Applications panel — observer_app_wx.erl analogue.
//
// Custom-drawn supervisor diagram: each SupervisorNode renders as a
// labelled rectangle, each child a smaller rectangle below it, connected
// by a vertical line and an L-shaped horizontal stub. Box colours encode
// node kind (supervisor vs worker) and live state (running vs stopped).
//
// Layout algorithm:
//
//   1. Build a hierarchy from the flat TreeSnapshot.children list using
//      parent_name links.
//   2. Recursive bottom-up width assignment: leaf width = label width
//      + padding; supervisor width = max(self, sum(children) + gaps).
//   3. Recursive top-down x/y placement.
//   4. Render: rectangles + connecting lines.
//
// Multi-machine: each connected machine gets its own diagram, laid out
// in a vertical stack with a divider line and the machine name as a
// caption.

#include "sup_gui/panels.h"

#include "supervisor.pb.h"

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace sup_gui {

namespace {

constexpr int kBoxPadX     = 12;
constexpr int kBoxPadY     = 6;
constexpr int kHGap        = 16;
constexpr int kVGap        = 32;
constexpr int kMachineGap  = 48;
constexpr int kMargin      = 16;

struct Node {
    std::string name;
    std::string strategy;       // supervisor only
    int         kind = 0;       // 0 = worker, 1 = supervisor
    int         pid  = -1;
    int         state = 0;
    std::string parent_name;

    std::vector<Node*> children;  // populated after building lookup

    // Layout outputs.
    int x = 0, y = 0;
    int w = 0, h = 0;
};

// Build hierarchy from flat list. Returns root nodes (parent_name="").
std::vector<std::unique_ptr<Node>>
build_tree(const system_supervisor::TreeSnapshot& snap,
           std::vector<Node*>& roots_out) {
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.reserve(snap.children_size());
    std::map<std::string, Node*> by_name;

    for (const auto& c : snap.children()) {
        auto n = std::unique_ptr<Node>(new Node());
        n->name        = c.name();
        n->kind        = static_cast<int>(c.kind());
        n->pid         = c.pid();
        n->state       = static_cast<int>(c.state());
        n->parent_name = c.parent_name();
        n->strategy    = c.strategy();
        by_name[n->name] = n.get();
        nodes.push_back(std::move(n));
    }
    for (auto& up : nodes) {
        if (up->parent_name.empty()) {
            roots_out.push_back(up.get());
        } else {
            auto it = by_name.find(up->parent_name);
            if (it != by_name.end()) {
                it->second->children.push_back(up.get());
            } else {
                roots_out.push_back(up.get());
            }
        }
    }
    return nodes;
}

// Width/height pass: bottom-up. Returns the box width for this node.
void layout_sizes(Node* n, wxDC& dc) {
    wxSize text = dc.GetTextExtent(n->name);
    int box_w = text.GetWidth()  + 2 * kBoxPadX;
    int box_h = text.GetHeight() + 2 * kBoxPadY;
    n->h = box_h;

    if (n->children.empty()) {
        n->w = box_w;
        return;
    }
    int child_total = 0;
    for (auto* c : n->children) {
        layout_sizes(c, dc);
        child_total += c->w;
    }
    child_total += static_cast<int>(n->children.size() - 1) * kHGap;
    n->w = std::max(box_w, child_total);
}

// Place: top-down. Sets x/y on this node and recurses.
void layout_place(Node* n, int x, int y) {
    n->x = x;
    n->y = y;
    if (n->children.empty()) return;

    int child_total = 0;
    for (auto* c : n->children) child_total += c->w;
    child_total += static_cast<int>(n->children.size() - 1) * kHGap;
    int cx = x + (n->w - child_total) / 2;
    int cy = y + n->h + kVGap;
    for (auto* c : n->children) {
        layout_place(c, cx, cy);
        cx += c->w + kHGap;
    }
}

void draw_node(wxDC& dc, const Node* n) {
    wxColour box_fill =
        n->kind == 1 ? wxColour(225, 235, 245)   // supervisor (light blue)
                     : wxColour(245, 245, 235);  // worker (light yellow)
    if (n->kind == 0 && n->pid <= 0) {
        box_fill = wxColour(250, 220, 220);      // worker not running (red-ish)
    }
    dc.SetBrush(wxBrush(box_fill));
    dc.SetPen(wxPen(*wxBLACK, 1));
    dc.DrawRectangle(n->x, n->y, n->w, n->h);
    dc.DrawText(n->name,
                n->x + (n->w - dc.GetTextExtent(n->name).GetWidth()) / 2,
                n->y + kBoxPadY);

    // Connector lines to children: vertical drop from this box centre,
    // horizontal hop, vertical drop to each child top.
    if (n->children.empty()) return;
    int cx = n->x + n->w / 2;
    int by = n->y + n->h;
    int half = kVGap / 2;
    int hy = by + half;
    dc.SetPen(wxPen(*wxBLACK, 1));
    dc.DrawLine(cx, by, cx, hy);
    int lx = n->children.front()->x + n->children.front()->w / 2;
    int rx = n->children.back()->x  + n->children.back()->w / 2;
    dc.DrawLine(lx, hy, rx, hy);
    for (auto* c : n->children) {
        int ccx = c->x + c->w / 2;
        dc.DrawLine(ccx, hy, ccx, c->y);
    }
    for (auto* c : n->children) draw_node(dc, c);
}

}  // namespace

// ---------------------------------------------------------------------------
// Panel implementation
// ---------------------------------------------------------------------------

class ApplicationsCanvas : public wxScrolledWindow {
public:
    // configure_cb: called when the user clicks Apply in the right-click
    // ConfigureTrace dialog. Wired to GrpcClient::configure_trace on the
    // matching machine. (machine, node, msg_type, enabled, kind).
    using ConfigureCallback = std::function<void(
        const std::string& /*machine*/, const std::string& /*node*/,
        const std::string& /*msg_type*/, bool /*enabled*/, uint32_t /*kind*/)>;

    explicit ApplicationsCanvas(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxVSCROLL | wxHSCROLL) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(*wxWHITE);
        SetScrollRate(16, 16);
        Bind(wxEVT_PAINT, &ApplicationsCanvas::on_paint, this);
        Bind(wxEVT_RIGHT_DOWN, &ApplicationsCanvas::on_right_down, this);
    }

    void set_snapshot(const std::string& machine,
                      system_supervisor::TreeSnapshot snap) {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshots_[machine] = std::move(snap);
        Refresh(false);
    }

    void set_configure_callback(ConfigureCallback cb) {
        configure_cb_ = std::move(cb);
    }

private:
    void on_paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        DoPrepareDC(dc);
        dc.Clear();

        std::lock_guard<std::mutex> lk(mtx_);

        // Build trees per machine, in deterministic order.
        std::vector<std::string> machines;
        machines.reserve(snapshots_.size());
        for (const auto& kv : snapshots_) machines.push_back(kv.first);
        std::sort(machines.begin(), machines.end());

        int y_cursor = kMargin;
        int max_w    = 0;

        for (const auto& m : machines) {
            const auto& snap = snapshots_[m];
            std::vector<Node*> roots;
            auto store = build_tree(snap, roots);
            if (roots.empty()) continue;

            // Caption: machine name.
            wxFont caption = dc.GetFont();
            caption.SetWeight(wxFONTWEIGHT_BOLD);
            dc.SetFont(caption);
            dc.SetTextForeground(*wxBLACK);
            wxString cap = wxString::Format("machine: %s", m.c_str());
            dc.DrawText(cap, kMargin, y_cursor);
            y_cursor += dc.GetTextExtent(cap).GetHeight() + 4;
            // Restore font.
            caption.SetWeight(wxFONTWEIGHT_NORMAL);
            dc.SetFont(caption);

            // Layout each root tree side-by-side.
            int x_cursor = kMargin;
            int row_h    = 0;
            for (auto* r : roots) {
                layout_sizes(r, dc);
                layout_place(r, x_cursor, y_cursor);
                draw_node(dc, r);
                x_cursor += r->w + kHGap * 2;
                row_h     = std::max(row_h, r->h);
                // Track furthest right edge from this tree.
                std::vector<Node*> stack{r};
                while (!stack.empty()) {
                    Node* n = stack.back(); stack.pop_back();
                    max_w  = std::max(max_w,    n->x + n->w);
                    row_h  = std::max(row_h,    n->y + n->h - y_cursor);
                    for (auto* c : n->children) stack.push_back(c);
                }
            }
            y_cursor += row_h + kMachineGap;
        }

        // Update virtual size for scrolling.
        SetVirtualSize(max_w + kMargin, y_cursor + kMargin);
    }

    // #365 — Right-click on a node row opens a ConfigureTrace
    // dialog. The hit-test reuses the layout cache (last on_paint
    // pass) so the click→node mapping is exact. node_at_point()
    // returns the box the click landed in, or nullptr.
    void on_right_down(wxMouseEvent& evt) {
        wxPoint p = CalcUnscrolledPosition(evt.GetPosition());
        std::string machine, node_name, node_msg_default;
        bool is_worker = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Re-layout in a memory DC to compute hit boxes — cheaper
            // than caching them across paints since the snapshot map
            // changes rarely.
            wxMemoryDC dc;
            wxBitmap bmp(1, 1);
            dc.SelectObject(bmp);
            int y_cursor = kMargin;
            for (const auto& kv : snapshots_) {
                const auto& snap = kv.second;
                std::vector<Node*> roots;
                auto store = build_tree(snap, roots);
                if (roots.empty()) continue;
                wxString cap = wxString::Format("machine: %s",
                                                 kv.first.c_str());
                y_cursor += dc.GetTextExtent(cap).GetHeight() + 4;
                int x_cursor = kMargin;
                for (auto* r : roots) {
                    layout_sizes(r, dc);
                    layout_place(r, x_cursor, y_cursor);
                    std::vector<Node*> stack{r};
                    while (!stack.empty()) {
                        Node* n = stack.back(); stack.pop_back();
                        wxRect rect(n->x, n->y, n->w, n->h);
                        if (rect.Contains(p)) {
                            machine     = kv.first;
                            node_name   = n->name;
                            is_worker   = (n->kind == 0);
                            // For workers we don't know a default
                            // msg_type — user types it. For sup rows
                            // a Configure makes no sense; skip.
                        }
                        for (auto* c : n->children) stack.push_back(c);
                        x_cursor = std::max(x_cursor, n->x + n->w);
                    }
                    x_cursor += kHGap * 2;
                    y_cursor = std::max(y_cursor, r->y + r->h);
                }
                y_cursor += kMachineGap;
            }
        }
        if (machine.empty() || node_name.empty() || !is_worker) {
            evt.Skip();
            return;
        }
        show_configure_dialog(machine, node_name);
    }

    void show_configure_dialog(const std::string& machine,
                                const std::string& node_name) {
        wxDialog dlg(this, wxID_ANY, "Configure Trace",
                     wxDefaultPosition, wxSize(420, 220));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(new wxStaticText(&dlg, wxID_ANY,
            wxString::Format("Node:    %s    (machine: %s)",
                              node_name.c_str(), machine.c_str())),
            0, wxALL, 10);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(&dlg, wxID_ANY, "Message type:"),
                 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);
        auto* msg = new wxTextCtrl(&dlg, wxID_ANY, "SmStateMsg",
                                    wxDefaultPosition, wxSize(220, -1));
        row->Add(msg, 1, wxRIGHT, 10);
        outer->Add(row, 0, wxEXPAND);

        // Trace KIND — the dimension the supervisor's per-node filter keys on
        // (#403), the same one `rtdb trace <node> CAST_OUT` sets. Index maps to
        // the TraceKind ordinal: 0=all(OTHER), 1=CAST_OUT … 5=STATEM.
        auto* krow = new wxBoxSizer(wxHORIZONTAL);
        krow->Add(new wxStaticText(&dlg, wxID_ANY, "Trace kind:"),
                  0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);
        wxArrayString kinds;
        kinds.Add("ALL (every kind)");   // ordinal 0 (OTHER = catch-all)
        kinds.Add("CAST_OUT");           // 1
        kinds.Add("CAST_IN");            // 2
        kinds.Add("CALL_OUT");           // 3
        kinds.Add("CALL_IN");            // 4
        kinds.Add("STATEM");             // 5
        auto* kind_choice = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition,
                                          wxDefaultSize, kinds);
        kind_choice->SetSelection(0);
        krow->Add(kind_choice, 1, wxRIGHT, 10);
        outer->Add(krow, 0, wxEXPAND);

        auto* enable_box = new wxCheckBox(&dlg, wxID_ANY,
            "Enabled (uncheck to remove the filter)");
        enable_box->SetValue(true);
        outer->Add(enable_box, 0, wxALL, 10);

        auto* buttons = dlg.CreateButtonSizer(wxOK | wxCANCEL);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        dlg.SetSizer(outer);
        outer->Layout();

        if (dlg.ShowModal() != wxID_OK) return;
        std::string msg_type = std::string(msg->GetValue().mb_str());
        bool enabled = enable_box->IsChecked();
        const uint32_t kind =
            static_cast<uint32_t>(std::max(0, kind_choice->GetSelection()));
        if (configure_cb_) {
            configure_cb_(machine, node_name, msg_type, enabled, kind);
        }
    }

    std::mutex mtx_;
    std::map<std::string, system_supervisor::TreeSnapshot> snapshots_;
    ConfigureCallback configure_cb_;
};

ApplicationsPanel::ApplicationsPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer  = new wxBoxSizer(wxVERTICAL);
    canvas_      = new ApplicationsCanvas(this);
    sizer->Add(canvas_, 1, wxEXPAND);
    SetSizer(sizer);
}

void ApplicationsPanel::on_frame(const std::string& machine_name,
                                 uint16_t tag,
                                 const std::string& payload) {
    if (tag != 0x0003) return;  // only TreeSnapshot
    system_supervisor::TreeSnapshot snap;
    if (!snap.ParseFromString(payload)) return;
    canvas_->set_snapshot(machine_name, std::move(snap));
}

void ApplicationsPanel::set_configure_trace_callback(
    ConfigureTraceCallback cb) {
    canvas_->set_configure_callback(std::move(cb));
}

}  // namespace sup_gui
