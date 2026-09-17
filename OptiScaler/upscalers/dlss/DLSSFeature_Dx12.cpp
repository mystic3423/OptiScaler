#include <pch.h>
#include "DLSSFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>

bool DLSSFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSS(InCommandList, InParameters);
}

bool DLSSFeatureDx12::InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssInited)
    {
        _dlssInited = NVNGXProxy::InitDx12(Device);

        if (!_dlssInited)
            return false;

        _moduleLoaded =
            (NVNGXProxy::D3D12_Init_ProjectID() != nullptr || NVNGXProxy::D3D12_Init_Ext() != nullptr) &&
            (NVNGXProxy::D3D12_Shutdown() != nullptr || NVNGXProxy::D3D12_Shutdown1() != nullptr) &&
            (NVNGXProxy::D3D12_GetParameters() != nullptr || NVNGXProxy::D3D12_AllocateParameters() != nullptr) &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_CreateFeature() != nullptr &&
            NVNGXProxy::D3D12_ReleaseFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;

        // delay between init and create feature
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Creating DLSS feature");

    if (NVNGXProxy::D3D12_CreateFeature() == nullptr)
    {
        LOG_ERROR("_CreateFeature is nullptr");
        return false;
    }

    ProcessInitParams(InParameters);

    // ---- split-frame tiling -------------------------------------------------
    // DLSS SR on D3D12 rejects an output width above 8192. Create one feature
    // per tile instead, each narrow enough to pass, and let NGX subrects do the
    // addressing into the game's full-frame textures at evaluate time.
    const unsigned int tileCount = DLSSTiling::TileCountFromEnv();
    _tiles.clear();
    _tileHandles.clear();

    if (tileCount > 1 && !DLSSTiling::BuildTiles(RenderWidth(), TargetWidth(), tileCount, _tiles))
    {
        LOG_ERROR("Tiling requested ({} tiles) but {}x -> {}x does not divide evenly; falling back to single feature",
                  tileCount, RenderWidth(), TargetWidth());
    }

    if (!_tiles.empty())
    {
        LOG_INFO("Tiled DLSS: {} tiles, render {}x{} -> target {}x{} ({} per tile)", _tiles.size(), RenderWidth(),
                 RenderHeight(), TargetWidth(), TargetHeight(), _tiles[0].outW);

        // Must be set at create time or the output subrect base is ignored.
        InParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);

        for (size_t i = 0; i < _tiles.size(); i++)
        {
            // Each feature is sized to its tile, not to the whole frame.
            InParameters->Set(NVSDK_NGX_Parameter_Width, _tiles[i].renderW);
            InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
            InParameters->Set(NVSDK_NGX_Parameter_OutWidth, _tiles[i].outW);
            InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

            NVSDK_NGX_Handle* handle = nullptr;
            NVSDK_NGX_Result nvResult;
            {
                ScopedSkipHeapCapture skipHeapCapture {};
                nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling,
                                                             InParameters, &handle);
            }

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("_CreateFeature tile {} result: {:X}", i, (unsigned int) nvResult);

                for (auto* h : _tileHandles)
                    if (h != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
                        NVNGXProxy::D3D12_ReleaseFeature()(h);

                _tileHandles.clear();
                _tiles.clear();
                return false;
            }

            LOG_INFO("_CreateFeature tile {} ({}px wide): Success", i, _tiles[i].outW);
            _tileHandles.push_back(handle);
        }

        // Restore the whole-frame values the game expects to read back.
        InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
        InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
        InParameters->Set(NVSDK_NGX_Parameter_OutWidth, TargetWidth());
        InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

        _p_dlssHandle = _tileHandles[0];
    }
    else
    {
        _p_dlssHandle = &_dlssHandle;

        NVSDK_NGX_Result nvResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);
        }

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }

        LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success");
    }

    ReadVersion();

    SetInit(true);
    return true;
}

bool DLSSFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D12_EvaluateFeature() == nullptr)
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    ProcessEvaluateParams(InParameters);

    if (!_tileHandles.empty())
    {
        // One evaluate per tile, all reading the same untouched full-frame
        // color / depth / motion-vector textures. Because the subrect is in the
        // same coordinate space as the original frame, MV.Scale.X/Y stay as the
        // game set them - no per-tile rescaling.
        DLSSTiling::SubrectBackup backup;
        DLSSTiling::Save(InParameters, backup);

        for (size_t i = 0; i < _tileHandles.size(); i++)
        //for (size_t i = 0; i < 1; i++)
        {
            DLSSTiling::ApplyTile(InParameters, backup, _tiles[i], RenderHeight());

            nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _tileHandles[i], InParameters, NULL);

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("_EvaluateFeature tile {} result: {:X}", i, (unsigned int) nvResult);
                DLSSTiling::Restore(InParameters, backup);
                return false;
            }
        }

        DLSSTiling::Restore(InParameters, backup);
    }
    else
    {
        nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssHandle, InParameters, NULL);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
    }

    _frameCount++;

    return true;
}

void DLSSFeatureDx12::Shutdown(ID3D12Device* InDevice)
{
    if (_dlssInited)
    {
        if (NVNGXProxy::D3D12_Shutdown() != nullptr)
            NVNGXProxy::D3D12_Shutdown()();
        else if (NVNGXProxy::D3D12_Shutdown1() != nullptr)
            NVNGXProxy::D3D12_Shutdown1()(InDevice);
    }

    DLSSFeature::Shutdown();
}

DLSSFeatureDx12::DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx12::~DLSSFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D12_ReleaseFeature() == nullptr)
        return;

    if (!_tileHandles.empty())
    {
        for (auto* h : _tileHandles)
            if (h != nullptr)
                NVNGXProxy::D3D12_ReleaseFeature()(h);

        _tileHandles.clear();
        _p_dlssHandle = nullptr;
    }
    else if (_p_dlssHandle != nullptr)
    {
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssHandle);
    }
}
