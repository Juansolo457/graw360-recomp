# Run inside RenderDoc (qrenderdoc --python tools/rdoc_pixel_history.py) with env vars:
#   GRAW_RDC = capture path, GRAW_OUT = output folder (private),
#   GRAW_PX, GRAW_PY = pixel position as fractions of the target (default: centre of the
#   Cross-Com box, 0.145 x 0.24 of the 16:9 frame)
# Lists the colour render targets of the frame with their draw ranges, then runs pixel history at
# that point on every target at least 640 wide, logging each event that touched the pixel: the
# shader output, the post-modification value, and which tests passed or failed. Also saves the
# final contents of those targets as PNGs. For the #2 comparison (docs/CROSSCOM.md).
import os
import traceback

import renderdoc as rd

out_dir = os.environ.get("GRAW_OUT", ".")
rdc = os.environ["GRAW_RDC"]
fx = float(os.environ.get("GRAW_PX", "0.145"))
fy = float(os.environ.get("GRAW_PY", "0.24"))
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "pixel_history.txt"), "w", encoding="utf-8")


def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()


def walk(actions):
    for a in actions:
        yield a
        if a.children:
            yield from walk(a.children)


def col(v):
    f = v.floatValue
    return "(%.3f %.3f %.3f %.3f)" % (f[0], f[1], f[2], f[3])


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(rdc, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open failed")
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError("replay failed %s" % status)
    sfile = ctl.GetStructuredFile()
    textures = {t.resourceId: t for t in ctl.GetTextures()}

    # Colour targets and the draws that write them.
    targets = {}
    last_event = 0
    for a in walk(ctl.GetRootActions()):
        last_event = max(last_event, a.eventId)
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        for o in a.outputs:
            if o != rd.ResourceId.Null():
                lst = targets.setdefault(o, [])
                lst.append(a.eventId)
    p("capture", rdc)
    p("colour targets (width x height format: draws, first..last event):")
    big = []
    for rid, evs in sorted(targets.items(), key=lambda kv: kv[1][-1]):
        t = textures.get(rid)
        if not t:
            continue
        p("  ", rid, "%dx%d" % (t.width, t.height), t.format.Name(), ":", len(evs), "draws,",
          evs[0], "..", evs[-1])
        if t.width >= 640:
            big.append((rid, t))

    ctl.SetFrameEvent(last_event, True)
    for rid, t in big:
        x = int(t.width * fx)
        y = int(t.height * fy)
        p("=" * 100)
        p("pixel history", rid, "%dx%d" % (t.width, t.height), "at", x, y)
        sub = rd.Subresource(0, 0, 0)
        hist = ctl.PixelHistory(rid, x, y, sub, rd.CompType.Typeless)
        p("  modifications:", len(hist))
        for m in hist[-60:]:
            a = None
            name = ""
            for aa in walk(ctl.GetRootActions()):
                if aa.eventId == m.eventId:
                    name = aa.GetName(sfile)
                    break
            flags = []
            for fl in ("sampleMasked", "backfaceCulled", "depthClipped", "viewClipped",
                       "scissorClipped", "shaderDiscarded", "depthTestFailed",
                       "stencilTestFailed", "predicationSkipped", "unboundPS"):
                if getattr(m, fl, False):
                    flags.append(fl)
            p("  ev", m.eventId, name[:60], "| prim", m.primitiveID,
              "| shaderOut", col(m.shaderOut.col), "| post", col(m.postMod.col),
              "| passed" if m.Passed() else "| FAILED", ",".join(flags))
        path = os.path.join(out_dir, "target_%s.png" % str(int(rid)) if hasattr(rid, "__int__") else "target.png")
        s = rd.TextureSave()
        s.resourceId = rid
        s.destType = rd.FileType.PNG
        try:
            ctl.SaveTexture(s, os.path.join(out_dir, "target_%s.png" % str(rid).replace("ResourceId::", "")))
        except Exception as e:  # noqa: BLE001
            p("  save failed", e)
    ctl.Shutdown()
    cap.Shutdown()
except Exception:  # noqa: BLE001
    p(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
