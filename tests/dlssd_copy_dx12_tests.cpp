// Standalone WARP readback test of the actual debug-copy HLSL. No NGX/game.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "../OptiScaler/upscalers/dlssd/DLSSDTiling.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr)
{
    if (FAILED(hr)) { std::printf("HRESULT %08X\n", static_cast<unsigned>(hr)); throw std::runtime_error("D3D12 failure"); }
}
static void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    cmd->ResourceBarrier(1, &b);
}
int main()
try
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages;
    if (debug) Check(device.As(&messages));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
    ComPtr<ID3DBlob> shader, error, serialized;
    Check(D3DCompile(DLSSDTiling::DebugCopyShader, sizeof(DLSSDTiling::DebugCopyShader) - 1,
                    nullptr, nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &shader, &error));
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    for (UINT i = 0; i < 2; ++i) { ranges[i].NumDescriptors = 1; ranges[i].OffsetInDescriptorsFromTableStart = i; }
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, 4 };
    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 2; rootDesc.pParameters = params;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
    pipelineDesc.pRootSignature = root.Get();
    pipelineDesc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    ComPtr<ID3D12PipelineState> pipeline;
    Check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)));
    auto create = [&](D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = type; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)));
        return r;
    };
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return create(desc, type, state);
    };
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEvent failed");
    UINT64 serial = 0;
    // Padded source, nonzero caller X/Y origins, three disjoint tiles. Formats
    // cover Cyberpunk color/MVs/normals, albedos, hit distance, depth, SSS and bias.
    const DXGI_FORMAT formats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS,
                                   DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16_FLOAT,
                                   DXGI_FORMAT_R8_UNORM };
    for (const auto format : formats)
    {
        const UINT bpp = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 :
                         format == DXGI_FORMAT_R16_FLOAT ? 2 : format == DXGI_FORMAT_R8_UNORM ? 1 : 4;
        const DXGI_FORMAT viewFormat = format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : format;
        D3D12_RESOURCE_DESC srcDesc {}; srcDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        srcDesc.Width = 45; srcDesc.Height = 7; srcDesc.DepthOrArraySize = srcDesc.MipLevels = 1;
        srcDesc.SampleDesc.Count = 1; srcDesc.Format = format;
        if (format == DXGI_FORMAT_R32_TYPELESS) srcDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        auto source = create(srcDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT srcLayout {}; UINT64 srcBytes;
        device->GetCopyableFootprints(&srcDesc, 0, 1, 0, &srcLayout, nullptr, nullptr, &srcBytes);
        auto upload = buffer(srcBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        std::vector<unsigned char> expected(static_cast<size_t>(srcBytes));
        for (UINT y = 0; y < 7; ++y) for (UINT x = 0; x < 45; ++x)
        {
            auto* pixel = expected.data() + y * srcLayout.Footprint.RowPitch + x * bpp;
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
                for (UINT c = 0; c < 4; ++c) pixel[c] = static_cast<unsigned char>((x * 7 + y * 19 + c * 41) % 256);
            else if (format == DXGI_FORMAT_R8_UNORM)
                pixel[0] = static_cast<unsigned char>((x * 7 + y * 19) % 256);
            else if (format == DXGI_FORMAT_R16_FLOAT)
            {
                // Finite half values including zero, varying signs/exponents and
                // mantissas; exact readback detects scalar guide corruption.
                const UINT index = x + y * 45;
                const unsigned short half = index % 17 == 0 ? 0 : static_cast<unsigned short>(
                    ((index % 2) << 15) | ((1 + index % 30) << 10) | ((index * 13) % 1024));
                std::memcpy(pixel, &half, 2);
            }
            else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                for (UINT c = 0; c < 4; ++c)
                {
                    const unsigned short half = static_cast<unsigned short>(0x3800 + x * 9 + y * 17 + c * 13);
                    std::memcpy(pixel + c * 2, &half, 2);
                }
            }
            else if (format == DXGI_FORMAT_R11G11B10_FLOAT)
            {
                // Finite, exactly representable packed HDR values, varying all
                // channels/exponents so Load->UAV must preserve their bits.
                const UINT index = x + y * 45;
                const UINT r = ((1 + index % 29) << 6) | (index % 64);
                const UINT g = ((1 + (index * 3) % 29) << 6) | ((index * 7) % 64);
                const UINT b = ((1 + (index * 5) % 29) << 5) | ((index * 11) % 32);
                const UINT packed = r | (g << 11) | (b << 22);
                std::memcpy(pixel, &packed, 4);
            }
            else { const float value = (x + y * 45 + 0.25f) / 512; std::memcpy(pixel, &value, 4); }
        }
        void* mapped; D3D12_RANGE noRead { 0, 0 };
        Check(upload->Map(0, &noRead, &mapped)); std::memcpy(mapped, expected.data(), expected.size()); upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = srcLayout;
        dst.pResource = source.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmd.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_RESOURCE_DESC outDesc = srcDesc; outDesc.Width = 12; outDesc.Height = 4;
        outDesc.Format = viewFormat; outDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT outLayout {}; UINT64 outBytes;
        device->GetCopyableFootprints(&outDesc, 0, 1, 0, &outLayout, nullptr, nullptr, &outBytes);
        ComPtr<ID3D12Resource> outputs[3], readbacks[3];
        ComPtr<ID3D12DescriptorHeap> heaps[3];
        for (UINT tile = 0; tile < 3; ++tile)
        {
            outputs[tile] = create(outDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            readbacks[tile] = buffer(outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heaps[tile])));
            auto cpu = heaps[tile]->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv {}; srv.Format = viewFormat;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(source.Get(), &srv, cpu);
            cpu.ptr += device->GetDescriptorHandleIncrementSize(hd.Type);
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav {}; uav.Format = viewFormat; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(outputs[tile].Get(), nullptr, &uav, cpu);
            auto* heap = heaps[tile].Get(); cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetComputeRootSignature(root.Get()); cmd->SetPipelineState(pipeline.Get());
            cmd->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
            const UINT region[] = { 3 + tile * 12, 2, 12, 4 };
            cmd->SetComputeRoot32BitConstants(1, 4, region, 0); cmd->Dispatch(2, 1, 1);
            Barrier(cmd.Get(), outputs[tile].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            src = {}; dst = {}; src.pResource = outputs[tile].Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = readbacks[tile].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = outLayout;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Check(cmd->Close()); ID3D12CommandList* lists[] = { cmd.Get() }; queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
        for (UINT tile = 0; tile < 3; ++tile)
        {
            Check(readbacks[tile]->Map(0, nullptr, &mapped));
            for (UINT y = 0; y < 4; ++y)
                if (std::memcmp(static_cast<unsigned char*>(mapped) + y * outLayout.Footprint.RowPitch,
                                expected.data() + (y + 2) * srcLayout.Footprint.RowPitch + (3 + tile * 12) * bpp,
                                12 * bpp) != 0) throw std::runtime_error("Pixel mismatch");
            readbacks[tile]->Unmap(0, &noRead);
        }
        std::printf("PASS: WARP format %u, three distinct tiles, padded source/nonzero XY bases, exact pixel bytes.\n", format);
        Check(allocator->Reset()); Check(cmd->Reset(allocator.Get(), nullptr));
    }
    Check(cmd->Close()); CloseHandle(event);
    if (messages)
    {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            Check(messages->GetMessage(i, nullptr, &bytes));
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(messages->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                std::printf("D3D12 validation: %s\n", message->pDescription);
                throw std::runtime_error("D3D12 validation warning/error");
            }
        }
        std::puts("PASS: D3D12 debug layer reported no warnings/errors.");
    }
    else std::puts("D3D12 debug layer unavailable; readback validation only.");
    std::puts("No NGX or game rendering exercised.");
    return 0;
}
catch (const std::exception& error) { std::printf("FAIL: %s\n", error.what()); return 1; }
