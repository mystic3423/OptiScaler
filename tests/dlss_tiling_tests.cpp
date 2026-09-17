// Standalone CPU checks; does not load NGX or exercise the GPU.
#include "../OptiScaler/upscalers/dlss/DLSSTiling.h"
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
    assert(worstBoundaryError < 3.0);
    std::printf("PASS: %u geometries; both MV modes and API parameter scopes.\n", cases);
    std::printf("Maximum crop-edge position error in 1x-3x upscale range: %.3f output pixels.\n", worstBoundaryError);
}
