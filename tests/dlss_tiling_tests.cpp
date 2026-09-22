// Standalone CPU checks; does not load NGX or exercise the GPU.
#include "../OptiScaler/upscalers/dlss/DLSSTiling.h"
#include "../OptiScaler/upscalers/dlssd/DLSSDTiling.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

struct Parameters : NVSDK_NGX_Parameter
{
    std::unordered_map<std::string, long double> numbers;
    std::unordered_map<std::string, void*> resources;
#define NUMBER_OVERLOAD(T) \
    void Set(const char* n, T v) override { numbers[n] = static_cast<long double>(v); } \
    NVSDK_NGX_Result Get(const char* n, T* v) const override \
    { \
        const auto it = numbers.find(n); \
        if (it == numbers.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter; \
        *v = static_cast<T>(it->second); \
        return NVSDK_NGX_Result_Success; \
    }
    NUMBER_OVERLOAD(unsigned long long)
    NUMBER_OVERLOAD(float)
    NUMBER_OVERLOAD(double)
    NUMBER_OVERLOAD(unsigned int)
    NUMBER_OVERLOAD(int)
#undef NUMBER_OVERLOAD
#define RESOURCE_OVERLOAD(T) \
    void Set(const char* n, T* v) override { resources[n] = v; } \
    NVSDK_NGX_Result Get(const char* n, T** v) const override \
    { \
        const auto it = resources.find(n); \
        if (it == resources.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter; \
        *v = static_cast<T*>(it->second); \
        return NVSDK_NGX_Result_Success; \
    }
    RESOURCE_OVERLOAD(ID3D11Resource)
    RESOURCE_OVERLOAD(ID3D12Resource)
    RESOURCE_OVERLOAD(void)
#undef RESOURCE_OVERLOAD
    void Reset() override { numbers.clear(); resources.clear(); }
};

void CheckRR()
{
    // A perspective matrix with nonzero off-center and cross-axis terms makes
    // the clip.w translation distinguishable from scaling the X diagonal only.
    float projection[16] = { 0.9f, 0.02f, 0, 0, 0.03f, 1.7f, 0, 0,
                             0.2f, -0.1f, -0.001f, 1, 0, 0, 0.02f, 0 };
    const auto projectX = [](const float* m, const std::array<float, 4>& v) {
        double x = 0, w = 0;
        for (size_t i = 0; i < 4; ++i) { x += v[i] * m[i * 4]; w += v[i] * m[i * 4 + 3]; }
        return x / w;
    };
    for (unsigned int input : { 3840u, 3841u, 5760u, 6682u, 7680u })
    {
        std::vector<DLSSTile> tiles;
        assert(DLSSTiling::BuildFrameTiles(input, 11520, 3, tiles));
        for (const auto& tile : tiles)
        {
            const auto matrix = DLSSDTiling::TileProjection(projection, input, tile.renderX, tile.renderW);
            for (float point : { -0.7f, 0.0f, 0.4f })
            {
                const std::array<float, 4> v = { point, 0.3f, 2.0f, 1.0f };
                const double fullPixel = (projectX(projection, v) + 1.0) * input / 2.0;
                const double tilePixel = (projectX(matrix.data(), v) + 1.0) * tile.renderW / 2.0;
                assert(std::abs(fullPixel - (tile.renderX + tilePixel)) < 0.002);
            }
            for (size_t i = 0; i < 16; ++i)
                if (i % 4 != 0) assert(matrix[i] == projection[i]);
        }

        Parameters p;
        p.Set("Width", input); p.Set("Height", 720u);
        p.Set("DLSS.Render.Subrect.Dimensions.Width", input);
        p.Set("DLSS.Render.Subrect.Dimensions.Height", 720u);
        p.Set("Output", reinterpret_cast<ID3D12Resource*>(0x1000));
        p.Set("ViewToClipMatrix", static_cast<void*>(projection));
        p.Set("WorldToViewMatrix", static_cast<void*>(projection));
        p.Set("MV.Scale.X", static_cast<float>(input)); p.Set("MV.Scale.Y", 720.0f);
        p.Set("Jitter.Offset.X", 0.25f); p.Set("DLSS.Pre.Exposure", 2.0f);
        p.Set("Reset", 0);
        p.Set("DLSS.Output.Subrect.Base.X", 17u); p.Set("DLSS.Output.Subrect.Base.Y", 11u);
        for (const auto& subrect : DLSSTiling::kInputSubrects) p.Set(subrect.name, 3u);
        for (const auto& guide : DLSSDTiling::guides)
        {
            p.Set(guide.resource, reinterpret_cast<ID3D12Resource*>(0x2000));
            p.Set((std::string(guide.base) + ".Subrect.Base.X").c_str(), 7u);
            p.Set((std::string(guide.base) + ".Subrect.Base.Y").c_str(), 5u);
        }
        const auto originalNumbers = p.numbers;
        const auto originalResources = p.resources;
        // Simulate returns after each successful/failed tile and scratch redirect.
        for (bool cropProjection : { false, true })
        for (size_t last = 0; last < tiles.size(); ++last)
        {
            [&] {
                DLSSTiling::EvaluationScope<ID3D12Resource> common(&p);
                DLSSDTiling::EvaluationScope rr(&p, cropProjection);
                for (size_t i = 0; i <= last; ++i)
                {
                    DLSSTiling::ApplyTile(&p, common.base, tiles[i], 720, true);
                    rr.Apply(tiles[i], input);
                    for (const auto& guide : DLSSDTiling::guides)
                    {
                        assert(p.numbers.at(std::string(guide.base) + ".Subrect.Base.X") == 7 + tiles[i].renderX);
                        assert(p.numbers.at(std::string(guide.base) + ".Subrect.Base.Y") == 5);
                    }
                    const auto actual = static_cast<float*>(p.resources.at("ViewToClipMatrix"));
                    const auto expected = DLSSDTiling::TileProjection(projection, input, tiles[i].renderX, tiles[i].renderW);
                    for (size_t j = 0; j < 16; ++j)
                        assert(actual[j] == (cropProjection ? expected[j] : projection[j]));
                    if (!cropProjection) assert(actual == projection);
                    assert(p.numbers.at("MV.Scale.X") == input);
                    assert(p.numbers.at("MV.Scale.Y") == 720);
                    assert(p.numbers.at("Jitter.Offset.X") == 0.25f);
                    assert(p.numbers.at("DLSS.Pre.Exposure") == 2);
                    assert(p.resources.at("WorldToViewMatrix") == projection);
                    p.Set("Reset", 1);
                    p.Set("Output", reinterpret_cast<ID3D12Resource*>(0x3000));
                }
            }();
            assert(p.numbers == originalNumbers);
            assert(p.resources == originalResources);
        }
    }
    Parameters empty;
    { DLSSDTiling::EvaluationScope scope(&empty); scope.Apply({0, 1280, 0, 3840, 3840, 0, 0}, 3840); }
    assert(empty.numbers.empty() && empty.resources.empty());
    std::puts("PASS: RR projection correspondence, guide offsets, unchanged MV/jitter/exposure, failure restoration.");
}

void CheckRRCopyRestoration()
{
    Parameters p;
    p.Set("Width", 3841u); p.Set("Height", 720u);
    p.Set("OutWidth", 11520u); p.Set("OutHeight", 2160u);
    p.Set("MV.Scale.X", 3841.0f); p.Set("MV.Scale.Y", 720.0f);
    p.Set("Jitter.Offset.X", 0.25f); p.Set("DLSS.Pre.Exposure", 2.0f);
    size_t ordinal = 0;
    auto add = [&](const DLSSDTiling::Guide& guide) {
        p.Set(guide.resource, reinterpret_cast<ID3D12Resource*>(0x1000 + ++ordinal * 16));
        p.Set((std::string(guide.base) + ".Subrect.Base.X").c_str(), static_cast<unsigned int>(ordinal));
        p.Set((std::string(guide.base) + ".Subrect.Base.Y").c_str(), static_cast<unsigned int>(ordinal + 2));
    };
    for (const auto& guide : DLSSDTiling::coreInputs) add(guide);
    for (const auto& guide : DLSSDTiling::guides) add(guide);
    const auto originalNumbers = p.numbers;
    const auto originalResources = p.resources;
    // Simulate failure after every possible input binding, including all three
    // tiles. Sources must remain the original resources even after tile zero.
    for (size_t fail = 0; fail < ordinal * 3; ++fail)
    {
        [&] {
            DLSSDTiling::CopyScope scope(&p);
            size_t count = 0;
            for (unsigned int tile = 0; tile < 3; ++tile)
            {
                scope.Dimensions(1290, 720, 3856, 2160);
                for (const auto& input : scope.inputs)
                {
                    assert(input.source == originalResources.at(input.key));
                    assert(input.x == originalNumbers.at(input.xKey));
                    assert(input.y == originalNumbers.at(input.yKey));
                    auto* copied = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(0x9000 + tile * 16));
                    scope.Bind(input, copied);
                    assert(p.resources.at(input.key) == copied);
                    assert(p.numbers.at(input.xKey) == 0 && p.numbers.at(input.yKey) == 0);
                    assert(p.numbers.at("Width") == 1290 && p.numbers.at("OutWidth") == 3856);
                    assert(p.numbers.at("MV.Scale.X") == 3841);
                    assert(p.numbers.at("Jitter.Offset.X") == 0.25f);
                    assert(p.numbers.at("DLSS.Pre.Exposure") == 2);
                    if (count++ == fail) return;
                }
            }
        }();
        assert(p.numbers == originalNumbers);
        assert(p.resources == originalResources);
    }
    // Absent guides are not bound or invented.
    p.resources.erase("DLSSD.Alpha");
    { DLSSDTiling::CopyScope scope(&p); assert(scope.inputs.size() == ordinal - 1); }
    assert(!p.resources.contains("DLSSD.Alpha"));
    // Exercise the real mixed direct/copied parameter ordering, including
    // nonzero caller bases and restoration after each tile's simulated failure.
    p.Set("Output", reinterpret_cast<ID3D12Resource*>(0x90000));
    p.Set("DLSS.Output.Subrect.Base.X", 19u);
    p.Set("DLSS.Output.Subrect.Base.Y", 11u);
    const auto mixedNumbers = p.numbers;
    const auto mixedResources = p.resources;
    std::vector<DLSSTile> tiles;
    assert(DLSSTiling::BuildFrameTiles(3841, 11520, 3, tiles));
    for (size_t failTile = 0; failTile < tiles.size(); ++failTile)
    {
        [&] {
            DLSSTiling::EvaluationScope<ID3D12Resource> common(&p);
            DLSSDTiling::EvaluationScope rr(&p);
            DLSSDTiling::CopyScope copies(&p);
            for (size_t i = 0; i <= failTile; ++i)
            {
                DLSSTiling::ApplyTile(&p, common.base, tiles[i], 720, true);
                rr.Apply(tiles[i], 3841);
                for (const auto& input : copies.inputs)
                {
                    if (DLSSDTiling::CopyInput(input.key))
                    {
                        copies.Bind(input, reinterpret_cast<ID3D12Resource*>(0x99900));
                        assert(p.numbers.at(input.xKey) == 0 && p.numbers.at(input.yKey) == 0);
                    }
                    else
                    {
                        assert(p.resources.at(input.key) == mixedResources.at(input.key));
                        assert(p.numbers.at(input.xKey) == input.x + tiles[i].renderX);
                        assert(p.numbers.at(input.yKey) == input.y);
                    }
                }
                copies.Dimensions(tiles[i].renderW, 720, tiles[i].evalOutW, 2160);
                assert(p.numbers.at("DLSS.Output.Subrect.Base.X") == 19 + tiles[i].outX);
                assert(p.numbers.at("MV.Scale.X") == 3841);
            }
        }();
        for (const auto& [key, value] : mixedNumbers) assert(p.numbers.at(key) == value);
        assert(p.resources == mixedResources);
    }
    std::puts("PASS: mixed direct core/copied guides preserve addressing and restore after each tile.");
    std::puts("PASS: isolated RR input/dimension restoration after every binding failure; original sources retained.");
}

template <typename Resource> void CheckRestoration()
{
    Parameters p;
    p.Set(NVSDK_NGX_Parameter_Output, reinterpret_cast<Resource*>(0x1000));
    p.Set(NVSDK_NGX_Parameter_Width, 3841u);
    p.Set(NVSDK_NGX_Parameter_Height, 720u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 41u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 17u);
    p.Set(NVSDK_NGX_Parameter_Reset, 1);
    for (size_t i = 0; i < DLSSTiling::kInputSubrectCount; ++i)
        p.Set(DLSSTiling::kInputSubrects[i].name, static_cast<unsigned int>(7 + i));
    const auto original = p.numbers;
    std::vector<DLSSTile> tiles;
    assert(DLSSTiling::BuildFrameTiles(3841, 11520, 3, tiles));
    for (bool lowRes : { false, true })
    {
        // Return after each possible failing tile to exercise scope cleanup.
        for (size_t failingTile = 0; failingTile < 3; ++failingTile)
        {
            auto evaluate = [&]() {
                DLSSTiling::EvaluationScope<Resource> scope(&p);
                for (size_t i = 0; i <= failingTile; ++i)
                {
                    const auto& t = tiles[i];
                    DLSSTiling::ApplyTile(&p, scope.base, t, 720, lowRes);
                    for (size_t j = 0; j < DLSSTiling::kInputSubrectCount; ++j)
                    {
                        const auto& subrect = DLSSTiling::kInputSubrects[j];
                        const auto offset = subrect.isMotionVector && !lowRes ? t.mvX : t.renderX;
                        assert(p.numbers.at(subrect.name) == 7 + j + offset);
                    }
                    p.Set(NVSDK_NGX_Parameter_Output, reinterpret_cast<Resource*>(0x2000));
                    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
                    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
                    p.Set(NVSDK_NGX_Parameter_Reset, 0);
                }
                return false;
            };
            assert(!evaluate());
            for (const auto& [key, value] : original)
                assert(p.numbers.at(key) == value);
            assert(p.resources.at(NVSDK_NGX_Parameter_Output) == reinterpret_cast<void*>(0x1000));
            assert(p.numbers.at(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == 3841);
            assert(p.numbers.at(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height) == 720);
        }
    }
}

int main()
{
    std::vector<DLSSTile> tiles;
    unsigned int cases = 0;
    double worstBoundaryError = 0;
    for (const unsigned int output : { 5760u, 7680u, 11520u, 12288u, 16383u })
    {
        for (unsigned int input = 3; input <= 16384; ++input)
        {
            assert(DLSSTiling::BuildFrameTiles(input, output, 3, tiles));
            assert(tiles.size() == 3);
            assert(tiles.front().renderX == 0);
            assert(tiles.back().renderX + tiles.back().renderW == input);
            assert(tiles.front().cropX == 0);
            assert(tiles.back().cropX + tiles.back().outW == tiles.back().evalOutW);
            unsigned int nextX = 0;
            for (size_t i = 0; i < tiles.size(); ++i)
            {
                const auto& t = tiles[i];
                assert(t.renderW > 0 && t.renderX + t.renderW <= input);
                assert(t.cropX + t.outW <= t.evalOutW);
                assert(t.mvX + t.evalOutW <= output);
                assert(t.evalOutW <= 8192);
                assert(t.outX == nextX);
                assert(t.renderW == tiles[0].renderW && t.evalOutW == tiles[0].evalOutW);
                if (input % 3 == 0)
                {
                    assert(t.renderX == input / 3 * i && t.renderW == input / 3);
                    assert(t.evalOutW == t.outW && t.cropX == 0 && t.mvX == t.outX);
                }
                if (input >= output / 3 && input <= output)
                {
                    // Map crop edges back into ideal full-frame output space.
                    for (const unsigned int edge : { 0u, t.outW })
                    {
                        const double mapped = (t.renderX + double(t.cropX + edge) * t.renderW / t.evalOutW)
                                              * output / input;
                        worstBoundaryError = (std::max)(worstBoundaryError, std::abs(mapped - (t.outX + edge)));
                    }
                }
                nextX += t.outW;
            }
            assert(nextX == output); // Exactly one destination region per monitor, no holes/overwrites.
            ++cases;
        }
    }
    for (const auto invalid : { 0u, 1u, 2u, 16385u, 0xffffffffu })
    {
        assert(!DLSSTiling::BuildFrameTiles(invalid, 11520, 3, tiles));
        assert(tiles.empty());
    }
    assert(!DLSSTiling::BuildFrameTiles(3841, 11521, 3, tiles));
    assert(!DLSSTiling::BuildFrameTiles(3841, 11520, 0, tiles));
    CheckRestoration<ID3D11Resource>();
    CheckRestoration<ID3D12Resource>();
    CheckRR();
    CheckRRCopyRestoration();
    assert(worstBoundaryError < 3.0);
    std::printf("PASS: %u geometries; both MV modes and API parameter scopes.\n", cases);
    std::printf("Maximum crop-edge position error in 1x-3x upscale range: %.3f output pixels.\n", worstBoundaryError);
}
