add_library(videod3d12
  D3D12BoundingBox.cpp
  D3D12BoundingBox.h
  D3D12PerfQuery.cpp
  D3D12PerfQuery.h
  D3D12Gfx.cpp
  D3D12Gfx.h
  D3D12StreamBuffer.cpp
  D3D12StreamBuffer.h
  D3D12SwapChain.cpp
  D3D12SwapChain.h
  D3D12VertexManager.cpp
  D3D12VertexManager.h
  DescriptorAllocator.cpp
  DescriptorAllocator.h
  DescriptorHeapManager.cpp
  DescriptorHeapManager.h
  DX12Context.cpp
  DX12Context.h
  DX12Pipeline.cpp
  DX12Pipeline.h
  DX12Shader.cpp
  DX12Shader.h
  DX12Texture.cpp
  DX12Texture.h
  DX12VertexFormat.cpp
  DX12VertexFormat.h
  VideoBackend.cpp
  VideoBackend.h
)

target_link_libraries(videod3d12
PUBLIC
  common
  videocommon
  videod3dcommon
)

if(MSVC)
  # Add precompiled header
  target_link_libraries(videod3d12 PRIVATE use_pch)
endif()

if(ENABLE_VR)
  target_sources(videod3d12 PRIVATE
    D3D12OpenXR.cpp
    D3D12OpenXR.h
    D3D12PrimedGunOverlay.cpp
    D3D12PrimedGunOverlay.h
  )
  target_link_libraries(videod3d12 PRIVATE openxr_loader)
  target_include_directories(videod3d12 PRIVATE "${CMAKE_SOURCE_DIR}/Externals/OpenXR/include")
endif()
