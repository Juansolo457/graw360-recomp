# Run inside RenderDoc (qrenderdoc --python tools/rdoc_trigger.py) with env vars:
#   GRAW_TIMES = comma-separated seconds from now at which to capture one frame each,
#   GRAW_OUT = output folder for the log (private)
# Connects to the first program RenderDoc has injected into on this machine (started with
# renderdoccmd capture) and triggers captures through the target-control API, so captures need
# no key press in the game window (works unattended, with the autopilot). The captures land
# where renderdoccmd's -c option said.
import os
import time
import traceback

import renderdoc as rd

out_dir = os.environ.get("GRAW_OUT", ".")
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "trigger.txt"), "w", encoding="utf-8")


def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    start = time.time()
    times = sorted(float(t) for t in os.environ["GRAW_TIMES"].split(","))
    target = None
    while target is None and time.time() - start < times[0]:
        ident = rd.EnumerateRemoteTargets("", 0)
        while ident:
            tc = rd.CreateTargetControl("", ident, "graw-trigger", True)
            if tc and tc.Connected():
                p("connected to", tc.GetTarget(), "pid", tc.GetPID(), "ident", ident)
                target = tc
                break
            ident = rd.EnumerateRemoteTargets("", ident)
        if target is None:
            time.sleep(1)
    if target is None:
        raise RuntimeError("no RenderDoc target found")
    for t in times:
        while time.time() - start < t:
            target.ReceiveMessage(None)
            time.sleep(0.1)
        target.TriggerCapture(1)
        p("triggered at %.1f s" % (time.time() - start))
    end = time.time() + 20
    while time.time() < end:
        msg = target.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            p("capture", msg.newCapture.path, "frame", msg.newCapture.frameNumber)
        time.sleep(0.1)
    target.Shutdown()
except Exception:  # noqa: BLE001
    p(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
