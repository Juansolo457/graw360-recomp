# Run inside RenderDoc (qrenderdoc --python tools/rdoc_count_draws.py) with GRAW_RDCS set to
# ';'-separated capture paths and GRAW_OUT to an output folder. Writes draw/dispatch counts per
# capture, so the one that holds a whole guest frame (not just the presenter blit) can be picked.
import os
import traceback

import renderdoc as rd

out_dir = os.environ.get("GRAW_OUT", ".")
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "draw_counts.txt"), "w", encoding="utf-8")


def walk(actions):
    for a in actions:
        yield a
        if a.children:
            yield from walk(a.children)


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    for path in os.environ["GRAW_RDCS"].split(";"):
        cap = rd.OpenCaptureFile()
        if cap.OpenFile(path, "", None) != rd.ResultCode.Succeeded:
            log.write("%s open failed\n" % path)
            continue
        status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
        if status != rd.ResultCode.Succeeded:
            log.write("%s replay failed\n" % path)
            cap.Shutdown()
            continue
        draws = dispatches = copies = 0
        targets = set()
        for a in walk(ctl.GetRootActions()):
            if a.flags & rd.ActionFlags.Drawcall:
                draws += 1
                for o in a.outputs:
                    if o != rd.ResourceId.Null():
                        targets.add(o)
            if a.flags & rd.ActionFlags.Dispatch:
                dispatches += 1
            if a.flags & rd.ActionFlags.Copy:
                copies += 1
        log.write("%s draws %d dispatches %d copies %d targets %d\n"
                  % (os.path.basename(path), draws, dispatches, copies, len(targets)))
        log.flush()
        ctl.Shutdown()
        cap.Shutdown()
except Exception:  # noqa: BLE001
    log.write(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
