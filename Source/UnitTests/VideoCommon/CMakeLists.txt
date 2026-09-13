add_dolphin_test(VertexLoaderTest VertexLoaderTest.cpp)
add_dolphin_test(GenerationCounterTableTest GenerationCounterTableTest.cpp)

if(ENABLE_VULKAN)
  add_dolphin_test(VulkanSamplerDescriptorSetCacheTest VulkanSamplerDescriptorSetCacheTest.cpp)
  target_include_directories(VulkanSamplerDescriptorSetCacheTest PRIVATE
    ${CMAKE_SOURCE_DIR}/Externals/Vulkan-Headers/include)
endif()
