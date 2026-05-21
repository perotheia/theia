// supervisor-gui — entry point.
//
// Standard wxApp boilerplate. The interesting work happens in
// MainFrame. We initialise libprotobuf's runtime once so subsequent
// SerializeAsString / ParseFromString calls don't lazy-init in panels.

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
