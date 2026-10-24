# Run inside RenderDoc (qrenderdoc --python tools/rdoc_find_video.py) with env vars:
#   GRAW_RDC = capture, GRAW_OUT = output folder (private), GRAW_W/GRAW_H = video size (160x120)
# Finds the small HUD draws (<= 8 vertices) that sample a GRAW_WxGRAW_H texture through any
# descriptor (bindless included), and for each logs it plus the next 8 draws on the same target:
# textures sampled (with UV ranges), post-VS positions and vertex colours, blend state. Used to
# compare the HUD overlays' vertex alpha between the recomp and Canary (#2).
import os
import struct
import traceback

import renderdoc as rd

out_dir = os.environ.get("GRAW_OUT", ".")
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "video_draws.txt"), "w", encoding="utf-8")
want = (int(os.environ.get("GRAW_W", "160")), int(os.environ.get("GRAW_H", "120")))


def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()


def walk(actions):
    for a in actions:
        yield a
        if a.children:
            yield from walk(a.children)


def sampled(ctl, textures):
    out = []
    for acc in ctl.GetDescriptorAccess():
        if acc.stage != rd.ShaderStage.Pixel or acc.type not in (rd.DescriptorType.Image,
                                                                  rd.DescriptorType.ImageSampler):
            continue
        rng = rd.DescriptorRange()
        rng.offset = acc.byteOffset
        rng.descriptorSize = acc.byteSize
        rng.count = 1
        for d in ctl.GetDescriptors(acc.descriptorStore, [rng]):
            t = textures.get(d.resource)
            if t:
                out.append((d.resource, t))
    return out


def describe(ctl, a, sfile, textures, tag):
    ctl.SetFrameEvent(a.eventId, True)
    st = ctl.GetPipelineState()
    b = st.GetColorBlends()[0] if st.GetColorBlends() else None
    texs = sampled(ctl, textures)
    p("%s ev %d verts %d outputs %s" % (tag, a.eventId, a.numIndices, [str(o) for o in a.outputs if o != rd.ResourceId.Null()]))
    for rid, t in texs:
        p("    tex", rid, "%dx%d" % (t.width, t.height), t.format.Name())
    if b:
        p("    blend", b.enabled, b.colorBlend.source, b.colorBlend.destination, "mask", b.writeMask)
    try:
        m = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if m.vertexResourceId != rd.ResourceId.Null():
            n = min(4, a.numIndices)
            data = ctl.GetBufferData(m.vertexResourceId, m.vertexByteOffset, m.vertexByteStride * n)
            for v in range(n):
                fl = struct.unpack_from("<%df" % (m.vertexByteStride // 4), data, v * m.vertexByteStride)
                p("    vsout", v, " ".join("%.4f" % x for x in fl[:16]))
    except Exception as e:  # noqa: BLE001
        p("    postvs failed", e)
    return texs


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["GRAW_RDC"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open failed")
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError("replay failed")
    sfile = ctl.GetStructuredFile()
    textures = {t.resourceId: t for t in ctl.GetTextures()}
    p("capture", os.environ["GRAW_RDC"])
    p("video-sized textures:", [(str(t.resourceId), t.format.Name()) for t in textures.values()
                                if (t.width, t.height) == want])
    draws = [a for a in walk(ctl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
    found = 0
    for i, a in enumerate(draws):
        if a.numIndices > 8:
            continue
        ctl.SetFrameEvent(a.eventId, True)
        texs = sampled(ctl, textures)
        if not any((t.width, t.height) == want for _, t in texs):
            continue
        found += 1
        p("=" * 100)
        describe(ctl, a, sfile, textures, "VIDEO")
        outs = set(str(o) for o in a.outputs)
        n = 0
        for b in draws[i + 1:]:
            if not (set(str(o) for o in b.outputs) & outs):
                continue
            describe(ctl, b, sfile, textures, "  after")
            n += 1
            if n >= 8:
                break
        if found >= 4:
            break
    p("video draws found:", found)
    ctl.Shutdown()
    cap.Shutdown()
except Exception:  # noqa: BLE001
    p(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
