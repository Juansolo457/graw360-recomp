// grawsp - the campaign, gr3xenon_release.exe. Host code is shared with the front end
// (../port/src, see graw_app.h).

#include "generated/gr3xenon_release/grawsp_init.h"

#define GRAW_APP_NAME "grawsp"
#define GRAW_XEX_IMAGE "game:\\GR3Xenon_Release.exe"
#include "graw_app.h"

REX_DEFINE_APP(grawsp, GrawApp::Create)
