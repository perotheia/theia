// supervisor-gui — entry point.
//
// Standard wxApp boilerplate. With the TCP feed removed (phase 0), the
// GUI starts without an active connection — the wxNotebook of panels
// shows empty/last-known state until services/com (phase 2) lands and
// the GUI is wired to its gRPC stub.

#include "sup_gui/main_frame.h"

#include <google/protobuf/stubs/common.h>
#include <wx/app.h>
#include <wx/wx.h>

namespace {

class SupervisorGuiApp : public wxApp {
public:
    bool OnInit() override {
        GOOGLE_PROTOBUF_VERIFY_VERSION;
        SetAppName("supervisor-gui");
        auto* frame = new sup_gui::MainFrame();
        frame->Show(true);
        return true;
    }

    int OnExit() override {
        google::protobuf::ShutdownProtobufLibrary();
        return 0;
    }
};

}  // namespace

wxIMPLEMENT_APP(SupervisorGuiApp);
