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

#include "TreeSnapshot.pb.h"

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
build_tree(const services::supervisor::TreeSnapshot& snap,
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
    explicit ApplicationsCanvas(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxVSCROLL | wxHSCROLL) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(*wxWHITE);
        SetScrollRate(16, 16);
        Bind(wxEVT_PAINT, &ApplicationsCanvas::on_paint, this);
    }

    void set_snapshot(const std::string& machine,
                      services::supervisor::TreeSnapshot snap) {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshots_[machine] = std::move(snap);
        Refresh(false);
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

    std::mutex mtx_;
    std::map<std::string, services::supervisor::TreeSnapshot> snapshots_;
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
    services::supervisor::TreeSnapshot snap;
    if (!snap.ParseFromString(payload)) return;
    canvas_->set_snapshot(machine_name, std::move(snap));
}

}  // namespace sup_gui
