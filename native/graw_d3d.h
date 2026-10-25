// GRAW native renderer: the campaign's guest Xbox 360 D3D (Yeti_Final_Xenon.exe, an XDK from
// 2006). Function addresses, device fields and object layouts, from the disassembly; how each was
// found is in docs/NATIVE_RENDERER.md. pgr4-native's equivalents (PGR4's 2007 XDK) differ in
// most offsets: the device's push cursor moved from +48 to +0, its register blocks by 1,088
// bytes, its bound objects by 104, and the resource header shrank by 12 bytes.
#pragma once

#include <cstdint>

namespace graw::native {

// Guest D3D functions the native path hooks (generated sub_X; r3 = the device unless noted).
namespace fn {
// Draws.
constexpr uint32_t kBeginVertices = 0x824C4578;         // (prim, vertex count, stride) -> space
constexpr uint32_t kEndVertices = 0x824C4818;           // commits BeginVertices' packets
constexpr uint32_t kDrawVerticesUP = 0x824C4838;        // (prim, count, data, stride)
constexpr uint32_t kBeginIndexedVertices = 0x824C48C0;  // (prim, base, vertices, indices, ...)
constexpr uint32_t kDrawVertices = 0x824C4BF8;          // (prim, start vertex, vertex count)
constexpr uint32_t kDrawIndexedVertices = 0x824C4E38;   // (prim, base vertex, start, count)
// Clears, resolves, Swap, tiling.
constexpr uint32_t kClearF = 0x824CD598;  // (flags, rect, color float4, z f1, stencil r8)
constexpr uint32_t kClear = 0x824CDCC8;   // (count, rects, flags, D3DCOLOR, z f1, stencil r9)
constexpr uint32_t kResolve = 0x824D1960;  // (flags, rect, texture, point, level, slice, color,
                                           //  z f1; stencil and params on the stack)
constexpr uint32_t kSwap = 0x824CC9C8;     // (front buffer texture, params)
constexpr uint32_t kBeginTiling = 0x824D2868;  // (flags, count, rects, color, z f1, stencil r9)
constexpr uint32_t kEndTiling = 0x824D2CA0;    // (flags, rects, texture, color, z f1, stencil r9,
                                               //  params r10)
// Push buffer: the functions that move the cursor (device+0) to another place by assignment.
constexpr uint32_t kMakeSpace = 0x824D7E80;     // submits the segment, starts the next one 32-byte
                                                // aligned after it (sub_824D7D40)
constexpr uint32_t kWrapSegment = 0x824D7C08;   // back to the buffer's start (sub_824D6F00)
constexpr uint32_t kRestoreSegment = 0x824D6F88;  // the segment saved at device+13612
// The draw flush: sends the dirty state (called by the four draw functions).
constexpr uint32_t kFlushState = 0x824D9E98;
}  // namespace fn

// Device layout (byte offsets from the guest D3D device pointer).
namespace dev {
constexpr uint32_t kPush = 0;            // last written push-buffer dword
constexpr uint32_t kPushEnd = 4;         // end of the current segment
constexpr uint32_t kPushLimit = 8;       // past it, the next write calls MakeSpace
constexpr uint32_t kDirtyMasks = 16;     // 5 x u64, big-endian (+16 VS consts, +24 PS consts,
                                         // +32 fetch/bool/0x2000, +40 0x2100..0x2280,
                                         // +48 0x2300/0x2380, clip planes, shader load)
constexpr uint32_t kFetchConstants = 1152;  // 0x4800, 32 x 6 dwords
constexpr uint32_t kVsConstants = 1920;     // 0x4000, 256 x vec4
constexpr uint32_t kPsConstants = 6016;     // 0x4400, 256 x vec4
constexpr uint32_t kBoolLoop = 10112;       // 0x4900, 40 dwords
constexpr uint32_t kRingReadPtr = 10384;    // pointer to the GPU's write-back of its read pointer
constexpr uint32_t kFlags = 10432;          // u8: 0x10 tiling begun, 0x04 predicated tiling
constexpr uint32_t kShadow2000 = 11456;     // register shadow blocks (sub_824D9E98)
constexpr uint32_t kShadow2100 = 11532;
constexpr uint32_t kShadow2180 = 11616;
constexpr uint32_t kShadow2200 = 11636;
constexpr uint32_t kShadow2280 = 11684;
constexpr uint32_t kShadow2300 = 11768;
constexpr uint32_t kShadow2380 = 11920;
// Not GPU registers: the window scissor SetScissorRect (sub_824D03C0) keeps in the two dwords
// after block 0x2000's flushed ones (x | y << 16, 15-bit signed fields) and sub_824CDD78 writes
// into the stream as PA_SC_WINDOW_SCISSOR_TL/BR (pgr4-native: device+10436/+10440, the same
// place relative to the block).
constexpr uint32_t kScissorTL = 11524;
constexpr uint32_t kScissorBR = 11528;
constexpr uint32_t kIndexBuffer = 12532;    // guest D3D index buffer object
constexpr uint32_t kColorSurfaces = 12536;  // 4 x guest D3D surface object
constexpr uint32_t kDepthSurface = 12552;
constexpr uint32_t kVertexStride = 12688;   // u8: BeginVertices' vertex stride in dwords
constexpr uint32_t kTileCount = 12996;      // BeginTiling's tiles
constexpr uint32_t kTileRects = 13000;      // 15 x {x1, y1, x2, y2}
constexpr uint32_t kTileOrigins = 13240;    // 15 x the rectangle rounded out to 32 pixels
constexpr uint32_t kPendingPush = 13652;    // BeginVertices' cursor, committed by EndVertices
}  // namespace dev

// The bits of the five dirty masks (device+16, +24, +32, +40, +48) the draw flush sub_824D9E98
// tests, by mask (host order; its rlwinm/rldicr masks use big-endian bit numbers).
constexpr uint64_t kDirtyBits[5] = {
    ~0ull,                 // +16: VS float constants
    ~0ull,                 // +24: PS float constants
    0xFFFFFFFF00000000ull  // +32: fetch constants
        | 0x80000000ull    //      bool/loop constants
        | 0x3FFFC000ull,   //      register block 0x2000
    0xFFFFF80000000000ull  // +40: register block 0x2100
        | 0x7C000000000ull //      0x2180
        | 0x3FFC000000ull  //      0x2200
        | 0x3FFFFE0ull,    //      0x2280
    0xFFFFFFFFFC000000ull  // +48: register block 0x2300
        | 0x3FC0000ull     //      0x2380
        | 0x400ull         //      sub_824D8D60
        | 0x1E0ull,        //      the shader load (kShaderLoadBits)
};
// Mask +48 bits 5..8 (rlwinm r11,r11,0,23,26): the flush reloads the shaders (sub_824D98C8:
// IM_LOAD, the shaders' literal constants, INVALIDATE_STATE).
constexpr uint64_t kShaderLoadBits = 0x1E0ull;

// Resource objects. GRAW's D3DResource header is 12 bytes (PGR4's: 24).
constexpr uint32_t kTextureFetchOffset = 16;  // D3DBaseTexture: GPU fetch constant, 6 dwords
constexpr uint32_t kIndexBufferFormat = 0;    // D3DIndexBuffer: bit 31 = 32-bit indices
constexpr uint32_t kIndexBufferAddress = 12;  // virtual address of the indices

}  // namespace graw::native
