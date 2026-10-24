# Run inside RenderDoc (qrenderdoc --python tools/rdoc_events.py) with env vars:
#   GRAW_RDC = capture, GRAW_OUT = output folder (private), GRAW_EVENTS = comma-separated event ids
#   or "target:<resource id number>" for every draw on that colour target
# For each event: shaders, viewport/scissor, blend/depth state, the descriptors the shaders
# actually accessed (works with bindless), their textures (saved as PNG) and samplers, and the
# post-VS output of the first vertices (positions, texcoords). Part of the #2 comparison.
import os
import struct
import traceback

import renderdoc as rd

out_dir = os.environ.get("GRAW_OUT", ".")
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "events.txt"), "w", encoding="utf-8")


def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()


def walk(actions):
    for a in actions:
        yield a
        if a.children:
            yield from walk(a.children)


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
    actions = {a.eventId: a for a in walk(ctl.GetRootActions())}
    spec = os.environ["GRAW_EVENTS"]
    if spec.startswith("target:"):
        want = spec.split(":", 1)[1]
        evs = [e for e, aa in sorted(actions.items())
               if (aa.flags & rd.ActionFlags.Drawcall)
               and any(str(o).replace("ResourceId::", "") == want for o in aa.outputs)]
        p("draws on target", want, ":", len(evs))
    else:
        evs = [int(x) for x in spec.split(",")]
    for ev in evs:
        a = actions.get(ev)
        p("=" * 100)
        p("event", ev, a.GetName(sfile) if a else "?", "verts", a.numIndices if a else "?")
        ctl.SetFrameEvent(ev, True)
        st = ctl.GetPipelineState()
        p("  vs", st.GetShader(rd.ShaderStage.Vertex), "ps", st.GetShader(rd.ShaderStage.Pixel))
        vp = st.GetViewport(0)
        sc = st.GetScissor(0)
        p("  viewport", vp.x, vp.y, vp.width, vp.height, "scissor", sc.x, sc.y, sc.width, sc.height)
        for i, b in enumerate(st.GetColorBlends()[:1]):
            p("  blend", i, "enabled", b.enabled, "color", b.colorBlend.source,
              b.colorBlend.operation, b.colorBlend.destination, "alpha", b.alphaBlend.source,
              b.alphaBlend.operation, b.alphaBlend.destination, "mask", b.writeMask)
        try:
            d3 = ctl.GetD3D12PipelineState()
            ds = d3.outputMerger.depthStencilState
            p("  depth enable", ds.depthEnable, "func", ds.depthFunction, "write", ds.depthWrites,
              "stencil", ds.stencilEnable)
        except Exception as e:  # noqa: BLE001
            p("  d3d12 state failed", e)
        # Descriptors the shaders accessed.
        try:
            accesses = ctl.GetDescriptorAccess()
            p("  descriptor accesses:", len(accesses))
            for acc in accesses:
                if acc.stage not in (rd.ShaderStage.Pixel, rd.ShaderStage.Vertex):
                    continue
                rng = rd.DescriptorRange()
                rng.offset = acc.byteOffset
                rng.descriptorSize = acc.byteSize
                rng.count = 1
                desc = ctl.GetDescriptors(acc.descriptorStore, [rng])
                for dsc in desc:
                    t = textures.get(dsc.resource)
                    info = "%dx%d %s" % (t.width, t.height, t.format.Name()) if t else ""
                    p("   ", str(acc.stage), str(acc.type), "index", acc.index, "array",
                      acc.arrayElement, "->", dsc.resource, info, "fmt", dsc.format.Name(),
                      "swizzle", dsc.swizzle.red, dsc.swizzle.green, dsc.swizzle.blue,
                      dsc.swizzle.alpha)
                    if t and acc.stage == rd.ShaderStage.Pixel:
                        s = rd.TextureSave()
                        s.resourceId = dsc.resource
                        s.destType = rd.FileType.PNG
                        s.alpha = rd.AlphaMapping.Preserve
                        try:
                            ctl.SaveTexture(s, os.path.join(
                                out_dir, "ev%d_%s.png" % (ev, str(dsc.resource).replace("ResourceId::", "res"))))
                        except Exception as e:  # noqa: BLE001
                            p("    save failed", e)
        except Exception as e:  # noqa: BLE001
            p("  descriptor access failed", e, traceback.format_exc())
        # Post-VS data.
        try:
            m = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if m.vertexResourceId != rd.ResourceId.Null() and a:
                n = min(6, a.numIndices)
                data = ctl.GetBufferData(m.vertexResourceId, m.vertexByteOffset, m.vertexByteStride * n)
                for v in range(n):
                    fl = struct.unpack_from("<%df" % (m.vertexByteStride // 4), data, v * m.vertexByteStride)
                    p("  vsout", v, " ".join("%.4f" % x for x in fl[:12]))
        except Exception as e:  # noqa: BLE001
            p("  postvs failed", e)
    ctl.Shutdown()
    cap.Shutdown()
except Exception:  # noqa: BLE001
    p(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
