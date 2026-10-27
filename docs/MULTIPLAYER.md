# Multiplayer

Instant Action and multiplayer are their own program, `grawmp` (`GR3Xenon_Release.exe`, `port-mp/`),
which the front end hands over to. It draws with ReXGlue's emulated renderer on every platform
(the native renderer is the campaign's only). This file collects what works; #35 is the wider
discovery (System Link, Xbox Live) and #45 the split-screen work.

## Local Play: split-screen (#45)

`grawmp`'s Multiplayer menu offers Xbox Live, System Link, Local Play and Credits. **Local Play** is
split-screen for up to four players on one machine:
- **Identity Setup:** each connected controller has a quarter of the screen. Players press A to
  join (pick an identity, then Ready). A controller that doesn't join here has no player in the
  match.
- **Pregame Lobby:** mode, type, map, time and kit. Each player readies with X; player 1 launches
  with Start (a countdown that Start cancels).
- **The match:** one view per player (two players: side by side), each with its own HUD.

**Controllers.** Each player needs a controller of their own: run `grawmp` with
`--graw_shared_controllers=false` (or `graw_shared_controllers = false` in `grawmp.toml` next to
it). The front end and the campaign keep the default, every controller driving player 1. With it
off:
- the first controller to connect is player 1, the next player 2, up to 4;
- a controller that disconnects keeps its slot free;
- the keyboard drives player 1.

**Sign-in.** The runtime signs in only player 1 (a local profile). Players 2-4 join unsigned, as
"PLAYER TWO" and so on, which Identity Setup accepts: no profiles are needed for split-screen.

**Known issues:**
- On a PC with a virtual controller that enumerates first (Logitech G HUB's "XInput Controller
  #1"), that controller is player 1 and the real pads start at player 2. Disabling the virtual
  controller, or connecting the real pad first, puts it on player 1.
- When the players aren't on controller slots 1 and 2 (as above, with the virtual controller
  idle on slot 1), the first player's view in the match can be black: only the HUD draws, which
  leaves that player unable to play. Seen once on Windows (2026-10-26), cause not known yet.
- On the Steam Deck, an Instant Action match crashed once after about a minute (Deck test 6:
  the game reported a "pure virtual function call"). It didn't come back in later play (test 7),
  so the cause is still open.
- A controller SDL has no mapping for is ignored (the log says "has no gamepad mapping").

**Test** (2026-10-26, Windows, `private/autopilot/mp-local-2.txt` with a scripted second player,
see `src/autopilot.h`'s `P<n>`):
- players 1 and 2 joined and readied up, and the match launched on Desert Gulch, split side by
  side;
- 60 fps, with one dip while the match loaded;
- no errors beyond the shader-include ones every `grawmp` run logs.

**Steam Deck** (test 7, 2026-10-26, played by the user): two players on one Deck in Instant
Action and Elimination, no black screens, a good frame rate.

Not checked yet: three and four players, a full match. Online play is #48, a third-person camera
#49.
