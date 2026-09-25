#pragma once

// -----------------------------------------------------------------------------
// Split-frame DLSS Frame Generation (DLSS-G) tiling.
//
// Companion to upscalers/dlss/DLSSTiling.h. SR tiling creates three NGX
// features; DLSS-G is driven through Streamline instead, so the equivalent is
// three Streamline viewports. Each viewport gets:
//   - the same shared input textures, tagged with that tile's sl::Extent
//     (depth/motion vectors in their own resolution, hudless/UI in display
//     resolution),
//   - a backbuffer tag whose extent is the tile's monitor region,
//   - its own constants (tile-local projection, tile aspect, MV scale
//     normalised to the tile's MV extent).
// All viewports write into the one shared back buffer, as documented in the
// Streamline DLSS-G programming guide (5.3 Multiple Viewports).
//
// Kept free of D3D/Streamline types so tests/dlssg_tiling_tests.cpp can
// exercise the geometry on the CPU.
// -----------------------------------------------------------------------------

#include <cstdint>

namespace DLSSGTiling
{
// Debug switch for A/B comparison at Surround resolutions. false keeps the
// original single-viewport DLSS-G path for every resolution.
constexpr bool EnableTiling = true;

// Same threshold as the SR tiling gate: ordinary outputs stay single viewport.
constexpr uint32_t MinTiledDisplayWidth = 8192;

// Matches DLSSTiling::TileCountFromEnv()'s hardcoded three (one per monitor).
constexpr uint32_t TileCount = 3;
constexpr uint32_t MaxTiles = TileCount;

// Streamline normalises motion vectors with consts.mvecScale. true normalises
// to the tile's own MV extent width (pixel displacement / tile width), which
// matches a viewport whose MV tag extent is that tile. false normalises to the
// full-frame MV width. Pixel displacement values themselves are never changed.
constexpr bool MVScaleUsesTileWidth = true;

inline uint32_t TileCountForDisplay(uint32_t displayWidth)
{
    if (!EnableTiling || displayWidth < MinTiledDisplayWidth)
        return 1;

    return TileCount;
}

struct Span
{
    uint32_t offset = 0;
    uint32_t size = 0;
};

// Boundary k of a range split into `count` parts, rounded to the nearest
// integer. Boundary 0 is 0 and boundary `count` is `size`.
inline uint32_t SplitBoundary(uint32_t size, uint32_t count, uint32_t k)
{
    if (count == 0)
        return 0;

    return static_cast<uint32_t>((2ull * size * k + count) / (2ull * count));
}

// Contiguous, gap-free split of [base, base + size) into `count` parts.
// Uneven sizes (e.g. 5875 render pixels) get tiles that differ by one pixel;
// every pixel belongs to exactly one tile. Boundaries are proportional, so a
// render-space boundary and its display-space boundary refer to the same
// screen position within half a pixel of their own resolution.
inline Span SplitSpan(uint32_t base, uint32_t size, uint32_t count, uint32_t index)
{
    Span span {};

    if (count == 0 || index >= count)
        return span;

    const uint32_t begin = SplitBoundary(size, count, index);
    const uint32_t end = SplitBoundary(size, count, index + 1);

    span.offset = base + begin;
    span.size = end - begin;
    return span;
}

// Converts a full-frame projection into the off-center projection of a tile
// covering display pixels [tileOrigin, tileOrigin + tileWidth) of a frame
// fullWidth wide. Row-vector convention (clip = view * M, DirectXMath):
//   clip.x' = (W / t) * clip.x + ((W - 2 * o - t) / t) * clip.w
// Other clip components are unchanged. Same mapping as the RR tile projection.
inline void ApplyTileToProjectionRowVector(float m[4][4], double fullWidth, double tileOrigin, double tileWidth)
{
    if (tileWidth <= 0.0 || fullWidth <= 0.0)
        return;

    const double scale = fullWidth / tileWidth;
    const double offset = (fullWidth - 2.0 * tileOrigin - tileWidth) / tileWidth;

    for (int row = 0; row < 4; row++)
        m[row][0] = static_cast<float>(scale * m[row][0] + offset * m[row][3]);
}
} // namespace DLSSGTiling
