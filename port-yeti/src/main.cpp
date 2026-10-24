// grawyeti - yeti_final_xenon.exe, Ubisoft Paris's Yeti engine: the campaign. Host code is shared
// with the front end (../port/src, see graw_app.h).

#include "generated/yeti_final_xenon/grawyeti_init.h"

#define GRAW_APP_NAME "grawyeti"
#define GRAW_MODE_TITLE "Campaign"
#define GRAW_XEX_IMAGE "game:\\Yeti_Final_Xenon.exe"
#include "graw_app.h"

REX_DEFINE_APP(grawyeti, GrawApp::Create)
