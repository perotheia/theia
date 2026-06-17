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

// OTP observer_app_wx-style layout: depth → X column (tree grows RIGHT, so it
// never overflows horizontally — depth is shallow), siblings stack DOWN (Y, the
// scrollable axis). A parent is vertically CENTERED against its children's span
// — it does NOT span their combined width (the old top-down look). Rounded
// selectable boxes; state drives colour.
constexpr int kBoxPadX     = 12;
constexpr int kBoxPadY     = 6;
constexpr int kColGap      = 36;   // horizontal gap between depth columns
constexpr int kRowGap      = 10;   // vertical gap between sibling boxes
constexpr int kMachineGap  = 40;
constexpr int kMargin      = 16;
constexpr double kRadius   = 7.0;  // rounded-box corner radius

// NodeFlag bits (mirror platform/supervisor: w.flags).
constexpr uint32_t kFlagCoreDumped = 1u;   // bit0 — crashed (fatal signal + core)
constexpr uint32_t kFlagDegraded   = 2u;   // bit1 — restart-thrashing

struct Node {
    std::string name;
    std::string strategy;       // supervisor only
    int         kind = 0;       // 0 = worker, 1 = supervisor
    int         pid  = -1;
    int         state = 0;
    uint32_t    restart_count = 0;
    int         last_exit_code = 0;
    uint32_t    flags = 0;
    std::string parent_name;

    std::vector<Node*> children;  // populated after building lookup

    // Layout outputs.
    int x = 0, y = 0;
    int w = 0, h = 0;
};

// A laid-out box, cached each paint for click → node mapping. Carries just the
// fields a click handler needs (no Node* — the tree store is rebuilt per paint).
struct HitBox {
    std::string machine;
    std::string name;
    int         kind = 0;     // 0 worker, 1 supervisor
    int         pid  = -1;
    uint32_t    flags = 0;    // CORE_DUMPED / DEGRADED (for menu gating)
    wxRect      rect;
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
        n->restart_count  = c.restart_count();
        n->last_exit_code = c.last_exit_code();
        n->flags          = c.flags();
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

// The label drawn in a box: name + compact badges. ↻N for restarts, ✗ for a
// crashed (CORE_DUMPED) node — so the breadth of a tree reads at a glance.
std::string box_label(const Node* n) {
    std::string s = n->name;
    // Crash / restart badges ride on the PROCESS row (kind 0) — that's where the
    // pid + per-process flags live. (kind 1 = supervisor, kind 2 = node/thread.)
    if (n->kind == 0) {
        if (n->flags & kFlagCoreDumped) s += "  ✗";        // ✗ crashed
        if (n->restart_count > 0)
            s += "  ↻" + std::to_string(n->restart_count); // ↻N restarts
    }
    return s;
}

// ---- OTP observer_app_wx layout: column-per-depth, vertical sibling flow ----
//
// PASS 1 (size): set each box's w/h from its label.
void measure(Node* n, wxDC& dc) {
    wxSize t = dc.GetTextExtent(wxString::FromUTF8(box_label(n).c_str()));
    n->w = t.GetWidth()  + 2 * kBoxPadX;
    n->h = t.GetHeight() + 2 * kBoxPadY;
    for (auto* c : n->children) measure(c, dc);
}

// `col_x[d]` = the LEFT x of depth-column d. Built from the widest box per
// depth so every column is just wide enough — the tree grows rightward by
// DEPTH (shallow), never by breadth.
void measure_columns(const Node* n, int depth, std::vector<int>& col_w) {
    if (static_cast<int>(col_w.size()) <= depth) col_w.resize(depth + 1, 0);
    col_w[depth] = std::max(col_w[depth], n->w);
    for (auto* c : n->children) measure_columns(c, depth + 1, col_w);
}

// PASS 2 (place): assign x by depth-column, y by a running cursor. A leaf
// consumes one row (advances `y`); a parent is CENTERED vertically against the
// span of its children (NOT stretched across their width). Returns the node's
// vertical centre so the parent can centre on its children's mean.
int place(Node* n, int depth, const std::vector<int>& col_x, int& y) {
    n->x = col_x[depth];
    if (n->children.empty()) {
        n->y = y;
        int centre = y + n->h / 2;
        y += n->h + kRowGap;
        return centre;
    }
    int first_c = 0, last_c = 0;
    for (size_t i = 0; i < n->children.size(); ++i) {
        int cc = place(n->children[i], depth + 1, col_x, y);
        if (i == 0) first_c = cc;
        last_c = cc;
    }
    int centre = (first_c + last_c) / 2;     // midway between first/last child
    n->y = centre - n->h / 2;                // parent centred on that span
    return centre;
}

// State → box colours. Crash (red) and restart (amber) are the signals the
// user asked to surface; degraded (orange border) and stopped (grey) too.
void box_colours(const Node* n, wxColour& fill, wxColour& border,
                 bool selected) {
    if (n->kind == 1) {                       // supervisor
        fill = wxColour(224, 234, 247);       // light blue
        border = wxColour(120, 140, 170);
    } else if (n->pid <= 0) {                  // worker, not running
        fill = wxColour(228, 228, 228);       // grey
        border = wxColour(150, 150, 150);
    } else if (n->flags & kFlagCoreDumped) {   // crashed
        fill = wxColour(250, 205, 205);       // red
        border = wxColour(190, 70, 70);
    } else if (n->restart_count > 0) {         // has restarted
        fill = wxColour(252, 236, 200);       // amber
        border = wxColour(200, 150, 60);
    } else if (n->kind == 0) {                 // PROCESS (running, clean) —
        fill = wxColour(214, 234, 240);       // teal-ish: distinct from the
        border = wxColour(90, 150, 165);      // green leaf NODES it contains
    } else {                                   // node (kind 2), running clean
        fill = wxColour(232, 245, 233);       // light green
        border = wxColour(120, 170, 120);
    }
    if (n->flags & kFlagDegraded)              // restart-thrashing: orange edge
        border = wxColour(225, 120, 0);
    if (selected) border = wxColour(0, 90, 200);  // selection: blue edge
}

void draw_node(wxDC& dc, const Node* n, const std::string& selected) {
    // Right-angle links to children, drawn first (under the boxes). From this
    // box's RIGHT edge → a vertical bus in the column gap → each child's LEFT.
    if (!n->children.empty()) {
        dc.SetPen(wxPen(wxColour(140, 140, 140), 1));
        int rx = n->x + n->w;                       // parent right edge
        int rcy = n->y + n->h / 2;                  // parent centre-y
        int busx = rx + kColGap / 2;                // vertical bus x
        dc.DrawLine(rx, rcy, busx, rcy);
        for (auto* c : n->children) {
            int ccy = c->y + c->h / 2;
            dc.DrawLine(busx, rcy, busx, ccy);      // bus segment
            dc.DrawLine(busx, ccy, c->x, ccy);      // into the child
        }
    }

    const bool is_sel = (!selected.empty() && n->name == selected);
    wxColour fill, border;
    box_colours(n, fill, border, is_sel);
    dc.SetBrush(wxBrush(fill));
    dc.SetPen(wxPen(border, is_sel ? 2 : 1));
    dc.DrawRoundedRectangle(n->x, n->y, n->w, n->h, kRadius);
    dc.SetTextForeground(*wxBLACK);
    // FromUTF8: box_label may carry multibyte badge glyphs (↻ ✗). A bare
    // std::string→wxString goes through the C locale and yields an EMPTY string
    // on a non-ASCII byte — which is why a restarted process (label "p1  ↻2")
    // drew as a blank pill while a clean one ("sm") rendered fine.
    wxString label = wxString::FromUTF8(box_label(n).c_str());
    dc.DrawText(label,
                n->x + (n->w - dc.GetTextExtent(label).GetWidth()) / 2,
                n->y + kBoxPadY);

    for (auto* c : n->children) draw_node(dc, c, selected);
}

// Lay a root tree out at (origin_x, origin_y); returns its bounding box.
// `y` is advanced by place() to the bottom of the subtree.
wxRect layout_tree(Node* root, wxDC& dc, int origin_x, int origin_y) {
    measure(root, dc);
    std::vector<int> col_w;
    measure_columns(root, 0, col_w);
    std::vector<int> col_x(col_w.size());
    int x = origin_x;
    for (size_t d = 0; d < col_w.size(); ++d) {
        col_x[d] = x;
        x += col_w[d] + kColGap;
    }
    int y = origin_y;
    place(root, 0, col_x, y);
    return wxRect(origin_x, origin_y, x - origin_x, y - origin_y);
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
    // GUI-3 menu callbacks (see panels.h for semantics). Each returns a status
    // line the canvas pushes to the frame's status bar (or "" for none).
    using LogLevelCallback = std::function<std::string(
        const std::string&, const std::string&, const std::string&)>;
    using RestartCallback = std::function<std::string(
        const std::string&, const std::string&)>;
    using TombstoneCallback = std::function<std::string(
        const std::string&, const std::string&)>;
    using KillCallback = std::function<std::string(
        const std::string&, const std::string&)>;
    using GetLogLevelCallback = std::function<std::string(
        const std::string&, const std::string&)>;

    explicit ApplicationsCanvas(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxVSCROLL | wxHSCROLL) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(*wxWHITE);
        SetScrollRate(16, 16);
        Bind(wxEVT_PAINT, &ApplicationsCanvas::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &ApplicationsCanvas::on_left_down, this);
        Bind(wxEVT_RIGHT_DOWN, &ApplicationsCanvas::on_right_down, this);
    }

    void set_snapshot(const std::string& machine,
                      system_supervisor::TreeSnapshot snap) {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshots_[machine] = std::move(snap);
        Refresh(false);
    }

    // Scope to ONE machine: drop every other machine's snapshot so the panel
    // renders a single machine's supervision tree (not one block per machine).
    void keep_only(const std::string& keep) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = snapshots_.begin(); it != snapshots_.end();)
            it = (it->first == keep) ? std::next(it) : snapshots_.erase(it);
        Refresh(false);
    }

    void set_configure_callback(ConfigureCallback cb) {
        configure_cb_ = std::move(cb);
    }
    void set_log_level_callback(LogLevelCallback cb) {
        log_level_cb_ = std::move(cb);
    }
    void set_restart_callback(RestartCallback cb) {
        restart_cb_ = std::move(cb);
    }
    void set_tombstone_callback(TombstoneCallback cb) {
        tombstone_cb_ = std::move(cb);
    }
    void set_kill_callback(KillCallback cb) {
        kill_cb_ = std::move(cb);
    }
    void set_get_log_level_callback(GetLogLevelCallback cb) {
        get_log_level_cb_ = std::move(cb);
    }

private:
    void on_paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        DoPrepareDC(dc);
        dc.Clear();

        std::lock_guard<std::mutex> lk(mtx_);
        hit_boxes_.clear();   // rebuilt below for click → node mapping

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

            // Lay out + draw each root tree, stacking DOWN the page (each tree
            // grows RIGHT by depth, so trees never collide horizontally; new
            // trees go below the previous). Cache hit boxes for click mapping.
            for (auto* r : roots) {
                wxRect bb = layout_tree(r, dc, kMargin, y_cursor);
                draw_node(dc, r, selected_for(m));
                // Flatten every box into the per-machine hit cache.
                std::vector<Node*> stack{r};
                while (!stack.empty()) {
                    Node* n = stack.back(); stack.pop_back();
                    max_w = std::max(max_w, n->x + n->w);
                    hit_boxes_.push_back(HitBox{
                        m, n->name, n->kind, n->pid, n->flags,
                        wxRect(n->x, n->y, n->w, n->h)});
                    for (auto* c : n->children) stack.push_back(c);
                }
                y_cursor = bb.GetBottom() + kRowGap;
            }
            y_cursor += kMachineGap;
        }

        // Update virtual size for scrolling.
        SetVirtualSize(max_w + kMargin, y_cursor + kMargin);
    }

    // Box the click landed in (from the cache the last paint built), or null.
    const HitBox* box_at(const wxPoint& p) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& b : hit_boxes_)
            if (b.rect.Contains(p)) return &b;
        return nullptr;
    }

    // Left-click SELECTS the node under the cursor (highlight). The selection
    // is what a following right-click / future detail panel acts on.
    void on_left_down(wxMouseEvent& evt) {
        wxPoint p = CalcUnscrolledPosition(evt.GetPosition());
        const HitBox* b = box_at(p);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (b) selected_[b->machine] = b->name;
            else   selected_.clear();   // click on empty space clears
        }
        Refresh(false);
        evt.Skip();
    }

    // Menu item IDs (local to this canvas).
    enum {
        kMenuTrace = wxID_HIGHEST + 1,
        kMenuLogBase,          // 5 contiguous: TRACE/DEBUG/INFO/WARN/ERROR
        kMenuRestart = kMenuLogBase + 8,
        kMenuTombstone,
        kMenuKill,
    };
    static const char* log_level_name(int idx) {
        static const char* kL[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
        return (idx >= 0 && idx < 5) ? kL[idx] : "INFO";
    }
    // Tree row kind: 1 = supervisor, 0 = process (proc), 2 = node (thread).

    // Right-click on a box → select it + pop a KIND-SPECIFIC context menu on the
    // selectable hit cache:
    //   supervisor (1) : Restart subtree.
    //   process    (0) : Kill (restart); Download tombstone if it cored.
    //   node       (2) : Log level ▸ (with the CURRENT level checked) +
    //                    Configure trace… (kinds as checkboxes).
    void on_right_down(wxMouseEvent& evt) {
        wxPoint p = CalcUnscrolledPosition(evt.GetPosition());
        const HitBox* b = box_at(p);
        if (!b) { evt.Skip(); return; }
        // Copy out the fields we need before unlocking — the cache is rebuilt
        // on the next paint, so `b` must not outlive this scope.
        std::string machine, name;
        int kind; uint32_t flags;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            selected_[b->machine] = b->name;
            machine = b->machine; name = b->name;
            kind = b->kind; flags = b->flags;
        }
        Refresh(false);

        wxMenu menu;
        if (kind == 1) {                              // supervisor
            menu.Append(kMenuRestart,
                        wxString::Format("Restart subtree \"%s\"", name.c_str()));
        } else if (kind == 0) {                       // process
            menu.Append(kMenuKill,
                        wxString::Format("Kill \"%s\" (restart)", name.c_str()));
            if (flags & kFlagCoreDumped) {
                menu.AppendSeparator();
                menu.Append(kMenuTombstone, "Download tombstone...");
            }
        } else {                                      // node (kind 2)
            // Log level ▸ with a RADIO check on the node's CURRENT level.
            std::string cur;
            if (get_log_level_cb_) cur = get_log_level_cb_(machine, name);
            auto* log_menu = new wxMenu;
            for (int i = 0; i < 5; ++i) {
                wxMenuItem* it = log_menu->AppendCheckItem(
                    kMenuLogBase + i, log_level_name(i));
                if (cur == log_level_name(i)) it->Check(true);
            }
            menu.AppendSubMenu(log_menu, "Log level");
            menu.Append(kMenuTrace, "Configure trace...");
        }

        const int sel = GetPopupMenuSelectionFromUser(menu, evt.GetPosition());
        if (sel == wxID_NONE) return;

        if (sel == kMenuTrace) {
            show_trace_dialog(machine, name);
        } else if (sel >= kMenuLogBase && sel < kMenuLogBase + 5) {
            if (log_level_cb_) {
                std::string s = log_level_cb_(machine, name,
                                              log_level_name(sel - kMenuLogBase));
                if (!s.empty()) wxLogStatus("%s", s.c_str());
            }
        } else if (sel == kMenuRestart) {
            if (restart_cb_) {
                std::string s = restart_cb_(machine, name);
                if (!s.empty()) wxLogStatus("%s", s.c_str());
            }
        } else if (sel == kMenuKill) {
            if (kill_cb_) {
                std::string s = kill_cb_(machine, name);
                if (!s.empty()) wxLogStatus("%s", s.c_str());
            }
        } else if (sel == kMenuTombstone) {
            if (tombstone_cb_) {
                std::string s = tombstone_cb_(machine, name);
                if (!s.empty()) wxLogStatus("%s", s.c_str());
            }
        }
    }

    // Trace dialog — a CHECKBOX per trace kind, like the log-level submenu but
    // multi-select. The trace filter is a per-node BITMASK of kinds; checking a
    // box enables that kind, unchecking removes it. No message-type field (the
    // TraceControlPush carries only kind+enabled — msg_type was a vestige) and
    // no global Enabled box (unchecking every kind = trace off).
    void show_trace_dialog(const std::string& machine,
                           const std::string& node_name) {
        // (ordinal, label) — STATEM only matters for statem nodes but is
        // harmless elsewhere. OTHER(0) = the "all kinds" catch-all.
        struct K { uint32_t ord; const char* label; };
        static const K kKinds[] = {
            {1, "CAST_OUT"}, {2, "CAST_IN"}, {3, "CALL_OUT"},
            {4, "CALL_IN"},  {5, "STATEM"},
        };

        wxDialog dlg(this, wxID_ANY,
                     wxString::Format("Trace: %s", node_name.c_str()),
                     wxDefaultPosition, wxSize(300, 280));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(new wxStaticText(&dlg, wxID_ANY,
            wxString::Format("Enable trace kinds for \"%s\":", node_name.c_str())),
            0, wxALL, 10);

        std::vector<wxCheckBox*> boxes;
        for (const auto& k : kKinds) {
            auto* cb = new wxCheckBox(&dlg, wxID_ANY, k.label);
            outer->Add(cb, 0, wxLEFT | wxRIGHT | wxTOP, 12);
            boxes.push_back(cb);
        }

        auto* buttons = dlg.CreateButtonSizer(wxOK | wxCANCEL);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        dlg.SetSizer(outer);
        outer->Layout();

        if (dlg.ShowModal() != wxID_OK) return;
        if (!configure_cb_) return;
        // One ConfigureTrace per kind: enabled = the box's state (checked adds
        // the kind to the node's mask, unchecked removes it). msg_type unused.
        for (size_t i = 0; i < boxes.size(); ++i) {
            configure_cb_(machine, node_name, /*msg_type=*/"",
                          boxes[i]->IsChecked(), kKinds[i].ord);
        }
    }

    // Selected node name in `machine` whose box should be highlighted, "" none.
    std::string selected_for(const std::string& machine) const {
        auto it = selected_.find(machine);
        return it == selected_.end() ? std::string{} : it->second;
    }

    mutable std::mutex mtx_;   // mutable: box_at()/selected_for() lock it const
    std::map<std::string, system_supervisor::TreeSnapshot> snapshots_;
    ConfigureCallback configure_cb_;
    LogLevelCallback  log_level_cb_;
    RestartCallback   restart_cb_;
    TombstoneCallback tombstone_cb_;
    KillCallback      kill_cb_;
    GetLogLevelCallback get_log_level_cb_;
    // Per-paint hit cache (rect → node), rebuilt every on_paint, used by
    // left/right click to map a point to a node without re-laying-out.
    std::vector<HitBox> hit_boxes_;
    // Per-machine selected node name (highlighted box).
    std::map<std::string, std::string> selected_;
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

void ApplicationsPanel::set_machine_filter(const std::string& keep) {
    if (canvas_) canvas_->keep_only(keep);   // one machine's tree only
}

void ApplicationsPanel::set_configure_trace_callback(
    ConfigureTraceCallback cb) {
    canvas_->set_configure_callback(std::move(cb));
}

void ApplicationsPanel::set_log_level_callback(LogLevelCallback cb) {
    canvas_->set_log_level_callback(std::move(cb));
}

void ApplicationsPanel::set_restart_callback(RestartCallback cb) {
    canvas_->set_restart_callback(std::move(cb));
}

void ApplicationsPanel::set_tombstone_callback(TombstoneCallback cb) {
    canvas_->set_tombstone_callback(std::move(cb));
}

void ApplicationsPanel::set_kill_callback(KillCallback cb) {
    canvas_->set_kill_callback(std::move(cb));
}

void ApplicationsPanel::set_get_log_level_callback(GetLogLevelCallback cb) {
    canvas_->set_get_log_level_callback(std::move(cb));
}

}  // namespace sup_gui
