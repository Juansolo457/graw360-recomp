// grawmp - gr3xenon_release.exe, Red Storm's GR3 engine: Instant Action, co-op and
// multiplayer. Host code is shared with the front end
// (../port/src, see graw_app.h).

#include "generated/gr3xenon_release/grawmp_init.h"

#define GRAW_APP_NAME "grawmp"
#define GRAW_XEX_IMAGE "game:\\GR3Xenon_Release.exe"
#include "graw_app.h"

REX_DEFINE_APP(grawmp, GrawApp::Create)
