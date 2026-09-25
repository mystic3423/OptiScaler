// CPU checks for OptiScaler/framegen/dlssg/DLSSGTiling.h (DLSS-G split-frame tiling).
// Build from the repo root in a VS x64 developer prompt:
//   cl /nologo /EHsc /std:c++20 /W4 /IOptiScaler\framegen\dlssg tests\dlssg_tiling_tests.cpp
//      /Fo:x64\dlssg_tiling_tests.obj /Fe:x64\dlssg_tiling_tests.exe
// These tests cover geometry only; they do not load Streamline or DLSS-G.

#include <DLSSGTiling.h>

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>
#include <cstdint>

using namespace DirectX;

static int g_failures = 0;
static long long g_checks = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            if (g_failures < 25)                                                                                       \
            {                                                                                                          \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
                std::printf(__VA_ARGS__);                                                                              \
                std::printf("\n");                                                                                     \
            }                                                                                                          \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static void TestTileCount()
{
    CHECK(DLSSGTiling::TileCountForDisplay(0) == 1, "0 wide");
    CHECK(DLSSGTiling::TileCountForDisplay(3840) == 1, "4K stays single viewport");
    CHECK(DLSSGTiling::TileCountForDisplay(7680) == 1, "7680 stays single viewport");
    CHECK(DLSSGTiling::TileCountForDisplay(8191) == 1, "8191 stays single viewport");

    const uint32_t expected = DLSSGTiling::EnableTiling ? DLSSGTiling::TileCount : 1;
    CHECK(DLSSGTiling::TileCountForDisplay(8192) == expected, "8192 threshold");
    CHECK(DLSSGTiling::TileCountForDisplay(11520) == expected, "triple 4K");
}

// Every pixel of [base, base + size) belongs to exactly one tile, tiles are in
// order and differ in size by at most one pixel.
static void TestSplitCoverage()
{
    const uint32_t bases[] = { 0, 1, 7, 128 };

    for (uint32_t count = 1; count <= 4; count++)
    {
        for (uint32_t base : bases)
        {
            for (uint32_t size = 0; size <= 12000; size += (size < 64 ? 1 : 37))
            {
                uint32_t expectedOffset = base;
                uint32_t minSize = UINT32_MAX;
                uint32_t maxSize = 0;

                for (uint32_t i = 0; i < count; i++)
                {
                    const auto span = DLSSGTiling::SplitSpan(base, size, count, i);
                    CHECK(span.offset == expectedOffset, "gap/overlap base %u size %u count %u tile %u", base, size,
                          count, i);
                    expectedOffset = span.offset + span.size;
                    minSize = span.size < minSize ? span.size : minSize;
                    maxSize = span.size > maxSize ? span.size : maxSize;
                }

                CHECK(expectedOffset == base + size, "coverage base %u size %u count %u", base, size, count);
                CHECK(maxSize - minSize <= 1, "uneven by more than 1: size %u count %u", size, count);
            }
        }
    }

    const auto outOfRange = DLSSGTiling::SplitSpan(10, 100, 3, 3);
    CHECK(outOfRange.size == 0, "index past count");
}

// Known layouts from owner logs.
static void TestKnownLayouts()
{
    // 11520 display: one tile per monitor
    for (uint32_t i = 0; i < 3; i++)
    {
        const auto span = DLSSGTiling::SplitSpan(0, 11520, 3, i);
        CHECK(span.offset == i * 3840 && span.size == 3840, "11520 tile %u -> %u %u", i, span.offset, span.size);
    }

    // Ultra Performance render width
    for (uint32_t i = 0; i < 3; i++)
    {
        const auto span = DLSSGTiling::SplitSpan(0, 3840, 3, i);
        CHECK(span.offset == i * 1280 && span.size == 1280, "3840 render tile %u", i);
    }

    // AFOP's uneven 5875 render width
    const auto a = DLSSGTiling::SplitSpan(0, 5875, 3, 0);
    const auto b = DLSSGTiling::SplitSpan(0, 5875, 3, 1);
    const auto c = DLSSGTiling::SplitSpan(0, 5875, 3, 2);
    CHECK(a.offset == 0 && a.size == 1958, "5875 tile 0: %u %u", a.offset, a.size);
    CHECK(b.offset == 1958 && b.size == 1959, "5875 tile 1: %u %u", b.offset, b.size);
    CHECK(c.offset == 3917 && c.size == 1958, "5875 tile 2: %u %u", c.offset, c.size);
}

// A render-space tile boundary and the matching display-space boundary refer
// to the same screen position within half a pixel of each resolution.
static void TestBoundaryCorrespondence()
{
    const uint32_t display = 11520;

    for (uint32_t render = 1280; render <= display; render++)
    {
        for (uint32_t k = 1; k < 3; k++)
        {
            const double renderBoundary = DLSSGTiling::SplitBoundary(render, 3, k) / (double) render;
            const double displayBoundary = DLSSGTiling::SplitBoundary(display, 3, k) / (double) display;
            const double tolerance = 0.5 / render + 0.5 / display + 1e-12;

            CHECK(std::fabs(renderBoundary - displayBoundary) <= tolerance, "render %u boundary %u off by %f", render,
                  k, std::fabs(renderBoundary - displayBoundary) * display);
        }
    }
}

// A view-space point projected with the full-frame matrix lands at display
// pixel px; the tile's off-center matrix must place it at px - tileOrigin.
// y, z and w must be unchanged.
static void TestProjectionCorrespondence()
{
    const float vfovs[] = { 0.6f, 1.0f, 1.3f };
    const uint32_t widths[] = { 11520, 8192, 9000 };
    const float height = 2160.0f;

    for (float vfov : vfovs)
    {
        for (uint32_t width : widths)
        {
            const float aspect = width / height;
            XMFLOAT4X4 full {};
            XMStoreFloat4x4(&full, XMMatrixPerspectiveFovRH(vfov, aspect, 0.1f, 5000.0f));

            for (uint32_t t = 0; t < 3; t++)
            {
                const auto span = DLSSGTiling::SplitSpan(0, width, 3, t);

                XMFLOAT4X4 tile = full;
                DLSSGTiling::ApplyTileToProjectionRowVector(tile.m, width, span.offset, span.size);

                const XMMATRIX fullM = XMLoadFloat4x4(&full);
                const XMMATRIX tileM = XMLoadFloat4x4(&tile);

                for (int ix = -20; ix <= 20; ix++)
                {
                    for (int iz = 1; iz <= 5; iz++)
                    {
                        const float z = -2.0f * iz; // right-handed: forward is -z
                        const XMVECTOR p = XMVectorSet(ix * 0.35f * iz, 0.3f * iz, z, 1.0f);

                        XMFLOAT4 cf {};
                        XMFLOAT4 ct {};
                        XMStoreFloat4(&cf, XMVector4Transform(p, fullM));
                        XMStoreFloat4(&ct, XMVector4Transform(p, tileM));

                        const double pxFull = (cf.x / cf.w * 0.5 + 0.5) * width;
                        const double pxTile = (ct.x / ct.w * 0.5 + 0.5) * span.size;

                        CHECK(std::fabs((pxFull - span.offset) - pxTile) < 0.01, "w %u tile %u x %f vs %f", width,
                              t, pxFull - span.offset, pxTile);
                        CHECK(std::fabs(cf.y - ct.y) < 1e-5f && std::fabs(cf.z - ct.z) < 1e-5f &&
                                  std::fabs(cf.w - ct.w) < 1e-5f,
                              "y/z/w changed");
                    }
                }
            }
        }
    }

    // Middle tile of an evenly split frame is symmetric: x scale only, no offset
    XMFLOAT4X4 full {};
    XMStoreFloat4x4(&full, XMMatrixPerspectiveFovRH(1.0f, 11520.0f / 2160.0f, 0.1f, 5000.0f));
    XMFLOAT4X4 middle = full;
    DLSSGTiling::ApplyTileToProjectionRowVector(middle.m, 11520, 3840, 3840);
    CHECK(std::fabs(middle.m[0][0] - 3.0f * full.m[0][0]) < 1e-5f, "middle tile scale");
    CHECK(std::fabs(middle.m[2][0]) < 1e-5f, "middle tile has no horizontal offset");

    // Tile aspect ratio used by Dispatch matches a 3840x2160 monitor
    const float tileAspect = (11520.0f / 2160.0f) * 3840.0f / 11520.0f;
    CHECK(std::fabs(tileAspect - 3840.0f / 2160.0f) < 1e-6f, "tile aspect");
}

// Normalised MV scale times the tile's MV extent gives back the game's pixel
// displacement, so the scale changes normalisation only.
static void TestMVScale()
{
    const float pixelScale = 1.0f; // game MVs already in pixels
    const uint32_t mvWidths[] = { 3840, 5760, 5875, 7680 };

    for (uint32_t mvWidth : mvWidths)
    {
        for (uint32_t t = 0; t < 3; t++)
        {
            const auto span = DLSSGTiling::SplitSpan(0, mvWidth, 3, t);
            const float tileScale = pixelScale / span.size;
            const float displacementPixels = 42.5f;

            CHECK(std::fabs(displacementPixels * tileScale * span.size - displacementPixels) < 1e-3f,
                  "mv width %u tile %u", mvWidth, t);
        }
    }
}

static void StoreRows(const XMMATRIX& m, float out[4][4])
{
    XMFLOAT4X4 f {};
    XMStoreFloat4x4(&f, m);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r][c] = f.m[r][c];
}

static XMMATRIX LoadRows(const float in[4][4])
{
    XMFLOAT4X4 f {};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            f.m[r][c] = in[r][c];
    return XMLoadFloat4x4(&f);
}

// Game-style Streamline camera matrices re-expressed per tile must keep their
// meaning: inverse pairs stay inverse, and clipToPrevClip moves a point to the
// same tile pixel as projecting the camera-moved point with the tile projection.
static void TestGameCameraMatricesPerTile()
{
    const uint32_t width = 11520;
    const float height = 2160.0f;
    const float vfovs[] = { 0.8f, 1.05f };
    const float yaws[] = { -0.05f, 0.0f, 0.03f }; // per-frame camera turn, radians

    for (float vfov : vfovs)
    {
        const XMMATRIX viewToClip = XMMatrixPerspectiveFovRH(vfov, width / height, 0.1f, 10000.0f);
        const XMMATRIX clipToView = XMMatrixInverse(nullptr, viewToClip);

        for (float yaw : yaws)
        {
            // Current view -> previous view: rotate about Y and translate a little
            const XMMATRIX viewToPrevView = XMMatrixRotationY(yaw) * XMMatrixTranslation(0.2f, -0.1f, 0.3f);
            const XMMATRIX clipToPrevClip = clipToView * viewToPrevView * viewToClip;
            const XMMATRIX prevClipToClip = XMMatrixInverse(nullptr, clipToPrevClip);

            float fullV2C[4][4], fullC2V[4][4], fullC2P[4][4], fullP2C[4][4];
            StoreRows(viewToClip, fullV2C);
            StoreRows(clipToView, fullC2V);
            StoreRows(clipToPrevClip, fullC2P);
            StoreRows(prevClipToClip, fullP2C);

            for (uint32_t t = 0; t < 3; t++)
            {
                const auto span = DLSSGTiling::SplitSpan(0, width, 3, t);

                float v2c[4][4], c2v[4][4], c2p[4][4], p2c[4][4], lens[4][4];
                for (int r = 0; r < 4; r++)
                {
                    for (int c = 0; c < 4; c++)
                    {
                        v2c[r][c] = fullV2C[r][c];
                        c2v[r][c] = fullC2V[r][c];
                        c2p[r][c] = fullC2P[r][c];
                        p2c[r][c] = fullP2C[r][c];
                        lens[r][c] = (r == c) ? 1.0f : 0.0f;
                    }
                }

                using K = DLSSGTiling::TileMatrixKind;
                DLSSGTiling::TileMatrix(v2c, K::ViewToClip, width, span.offset, span.size);
                DLSSGTiling::TileMatrix(c2v, K::ClipToView, width, span.offset, span.size);
                DLSSGTiling::TileMatrix(c2p, K::ClipToClip, width, span.offset, span.size);
                DLSSGTiling::TileMatrix(p2c, K::ClipToClip, width, span.offset, span.size);
                DLSSGTiling::TileMatrix(lens, K::ClipToClip, width, span.offset, span.size);

                // ViewToClip matches the existing projection helper
                float legacy[4][4];
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        legacy[r][c] = fullV2C[r][c];
                DLSSGTiling::ApplyTileToProjectionRowVector(legacy, width, span.offset, span.size);
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        CHECK(std::fabs(legacy[r][c] - v2c[r][c]) < 1e-5f, "viewToClip helper mismatch");

                CHECK(DLSSGTiling::InverseError(v2c, c2v) < 1e-4, "tile %u viewToClip*clipToView err %g", t,
                      DLSSGTiling::InverseError(v2c, c2v));
                CHECK(DLSSGTiling::InverseError(c2p, p2c) < 1e-4, "tile %u clipToPrevClip*prevClipToClip err %g", t,
                      DLSSGTiling::InverseError(c2p, p2c));

                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        CHECK(std::fabs(lens[r][c] - ((r == c) ? 1.0f : 0.0f)) < 1e-6f, "identity lens changed");

                const XMMATRIX tileV2C = LoadRows(v2c);
                const XMMATRIX tileC2P = LoadRows(c2p);

                for (int ix = -12; ix <= 12; ix++)
                {
                    for (int iz = 1; iz <= 4; iz++)
                    {
                        const float z = -3.0f * iz;
                        const XMVECTOR p = XMVectorSet(ix * 0.9f * iz, 0.4f * iz, z, 1.0f);

                        // Through the tile clipToPrevClip
                        XMFLOAT4 viaMatrix {};
                        XMStoreFloat4(&viaMatrix, XMVector4Transform(XMVector4Transform(p, tileV2C), tileC2P));

                        // Directly: move the point into the previous view, then project with the tile projection
                        XMFLOAT4 direct {};
                        XMStoreFloat4(&direct, XMVector4Transform(XMVector4Transform(p, viewToPrevView), tileV2C));

                        if (std::fabs(direct.w) < 1e-3f)
                            continue;

                        const double pxMatrix = (viaMatrix.x / viaMatrix.w * 0.5 + 0.5) * span.size;
                        const double pxDirect = (direct.x / direct.w * 0.5 + 0.5) * span.size;
                        const double pyMatrix = viaMatrix.y / viaMatrix.w;
                        const double pyDirect = direct.y / direct.w;

                        CHECK(std::fabs(pxMatrix - pxDirect) < 0.05, "tile %u yaw %f prev x %f vs %f", t, yaw,
                              pxMatrix, pxDirect);
                        CHECK(std::fabs(pyMatrix - pyDirect) < 1e-4, "tile %u prev y", t);
                    }
                }
            }
        }
    }
}

int main()
{
    TestTileCount();
    TestSplitCoverage();
    TestKnownLayouts();
    TestBoundaryCorrespondence();
    TestProjectionCorrespondence();
    TestMVScale();
    TestGameCameraMatricesPerTile();

    std::printf("%lld checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
