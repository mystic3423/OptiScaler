#include <pch.h>
#include "DLSSFeature_Dx11.h"
#include <Config.h>

#include <dxgi.h>

bool DLSSFeatureDx11::InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");

        SetInit(false);
        return false;
    }

    NVSDK_NGX_Result nvResult;
    bool initResult = false;

    do
    {
        if (!_dlssInited)
        {
            _dlssInited = NVNGXProxy::InitDx11(Device);

            if (!_dlssInited)
                return false;

            _moduleLoaded =
                (NVNGXProxy::D3D11_Init_ProjectID() != nullptr || NVNGXProxy::D3D11_Init_Ext() != nullptr) &&
                (NVNGXProxy::D3D11_Shutdown() != nullptr || NVNGXProxy::D3D11_Shutdown1() != nullptr) &&
                (NVNGXProxy::D3D11_GetParameters() != nullptr || NVNGXProxy::D3D11_AllocateParameters() != nullptr) &&
                NVNGXProxy::D3D11_DestroyParameters() != nullptr && NVNGXProxy::D3D11_CreateFeature() != nullptr &&
                NVNGXProxy::D3D11_ReleaseFeature() != nullptr && NVNGXProxy::D3D11_EvaluateFeature() != nullptr;

            // delay between init and create feature
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        LOG_INFO("Creating DLSS feature");

        if (NVNGXProxy::D3D11_CreateFeature() == nullptr)
        {
            LOG_ERROR("NVNGXProxy::D3D11_CreateFeature is nullptr");
            break;
        }

        ProcessInitParams(InParameters);

        // ---- split-frame tiling ---------------------------------------------
        // DLSS SR rejects an output width above 8192 on D3D11 exactly as it
        // does on D3D12: both APIs cap Texture2D at 16384 and DLSS derives its
        // limit as half of that. (Vulkan's cap is 32768, which is why the same
        // resolution passes there.) Create one feature per tile instead, each
        // narrow enough to pass, and let NGX subrects address the game's
        // full-frame textures at evaluate time.
        const unsigned int tileCount = DLSSTiling::TileCountFromEnv();
        _tiles.clear();
        _tileHandles.clear();

        if (tileCount > 1 && !DLSSTiling::BuildTiles(RenderWidth(), TargetWidth(), tileCount, _tiles))
        {
            LOG_ERROR("Tiling requested ({} tiles) but {}x -> {}x does not divide evenly; "
                      "falling back to single feature",
                      tileCount, RenderWidth(), TargetWidth());
        }

        if (!_tiles.empty())
        {
            LOG_INFO("Tiled DLSS: {} tiles, render {}x{} -> target {}x{} ({} per tile)", _tiles.size(), RenderWidth(),
                     RenderHeight(), TargetWidth(), TargetHeight(), _tiles[0].outW);

            // Must be set at create time or the output subrect base is ignored.
            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);

            bool tilesOk = true;

            for (size_t i = 0; i < _tiles.size(); i++)
            {
                // Each feature is sized to its tile, not to the whole frame.
                InParameters->Set(NVSDK_NGX_Parameter_Width, _tiles[i].renderW);
                InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
                InParameters->Set(NVSDK_NGX_Parameter_OutWidth, _tiles[i].outW);
                InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

                NVSDK_NGX_Handle* handle = nullptr;
                nvResult = NVNGXProxy::D3D11_CreateFeature()(InContext, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                             &handle);

                if (nvResult != NVSDK_NGX_Result_Success)
                {
                    LOG_ERROR("NVNGXProxy::D3D11_CreateFeature tile {} result: {:X}", i, (unsigned int) nvResult);

                    for (auto* h : _tileHandles)
                        if (h != nullptr && NVNGXProxy::D3D11_ReleaseFeature() != nullptr)
                            NVNGXProxy::D3D11_ReleaseFeature()(h);

                    _tileHandles.clear();
                    _tiles.clear();
                    tilesOk = false;
                    break;
                }

                LOG_INFO("NVNGXProxy::D3D11_CreateFeature tile {} ({}px wide): Success", i, _tiles[i].outW);
                _tileHandles.push_back(handle);
            }

            // Restore the whole-frame values the game expects to read back.
            InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
            InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
            InParameters->Set(NVSDK_NGX_Parameter_OutWidth, TargetWidth());
            InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

            if (!tilesOk)
                break;

            _p_dlssHandle = _tileHandles[0];
        }
        else
        {
            _p_dlssHandle = &_dlssHandle;
            nvResult = NVNGXProxy::D3D11_CreateFeature()(InContext, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("NVNGXProxy::D3D11_CreateFeature result: {0:X}", (unsigned int) nvResult);
                break;
            }
        }

        ReadVersion();

        initResult = true;

    } while (false);

    SetInit(initResult);

    return initResult;
}

bool DLSSFeatureDx11::EvaluateInternal(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D11_EvaluateFeature() == nullptr)
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
        {
            DLSSTiling::ApplyTile(InParameters, backup, _tiles[i], RenderHeight());

            nvResult = NVNGXProxy::D3D11_EvaluateFeature()(InDeviceContext, _tileHandles[i], InParameters, NULL);

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
        nvResult = NVNGXProxy::D3D11_EvaluateFeature()(InDeviceContext, _p_dlssHandle, InParameters, NULL);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
    }

    LOG_TRACE("_EvaluateFeature ok!");

    return true;
}

DLSSFeatureDx11::DLSSFeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx11::~DLSSFeatureDx11()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D11_ReleaseFeature() == nullptr)
        return;

    if (!_tileHandles.empty())
    {
        for (auto* h : _tileHandles)
            if (h != nullptr)
                NVNGXProxy::D3D11_ReleaseFeature()(h);

        _tileHandles.clear();
        _p_dlssHandle = nullptr;
    }
    else if (_p_dlssHandle != nullptr)
    {
        NVNGXProxy::D3D11_ReleaseFeature()(_p_dlssHandle);
    }
}
