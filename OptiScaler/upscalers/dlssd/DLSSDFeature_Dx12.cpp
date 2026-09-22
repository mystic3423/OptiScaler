#include <pch.h>
#include "DLSSDFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>

namespace
{
// Deliberately narrow debug contract: float/UNORM guides, with Cyberpunk's
// R32_TYPELESS depth exposed as R32_FLOAT. Never guess integer/sRGB packing.
DXGI_FORMAT DebugCopyFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
}

bool DLSSDFeatureDx12::InitDebugCopy()
{
    if (_debugCopyPipeline)
        return true;
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    shader.Attach(CompileShader(DLSSDTiling::DebugCopyShader, "CSMain", "cs_5_0"));
    if (!shader)
        return false;
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    for (UINT i = 0; i < 2; ++i)
    {
        ranges[i].NumDescriptors = 1;
        ranges[i].OffsetInDescriptorsFromTableStart = i;
    }
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, 4 };
    D3D12_ROOT_SIGNATURE_DESC root {};
    root.NumParameters = 2;
    root.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    auto result = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(result))
        result = Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&_debugCopyRoot));
    if (SUCCEEDED(result))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = _debugCopyRoot.Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        result = Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_debugCopyPipeline));
    }
    if (FAILED(result))
        LOG_ERROR("RR DEBUG isolated copy pipeline failed: {:X}", static_cast<unsigned int>(result));
    return SUCCEEDED(result);
}

ID3D12Resource* DLSSDFeatureDx12::DebugTextureFor(size_t tile, const char* key, DXGI_FORMAT format,
                                                unsigned int width, unsigned int height, bool output)
{
    for (const auto& entry : _debugTextures)
    {
        const auto desc = entry.resource->GetDesc();
        if (entry.tile == tile && entry.key == key && desc.Format == format &&
            desc.Width == width && desc.Height == height)
            return entry.resource.Get();
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format };
    if (format == DXGI_FORMAT_UNKNOWN ||
        FAILED(Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
    {
        LOG_ERROR("RR DEBUG unsupported copy format {} for {}", static_cast<unsigned int>(format), key);
        return nullptr;
    }
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    const auto result = Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(result))
    {
        LOG_ERROR("RR DEBUG copy allocation failed for {}: {:X}", key, static_cast<unsigned int>(result));
        return nullptr;
    }
    const auto label = std::string("RR DEBUG tile ") + std::to_string(tile) + " " + key;
    resource->SetName(std::wstring(label.begin(), label.end()).c_str());
    LOG_INFO("RR DEBUG isolated allocation tile={} {} {}x{} format={} ptr={}", tile, key, width, height,
             static_cast<unsigned int>(format), static_cast<void*>(resource.Get()));
    _debugTextures.push_back({ tile, key, resource });
    return resource.Get();
}

bool DLSSDFeatureDx12::CopyDebugInput(ID3D12GraphicsCommandList* commands,
                                     const DLSSDTiling::CopyScope::Input& input,
                                     ID3D12Resource* destination, const DLSSTile& tile)
{
    ID3D12DescriptorHeap* heap = nullptr;
    for (const auto& entry : _debugDescriptors)
        if (entry.source.Get() == input.source && entry.destination.Get() == destination)
        {
            heap = entry.heap.Get();
            break;
        }
    if (!heap)
    {
        // Immutable descriptors: no frame-ring overwrite while GPU work is in
        // flight. Retain source refs too, so pointer reuse cannot hit stale views.
        DebugDescriptors entry;
        entry.source = input.source;
        entry.destination = destination;
        D3D12_DESCRIPTOR_HEAP_DESC desc {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 2;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const auto result = Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&entry.heap));
        if (FAILED(result))
        {
            LOG_ERROR("RR DEBUG descriptor allocation failed: {:X}", static_cast<unsigned int>(result));
            return false;
        }
        auto cpu = entry.heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DebugCopyFormat(input.source->GetDesc().Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        Device->CreateShaderResourceView(input.source, &srv, cpu);
        cpu.ptr += Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = destination->GetDesc().Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Device->CreateUnorderedAccessView(destination, nullptr, &uav, cpu);
        heap = entry.heap.Get();
        _debugDescriptors.push_back(std::move(entry));
    }
    // NGX inputs are already readable at this boundary. Load them as SRVs,
    // including depth, without changing the game's resource states. Only our
    // own textures transition. No filtering, rescaling or MV value conversion.
    ResourceBarrier(commands, destination, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->SetDescriptorHeaps(1, &heap);
    commands->SetComputeRootSignature(_debugCopyRoot.Get());
    commands->SetPipelineState(_debugCopyPipeline.Get());
    commands->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    const unsigned int region[] = { input.x + tile.renderX, input.y, tile.renderW, RenderHeight() };
    commands->SetComputeRoot32BitConstants(1, 4, region, 0);
    commands->Dispatch((tile.renderW + 7) / 8, (RenderHeight() + 7) / 8, 1);
    ResourceBarrier(commands, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

ID3D12Resource* DLSSDFeatureDx12::CropOutput(ID3D12Resource* output, unsigned int width, unsigned int height)
{
    const auto original = output->GetDesc();
    for (const auto& cached : _cropOutputs)
    {
        const auto desc = cached->GetDesc();
        if (desc.Width == width && desc.Height == height && desc.Format == original.Format)
            return cached.Get();
    }
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = original.Format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
    const auto result = Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&scratch));
    if (FAILED(result))
    {
        LOG_ERROR("Integer-crop RR output allocation failed: {:X}", (unsigned int) result);
        return nullptr;
    }
    _cropOutputs.push_back(scratch);
    return scratch.Get();
}

namespace
{
template <typename T> void LogRRParameter(NVSDK_NGX_Parameter* parameters, const char* name)
{
    T value {};
    const auto result = parameters->Get(name, &value);
    if (result == NVSDK_NGX_Result_Success)
        LOG_DEBUG("RR diagnostic: {} = {}", name, value);
    else
        LOG_DEBUG("RR diagnostic: {} unavailable (Get=0x{:X})", name, static_cast<unsigned int>(result));
}
}

void DLSSDFeatureDx12::LogDiagnostics(NVSDK_NGX_Parameter* parameters, const char* phase, bool resources)
{
    if (!spdlog::should_log(spdlog::level::debug))
        return;

    LOG_DEBUG("RR diagnostic: {} OptiHandle={} nativeHandle={} frame={} render={}x{} target={}x{} "
              "LowResMV={} HDR={} DepthInverted={} JitteredMV={} AutoExposure={} tiles={}",
              phase, Handle()->Id, static_cast<void*>(_p_dlssdHandle), _frameCount, RenderWidth(), RenderHeight(),
              TargetWidth(), TargetHeight(), LowResMV(), IsHdr(), DepthInverted(), JitteredMV(), AutoExposure(), _tileHandles.size());

    for (const char* key : { NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
                            NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
                            NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
                            NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height })
        LogRRParameter<unsigned int>(parameters, key);
    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                            NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
                            "DLSS.Denoise.Mode", "DLSS.Roughness.Mode", "DLSS.Use.HW.Depth",
                            NVSDK_NGX_Parameter_Reset })
        LogRRParameter<int>(parameters, key);

    if (!resources)
        return;

    for (const char* key : { NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y,
                            NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y,
                            NVSDK_NGX_Parameter_DLSS_Pre_Exposure, NVSDK_NGX_Parameter_DLSS_Exposure_Scale })
        LogRRParameter<float>(parameters, key);

    // RR-only names follow NVIDIA's nvsdk_ngx_defs_dlssd.h. This checkout's older
    // SDK lacks that header. Missing keys remain distinct from supplied zeroes.
    struct ResourceParameter { const char* name; const char* subrect; };
    const ResourceParameter inputs[] = {
        { NVSDK_NGX_Parameter_Color, "DLSS.Input.Color" },
        { NVSDK_NGX_Parameter_Output, "DLSS.Output" },
        { NVSDK_NGX_Parameter_Depth, "DLSS.Input.Depth" },
        { NVSDK_NGX_Parameter_MotionVectors, "DLSS.Input.MV" },
        { "DLSS.Input.DiffuseAlbedo", "DLSS.Input.DiffuseAlbedo" },
        { "DLSS.Input.SpecularAlbedo", "DLSS.Input.SpecularAlbedo" },
        { NVSDK_NGX_Parameter_GBuffer_Normals, "DLSS.Input.Normals" },
        { NVSDK_NGX_Parameter_GBuffer_Roughness, "DLSS.Input.Roughness" },
        { "DLSSD.SpecularHitDistance", "DLSSD.SpecularHitDistance" },
        { "DLSSD.DiffuseHitDistance", "DLSSD.DiffuseHitDistance" },
        { NVSDK_NGX_Parameter_MotionVectorsReflection, nullptr },
        { NVSDK_NGX_Parameter_ExposureTexture, nullptr },
        { "DLSSD.ScreenSpaceSubsurfaceScatteringGuide", "DLSSD.ScreenSpaceSubsurfaceScatteringGuide" },
        { "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering", "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering" },
        { "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering", "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering" },
        { "DLSSD.ColorBeforeTransparency", "DLSSD.ColorBeforeTransparency" },
        { "DLSSD.DepthOfFieldGuide", "DLSSD.DepthOfFieldGuide" },
    };
    for (const auto& input : inputs)
    {
        ID3D12Resource* resource = nullptr;
        const auto result = parameters->Get(input.name, &resource);
        LOG_DEBUG("RR diagnostic: resource {} Get=0x{:X} ptr={}", input.name,
                  static_cast<unsigned int>(result), static_cast<void*>(resource));
        if (result != NVSDK_NGX_Result_Success || resource == nullptr)
            continue;
        const auto desc = resource->GetDesc();
        // Allocation size is not proof of the active sampling region.
        LOG_DEBUG("RR diagnostic: allocation={}x{} dimension={} format={} layers={} mips={} samples={} flags=0x{:X}",
                  desc.Width, desc.Height, static_cast<unsigned int>(desc.Dimension),
                  static_cast<unsigned int>(desc.Format), desc.DepthOrArraySize, desc.MipLevels,
                  desc.SampleDesc.Count, static_cast<unsigned int>(desc.Flags));
        if (input.subrect != nullptr)
        {
            const std::string base(input.subrect);
            LogRRParameter<unsigned int>(parameters, (base + ".Subrect.Base.X").c_str());
            LogRRParameter<unsigned int>(parameters, (base + ".Subrect.Base.Y").c_str());
        }
    }

    for (const char* key : { "WorldToViewMatrix", "ViewToClipMatrix" })
    {
        void* matrix = nullptr;
        const auto result = parameters->Get(key, &matrix);
        LOG_DEBUG("RR diagnostic: {} Get=0x{:X} ptr={}", key, static_cast<unsigned int>(result), matrix);
        if (result == NVSDK_NGX_Result_Success && matrix != nullptr)
        {
            const auto values = static_cast<const float*>(matrix);
            for (unsigned int row = 0; row < 4; ++row)
                LOG_DEBUG("RR diagnostic: {} storage row {} = [{}, {}, {}, {}]", key, row,
                          values[row * 4], values[row * 4 + 1], values[row * 4 + 2], values[row * 4 + 3]);
        }
    }
}

bool DLSSDFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSSD(InCommandList, InParameters);
}

bool DLSSDFeatureDx12::InitDLSSD(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssdInited)
    {
        _dlssdInited = NVNGXProxy::InitDx12(Device);

        if (!_dlssdInited)
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

    LOG_INFO("Creating DLSSD feature");

    if (NVNGXProxy::D3D12_CreateFeature() != nullptr)
    {
        LogDiagnostics(InParameters, "create incoming", false);
        ProcessInitParams(InParameters);
        LogDiagnostics(InParameters, "create effective", false);

        int originalOutputSubrects = 0;
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, &originalOutputSubrects);
        _tileOutputHeight = TargetHeight();
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &_tileCreateFlags);
        InParameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &_tileQuality);

        // Create the direct-output set at nominal integer dimensions. On an
        // uneven first frame this set is retained for later divisible frames;
        // evaluation creates the expanded set and uses the full active input.
        // Keep ordinary-sized outputs on the single-feature path.
        const unsigned int tileCount = TargetWidth() < 8192 ? 1 : DLSSTiling::TileCountFromEnv();
        _tiles.clear();
        _tileHandles.clear();

        if (tileCount > 1 && !DLSSTiling::BuildTiles((RenderWidth() / tileCount) * tileCount, TargetWidth(), tileCount, _tiles))
        {
            LOG_ERROR("Tiling requested ({} tiles) but {}x -> {}x cannot form a valid output layout; falling back to single feature",
                      tileCount, RenderWidth(), TargetWidth());
        }

        if (!_tiles.empty())
        {
            DLSSTiling::EvaluationScope<ID3D12Resource> createScope(InParameters);
            LOG_INFO("Tiled RR: {} tiles, render {}x{} -> target {}x{} ({} per tile)", _tiles.size(), RenderWidth(),
                     RenderHeight(), TargetWidth(), TargetHeight(), _tiles[0].outW);

            // Direct output requires subrect support at feature creation. The
            // reference diagnostic instead uses private tile-sized outputs.
            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
                              DLSSDTiling::DebugIsolatedTextures && DLSSDTiling::DebugPrivateOutput ? 0 : 1);
            if (DLSSDTiling::DebugIsolatedTextures)
                LOG_WARN("RR DEBUG INPUT EXPERIMENT: copyCoreInputs={}, copyMotionVectors={}, copyMaterialGuides={}, direct-layout privateOutput={}, "
                         "tile-local projection; temporary diagnostic, not the final tiling path",
                         DLSSDTiling::DebugCopyCoreInputs, DLSSDTiling::DebugCopyMotionVectors,
                         DLSSDTiling::DebugCopyMaterialGuides,
                         DLSSDTiling::DebugPrivateOutput);

            bool tilesOk = true;

            for (size_t i = 0; i < _tiles.size(); i++)
            {
                // Each feature is sized to its tile, not to the whole frame.
                InParameters->Set(NVSDK_NGX_Parameter_Width, _tiles[i].renderW);
                InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
                InParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, _tiles[i].renderW);
                InParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, RenderHeight());
                InParameters->Set(NVSDK_NGX_Parameter_OutWidth, _tiles[i].outW);
                InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

                NVSDK_NGX_Handle* handle = nullptr;
                NVSDK_NGX_Result nvResult;
                {
                    ScopedSkipHeapCapture skipHeapCapture {};
                    nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_RayReconstruction,
                                                                 InParameters, &handle);
                }

                if (nvResult != NVSDK_NGX_Result_Success || handle == nullptr ||
                    std::find(_tileHandles.begin(), _tileHandles.end(), handle) != _tileHandles.end())
                {
                    LOG_ERROR("_CreateFeature tile {} result: {:X}", i, (unsigned int) nvResult);

                    if (handle && std::find(_tileHandles.begin(), _tileHandles.end(), handle) == _tileHandles.end())
                        NVNGXProxy::D3D12_ReleaseFeature()(handle);
                    for (auto* h : _tileHandles)
                        if (h != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
                            NVNGXProxy::D3D12_ReleaseFeature()(h);

                    _tileHandles.clear();
                    _tiles.clear();
                    tilesOk = false;
                    break;
                }

                LOG_INFO("_CreateFeature tile {} ({}px wide): Success, RR handle {}", i, _tiles[i].outW, handle->Id);
                _tileHandles.push_back(handle);
            }

            // Restore the whole-frame values the game expects to read back.
            InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
            InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
            InParameters->Set(NVSDK_NGX_Parameter_OutWidth, TargetWidth());
            InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, originalOutputSubrects);
            if (!tilesOk)
                return false;

            _p_dlssdHandle = _tileHandles[0];
        }
        else
        {
            _p_dlssdHandle = nullptr;

            NVSDK_NGX_Result nvResult;
            {
                ScopedSkipHeapCapture skipHeapCapture {};

                nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_RayReconstruction, InParameters,
                                                             &_p_dlssdHandle);
            }

            if (nvResult != NVSDK_NGX_Result_Success || _p_dlssdHandle == nullptr)
            {
                LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
                return false;
            }

            LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success");
        }

    }
    else
    {
        LOG_ERROR("_CreateFeature is nullptr");
        return false;
    }

    ReadVersion();

    SetInit(true);
    return true;
}

bool DLSSDFeatureDx12::ValidateTileInputs(NVSDK_NGX_Parameter* parameters)
{
    // These legacy inputs have no audited subrect contract in this prototype.
    // Reject them instead of silently sampling tile zero on every monitor.
    for (const char* key : { "MotionVectorsReflection", "MotionVectors3D", "DepthHighRes",
                            "Position.ViewSpace", "RayTracingHitDistance", "DLSSD.OutputAlpha" })
    {
        ID3D12Resource* resource = nullptr;
        if (parameters->Get(key, &resource) == NVSDK_NGX_Result_Success && resource)
        {
            LOG_ERROR("Tiled RR does not yet support supplied input/output {}", key);
            return false;
        }
    }
    if (!LowResMV())
    {
        LOG_ERROR("Tiled RR currently requires render-resolution motion vectors");
        return false;
    }
    auto check = [&](const char* key, const char* base, unsigned int width, unsigned int height, bool required) {
        ID3D12Resource* resource = nullptr;
        if (parameters->Get(key, &resource) != NVSDK_NGX_Result_Success || !resource)
        {
            if (required)
                LOG_ERROR("Tiled RR missing required resource {}", key);
            return !required;
        }
        unsigned int x = 0, y = 0;
        parameters->Get((std::string(base) + ".Subrect.Base.X").c_str(), &x);
        parameters->Get((std::string(base) + ".Subrect.Base.Y").c_str(), &y);
        const auto desc = resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
            desc.SampleDesc.Count != 1 || static_cast<uint64_t>(x) + width > desc.Width ||
            static_cast<uint64_t>(y) + height > desc.Height)
        {
            LOG_ERROR("Tiled RR {} active region ({},{}) {}x{} exceeds/does not match allocation {}x{}",
                      key, x, y, width, height, desc.Width, desc.Height);
            return false;
        }
        return true;
    };
    if (!check("Color", "DLSS.Input.Color", RenderWidth(), RenderHeight(), true) ||
        !check("Depth", "DLSS.Input.Depth", RenderWidth(), RenderHeight(), true) ||
        !check("MotionVectors", "DLSS.Input.MV", RenderWidth(), RenderHeight(), true) ||
        !check("Output", "DLSS.Output", TargetWidth(), TargetHeight(), true))
        return false;
    for (const auto& guide : DLSSDTiling::guides)
        if (!check(guide.resource, guide.base, RenderWidth(), RenderHeight(), false))
            return false;
    const DLSSDTiling::Guide common[] = {
        { "DLSS.Input.Bias.Current.Color.Mask", "DLSS.Input.Bias.Current.Color" },
        { "TransparencyMask", "DLSS.Input.Translucency" },
        { "DLSS.TransparencyLayer", "DLSS.TransparencyLayer" },
        { "DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerOpacity" },
        { "DLSS.TransparencyLayerMvecs", "DLSS.TransparencyLayerMvecs" },
        { "DLSS.DisocclusionMask", "DLSS.DisocclusionMask" },
    };
    for (const auto& guide : common)
        if (!check(guide.resource, guide.base, RenderWidth(), RenderHeight(), false))
            return false;
    void* projection = nullptr;
    void* view = nullptr;
    parameters->Get("ViewToClipMatrix", &projection);
    parameters->Get("WorldToViewMatrix", &view);
    if (!projection || !view)
    {
        LOG_ERROR("Tiled RR requires camera matrices for the hit-distance path");
        return false;
    }
    return true;
}

bool DLSSDFeatureDx12::EvaluateTiles(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Parameter* parameters)
{
    NVSDK_NGX_Result nvResult;
    if (!_tileHandles.empty())
    {
        const bool previousValid = _tileHistoryValid;
        _tileHistoryValid = false; // Any early return forces a reset on the next complete frame.
        std::vector<DLSSTile> frameTiles;
        if (RenderHeight() == 0 || TargetHeight() != _tileOutputHeight ||
            TargetWidth() != _tiles[0].outW * _tileHandles.size() ||
            !DLSSTiling::BuildFrameTiles(RenderWidth(), TargetWidth(),
                                        static_cast<unsigned int>(_tileHandles.size()), frameTiles))
        {
            LOG_ERROR("Invalid current tiled RR geometry: {}x{} -> {}x{}",
                      RenderWidth(), RenderHeight(), TargetWidth(), TargetHeight());
            return false;
        }

        const bool cropped = frameTiles[0].evalOutW != frameTiles[0].outW;
        const bool privateOutput = cropped ||
                                   (DLSSDTiling::DebugIsolatedTextures && DLSSDTiling::DebugPrivateOutput);
        const bool reset = !previousValid || cropped != _lastTilesCropped ||
                           (RenderWidth() != _lastTileRenderW || RenderHeight() != _lastTileRenderH);
        if (!ValidateTileInputs(parameters))
            return false;
        DLSSTiling::EvaluationScope<ID3D12Resource> scope(parameters);
        DLSSDTiling::EvaluationScope rrScope(parameters, DLSSDTiling::DebugIsolatedTextures);
        std::unique_ptr<DLSSDTiling::CopyScope> copies;
        if (DLSSDTiling::DebugIsolatedTextures)
        {
            copies = std::make_unique<DLSSDTiling::CopyScope>(parameters);
            // Check the complete input set before submitting any copy commands.
            for (const auto& input : copies->inputs)
            {
                if (!DLSSDTiling::CopyInput(input.key))
                    continue;
                const auto desc = input.source->GetDesc();
                if (DebugCopyFormat(desc.Format) == DXGI_FORMAT_UNKNOWN ||
                    (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
                {
                    LOG_ERROR("RR DEBUG unsupported input {} format={} flags={}", input.key,
                              static_cast<unsigned int>(desc.Format), static_cast<unsigned int>(desc.Flags));
                    return false;
                }
            }
            if (!InitDebugCopy())
                return false;
        }
        ID3D12Resource* scratch = nullptr;
        if (cropped || copies)
        {
            if (scope.output == nullptr)
                return false;
            const auto desc = scope.output->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
                desc.SampleDesc.Count != 1 || desc.MipLevels != 1)
                return false;
            if (static_cast<uint64_t>(scope.base.outX) + TargetWidth() > desc.Width ||
                static_cast<uint64_t>(scope.base.outY) + TargetHeight() > desc.Height)
            {
                LOG_ERROR("Integer-crop RR destination subrect exceeds output texture");
                return false;
            }
            if (!copies)
            {
                scratch = CropOutput(scope.output, frameTiles[0].evalOutW, TargetHeight());
                if (scratch == nullptr)
                    return false;
            }
            if (cropped && !EnsureCroppedTileHandles(parameters, frameTiles, [&](NVSDK_NGX_Handle** handle) {
                ScopedSkipHeapCapture skipHeapCapture {};
                return NVNGXProxy::D3D12_CreateFeature()(commands, NVSDK_NGX_Feature_RayReconstruction,
                                                          parameters, handle);
            }, [](NVSDK_NGX_Handle* handle) { NVNGXProxy::D3D12_ReleaseFeature()(handle); }))
                return false;
        }

        const auto& handles = cropped ? _croppedTileHandles : _tileHandles;
        for (size_t i = 0; i < handles.size(); ++i)
        {
            const auto& tile = frameTiles[i];
            DLSSTiling::ApplyTile(parameters, scope.base, tile, RenderHeight(), LowResMV());
            rrScope.Apply(tile, RenderWidth());
            parameters->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : scope.base.reset);
            if (copies)
            {
                for (const auto& input : copies->inputs)
                {
                    if (!DLSSDTiling::CopyInput(input.key))
                    {
                        // Core/guide scopes already applied caller base + tile
                        // origin. Keep the original resource and its Y base.
                        if (_frameCount == 0)
                            LOG_INFO("RR DEBUG direct input tile={} {} base=({}, {}) active={}x{}",
                                     i, input.key, input.x + tile.renderX, input.y, tile.renderW, RenderHeight());
                        continue;
                    }
                    auto* local = DebugTextureFor(i, input.key, DebugCopyFormat(input.source->GetDesc().Format),
                                                  tile.renderW, RenderHeight(), false);
                    if (!local || !CopyDebugInput(commands, input, local, tile))
                        return false;
                    copies->Bind(input, local);
                    if (_frameCount == 0)
                        LOG_INFO("RR DEBUG copy tile={} {} source=({}, {}) size={}x{} localBase=(0,0)",
                                 i, input.key, input.x + tile.renderX, input.y, tile.renderW, RenderHeight());
                }
                if (privateOutput)
                {
                    scratch = DebugTextureFor(i, "Output", scope.output->GetDesc().Format,
                                               tile.evalOutW, TargetHeight(), true);
                    if (!scratch)
                        return false;
                }
                copies->Dimensions(tile.renderW, RenderHeight(), tile.evalOutW, TargetHeight());
            }
            if (_frameCount == 0)
                LOG_INFO("RR tile {} handle={} renderX={} renderW={} outputX={} evalOutW={} cropX={} reset={} projection={} isolatedCopies={} privateOutput={}",
                         i, handles[i]->Id, tile.renderX, tile.renderW, tile.outX, tile.evalOutW, tile.cropX, reset,
                         copies ? "tile-local" : "game", copies != nullptr, privateOutput);
            if (privateOutput)
            {
                parameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D12Resource*>(scratch));
                parameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
                parameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
            }
            if (_frameCount == 0)
                LogDiagnostics(parameters, "tile effective (native handle ID in RR tile line)", true);
            nvResult = NVNGXProxy::D3D12_EvaluateFeature()(commands, handles[i], parameters, nullptr);
            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("_EvaluateFeature tile {} result: {:X}", i, (unsigned int) nvResult);
                return false;
            }
            if (privateOutput)
            {
                // NGX output is UAV, as in the existing postprocessing path.
                ResourceBarrier(commands, scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
                ResourceBarrier(commands, scope.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION source {}, destination {};
                source.pResource = scratch;
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.pResource = scope.output;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                const D3D12_BOX box { tile.cropX, 0, 0, tile.cropX + tile.outW, TargetHeight(), 1 };
                commands->CopyTextureRegion(&destination, scope.base.outX + tile.outX,
                                                scope.base.outY, 0, &source, &box);
                ResourceBarrier(commands, scope.output, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ResourceBarrier(commands, scratch, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        _lastTilesCropped = cropped;
        _lastTileRenderW = RenderWidth();
        _lastTileRenderH = RenderHeight();
        _tileHistoryValid = true;
    }
    return true;
}

bool DLSSDFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
    {
        ProcessEvaluateParams(InParameters);

        // Bound snapshots even when a game changes its input size every frame.
        if (spdlog::should_log(spdlog::level::debug) && _diagnosticSnapshots < 8 &&
            (_diagnosticSnapshots == 0 || _diagnosticWidth != RenderWidth() || _diagnosticHeight != RenderHeight()))
        {
            LogDiagnostics(InParameters, "evaluate", true);
            _diagnosticWidth = RenderWidth();
            _diagnosticHeight = RenderHeight();
            ++_diagnosticSnapshots;
        }

        if (!_tileHandles.empty())
        {
            if (!EvaluateTiles(InCommandList, InParameters))
            {
                if (!_loggedEvaluationFailure)
                {
                    LogDiagnostics(InParameters, "first tiled evaluation failure (restored parameters)", true);
                    _loggedEvaluationFailure = true;
                }
                return false;
            }
            nvResult = NVSDK_NGX_Result_Success;
        }
        else
            nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssdHandle, InParameters, NULL);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            if (!_loggedEvaluationFailure)
            {
                LogDiagnostics(InParameters, "first evaluation failure", true);
                _loggedEvaluationFailure = true;
            }
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
        if (_frameCount == 0)
            LOG_INFO("RR first evaluation succeeded: OptiHandle={} nativeHandleId={}",
                     Handle()->Id, _p_dlssdHandle->Id);
    }
    else
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    _frameCount++;

    return true;
}

DLSSDFeatureDx12::DLSSDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters),
      DLSSDFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSDFeatureDx12::~DLSSDFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D12_ReleaseFeature() == nullptr)
        return;
    for (auto* handle : _croppedTileHandles)
        NVNGXProxy::D3D12_ReleaseFeature()(handle);
    if (!_tileHandles.empty())
    {
        for (auto* handle : _tileHandles)
            NVNGXProxy::D3D12_ReleaseFeature()(handle);
    }
    else if (_p_dlssdHandle)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssdHandle);
}
