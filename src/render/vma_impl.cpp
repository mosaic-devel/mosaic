// The single translation unit that compiles the Vulkan Memory Allocator implementation
// (vendored, MIT, third_party/vma). Everywhere else includes <vk_mem_alloc.h> for declarations
// only. VMA calls Vulkan entry points that the loader (Vulkan::Vulkan) provides at link time, so
// the static-function path is used and no function pointers need to be supplied.
//
// This file is compiled with warnings disabled (see src/render/CMakeLists.txt) -- it is
// third-party code, not ours to lint.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

#include <vulkan/vulkan.h>

#include <vk_mem_alloc.h>
