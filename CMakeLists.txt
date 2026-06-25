cmake_minimum_required(VERSION 3.30.2 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 26)

include(FetchContent)

project(vk-snapshot LANGUAGES CXX)

if(MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /permissive- /Zc:wchar_t /Zc:forScope /Zc:inline")
endif()

# ========================= Vulkan =========================

find_package(Vulkan REQUIRED)

# ========================= VMA =========================

FetchContent_Declare(
        vmahpp
        URL      https://github.com/YaaZ/VulkanMemoryAllocator-Hpp/releases/download/v3.3.0+3/VulkanMemoryAllocator-Hpp-3.3.0.tar.gz
        URL_HASH SHA256=8c2a0573babe1f86f3241d12b4c9df40bb944649b7f4239c1c611797f8af6cd5
)
FetchContent_MakeAvailable(vmahpp)

# ========================= GLFW =========================

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "GLFW build documentation" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "GLFW build documentation" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "GLFW build documentation" FORCE)

FetchContent_Declare(
        glfw
        GIT_REPOSITORY  https://github.com/glfw/glfw
        GIT_TAG         b00e6a8
)
FetchContent_MakeAvailable(glfw)

# ========================= ImGUI =========================

FetchContent_Declare(
        imgui
        GIT_REPOSITORY  https://github.com/ocornut/imgui
        GIT_TAG         6acba3b
)
FetchContent_MakeAvailable(imgui)

file(GLOB IMGUI_SRCS
        "${imgui_SOURCE_DIR}/*.h"
        "${imgui_SOURCE_DIR}/*.cpp"
)

set(IMGUI_IMPL_SRCS
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.h
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.h
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

add_library(imgui STATIC ${IMGUI_SRCS} ${IMGUI_IMPL_SRCS})

target_link_libraries(imgui PUBLIC Vulkan::Vulkan)

target_compile_options(imgui PRIVATE -w -DIMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING)

target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

# ========================= SPIRV-Reflect =========================

set(SPIRV_REFLECT_STATIC_LIB ON)

FetchContent_Declare(
        spirv-reflect
        GIT_REPOSITORY  https://github.com/KhronosGroup/SPIRV-Reflect
        GIT_TAG         c90b7b7
)
FetchContent_MakeAvailable(spirv-reflect)

# ========================= glm =========================

set(GLM_ENABLE_CXX_20           ON)
set(GLM_FORCE_DEPTH_ZERO_TO_ONE ON)
set(GLM_ENABLE_EXPERIMENTAL     ON)

FetchContent_Declare(
        glm
        GIT_REPOSITORY  https://github.com/g-truc/glm
        GIT_TAG         6f14f47
)
FetchContent_MakeAvailable(glm)

# ========================= General =========================

include_directories(.)

set(ALL_LIBS
        glfw
        glm::glm
        imgui
        spirv-reflect-static
        Vulkan::Vulkan
)

add_library(vk-snapshot SHARED "layer.cpp")
target_link_libraries(vk-snapshot PRIVATE ${ALL_LIBS})
