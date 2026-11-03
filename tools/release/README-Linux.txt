GRAW 360 Recompiled for Linux and the Steam Deck
================================================

The Xbox 360 version of Tom Clancy's Ghost Recon Advanced Warfighter (2006), statically recompiled
to run natively on Linux: no Proton, no Wine. A work in progress: the campaign plays but hasn't been
played through end to end yet.

This package holds the game's programs only. It contains no game files: you need your own Xbox 360
disc, and the game reads its files from it.

What you need
  - A Steam Deck. That's the only machine this build has been tested on. An x86-64 Linux PC with a
    Vulkan 1.3 GPU, a CPU with AVX2 and FMA, glibc 2.35 or newer and a libstdc++ with GLIBCXX_3.4.32
    (GCC 13's; SteamOS has it) may run it too, at the same 1280x720 and 30 fps.
  - Your own disc of the game for Xbox 360, USA/Europe multilingual, title 555307D5, as a disc image
    (ISO). A PC drive can't read Xbox 360 discs directly: you need an ISO made from your disc.
  - Python 3, to unpack the disc image (SteamOS has it).
  - About 5 GB of free space for the unpacked disc.

Setup (on the Deck: Desktop Mode)
  1. Extract this zip where you keep games, for example /home/deck/Games.
  2. Unpack your disc image into the game folder. Open a terminal in the folder this README is in,
     the one with xdvdfs.py, and run:
       python3 -I xdvdfs.py "<your GRAW ISO>" extract game
     Don't use an archiver such as Ark, WinRAR or 7-Zip for this. They can't unpack an Xbox 360 disc
     image. The game folder should then hold default.xex and Yeti_Final_Xenon.exe directly, not in
     another folder inside it.
  3. Run linux/graw-linux.sh. On the Deck, add it to Steam instead:
       - Steam > Games > "Add a Non-Steam Game to My Library" > Browse > set the file type to
         "All Files" > linux/graw-linux.sh > Add.
       - Right-click the new entry > Properties > Compatibility: leave "Force the use of a specific
         Steam Play compatibility tool" UNTICKED. This is a Linux program.
       - Rename it (e.g. "GRAW") and go back to Game Mode.
  4. In Game Mode, set the frame limit to 30 in the Quick Access menu's Performance page. That's how
     the 30 fps below was measured.

Frame rate on the Deck
  The campaign runs at the 360's 30 fps. Measured in one 43-minute session: 29.9 fps average, with
  0.64% of frames over 40 ms. Very heavy moments (2,500 draws and up) still dip.

  The first time you see an area or effect, its shaders are built as it appears, and that can hitch.
  They're kept in linux/cache and built before the first frame from then on, so it gets smoother as
  you play. A later start of the campaign can spend some time on this before the first frame; it
  stops after 2 minutes at most.

Split-screen
  Instant Action's Multiplayer > Local Play gives each controller its own player, up to 4.

Logs
  linux/logs holds each run's log, a frame-time log and a sensor log (clocks, power, temperatures).
  They stay on your machine; nothing is sent anywhere. A long session writes about 60 MB. Delete old
  ones whenever you like, between runs.

If it doesn't start
  Read the newest linux/logs/system-<time>.txt. A "LACKS GLIBCXX" or "not found" line names the
  problem; "isn't the supported version" means a different disc from the one above.
  Report problems with the newest files from linux/logs:
  https://github.com/Juansolo457/graw360-recomp/issues

Licenses
  LICENSE, NOTICE and LICENSES/ are this project's terms; third-party-licenses/ holds the libraries'.
  These programs are built from https://github.com/Juansolo457/graw360-recomp (VERSION.txt names the
  commit) and ReXGlue, including its copy of FFmpeg (LGPL 2.1 or later, in librexruntimerd.so):
  https://github.com/rexglue/rexglue-sdk

Unofficial. Ghost Recon is a trademark of Ubisoft Entertainment. Not affiliated with Ubisoft, Red
Storm Entertainment, Ubisoft Paris or Microsoft.
