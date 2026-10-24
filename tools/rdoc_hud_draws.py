# Run inside RenderDoc (qrenderdoc --python tools/rdoc_hud_draws.py) with env vars:
#   GRAW_RDC  = path to the .rdc capture
#   GRAW_OUT  = output folder (private: captures contain game imagery)
#   GRAW_W, GRAW_H = size of the panel texture to look for (default 160x120)
# Finds every draw that binds a GRAW_WxGRAW_H texture as a shader resource and writes, per draw:
# event id, marker path, vertex count, viewport/scissor, blend and depth state, the bound
# resources, the vertex shader inputs (first vertices, post-VS positions), and PNGs of the
# sampled texture and of the colour target before and after the draw. Part of the #2
# recomp-vs-Canary comparison (docs/CROSSCOM.md).
import os
import sys
import traceback

import renderdoc as rd

out_dir = os.environ.get("GRAW_OUT", ".")
rdc = os.environ["GRAW_RDC"]
want_w = int(os.environ.get("GRAW_W", "160"))
want_h = int(os.environ.get("GRAW_H", "120"))
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "hud_draws.txt"), "w", encoding="utf-8")


def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()


def save_tex(ctl, res_id, path, sub=None):
    s = rd.TextureSave()
    s.resourceId = res_id
    s.destType = rd.FileType.PNG
    s.alpha = rd.AlphaMapping.Preserve
    if sub is not None:
        s.mip = sub.mip
        s.slice.sliceIndex = sub.slice
    try:
        ctl.SaveTexture(s, path)
    except Exception as e:  # noqa: BLE001
        p("  save failed", path, e)


def walk(actions, prefix=""):
    for a in actions:
        name = a.GetName(sfile) if hasattr(a, "GetName") else a.customName
        yield a, prefix
        if a.children:
            yield from walk(a.children, prefix + name + " / ")


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(rdc, "", None) != rd.ResultCode.Succeeded:
        p("open failed")
        raise SystemExit
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        p("replay failed", status)
        raise SystemExit
    sfile = ctl.GetStructuredFile()
    textures = {t.resourceId: t for t in ctl.GetTextures()}
    resources = {r.resourceId: r for r in ctl.GetResources()}
    p("capture", rdc, "textures", len(textures))
    sized = [t for t in textures.values() if t.width == want_w and t.height == want_h]
    p("textures of %dx%d:" % (want_w, want_h))
    for t in sized:
        p("  ", resources[t.resourceId].name if t.resourceId in resources else "", t.resourceId,
          t.format.Name(), "mips", t.mips, "arraysize", t.arraysize)
    sized_ids = {t.resourceId for t in sized}

    # Bindless renderers pick textures from a descriptor heap at run time, so per-draw binding
    # lists miss them. Use each texture's usage list instead.
    usage_events = {}
    for t in sized:
        for u in ctl.GetUsage(t.resourceId):
            p("  usage", t.resourceId, "event", u.eventId, str(u.usage))
            usage_events.setdefault(u.eventId, []).append((str(u.usage), t.resourceId))
    actions_by_event = {}
    for a, prefix in walk(ctl.GetRootActions()):
        actions_by_event[a.eventId] = (a, prefix)
    count = 0
    for ev in sorted(usage_events):
        if ev not in actions_by_event:
            continue
        a, prefix = actions_by_event[ev]
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        ctl.SetFrameEvent(a.eventId, True)
        state = ctl.GetPipelineState()
        bound = [(0, rid) for use, rid in usage_events[ev] if "Resource" in use]
        p("usage at event", ev, usage_events[ev])
        if not bound:
            bound = [(0, rid) for use, rid in usage_events[ev]]
        count += 1
        if count > 40:
            break
        p("=" * 100)
        p("event", a.eventId, "draw", a.GetName(sfile), "verts", a.numIndices, "inst", a.numInstances)
        p("path", prefix)
        for idx, rid in bound:
            p("  samples", rid, "at slot", idx)
        vp = state.GetViewport(0)
        sc = state.GetScissor(0)
        p("  viewport", vp.x, vp.y, vp.width, vp.height, "depth", vp.minDepth, vp.maxDepth)
        p("  scissor", sc.x, sc.y, sc.width, sc.height, "enabled", sc.enabled)
        cb = state.GetColorBlends()
        if cb:
            b = cb[0]
            p("  blend0 enabled", b.enabled, "color", b.colorBlend.source, b.colorBlend.operation,
              b.colorBlend.destination, "alpha", b.alphaBlend.source, b.alphaBlend.operation,
              b.alphaBlend.destination, "writemask", b.writeMask)
        outs = state.GetOutputTargets()
        for i, o in enumerate(outs):
            if o.resource != rd.ResourceId.Null():
                t = textures.get(o.resource)
                p("  output", i, o.resource, t.width if t else "?", t.height if t else "?",
                  t.format.Name() if t else "")
        depth = state.GetDepthTarget()
        p("  depth target", depth.resource)
        try:
            ps = state.GetShaderReflection(rd.ShaderStage.Pixel)
            vs = state.GetShaderReflection(rd.ShaderStage.Vertex)
            p("  vs", state.GetShader(rd.ShaderStage.Vertex), "ps", state.GetShader(rd.ShaderStage.Pixel))
        except Exception as e:  # noqa: BLE001
            p("  shader refl failed", e)
        # Post-VS positions of the first vertices.
        try:
            mesh = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if mesh.vertexResourceId != rd.ResourceId.Null():
                data = ctl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset,
                                         mesh.vertexByteStride * min(8, max(1, a.numIndices)))
                import struct
                for v in range(min(8, a.numIndices)):
                    off = v * mesh.vertexByteStride
                    fl = struct.unpack_from("<%df" % (mesh.vertexByteStride // 4), data, off)
                    p("  vsout", v, ["%.3f" % x for x in fl[:12]])
        except Exception as e:  # noqa: BLE001
            p("  postvs failed", e)
        base = os.path.join(out_dir, "ev%06d" % a.eventId)
        for idx, rid in bound:
            save_tex(ctl, rid, base + "_tex%d.png" % idx)
        if outs and outs[0].resource != rd.ResourceId.Null():
            save_tex(ctl, outs[0].resource, base + "_rt_after.png")
            ctl.SetFrameEvent(a.eventId, False)
            prev = a.previous
            if prev is not None:
                ctl.SetFrameEvent(prev.eventId, True)
                save_tex(ctl, outs[0].resource, base + "_rt_before.png")
    p("draws sampling the texture:", count)
    ctl.Shutdown()
    cap.Shutdown()
except SystemExit:
    pass
except Exception:  # noqa: BLE001
    p(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
