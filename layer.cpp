#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_layer_settings.hpp>
#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_dispatch_table.h>

#define NOMINMAX 1
#include <Windows.h>

#define IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_vulkan.h>

#include <string>
#include <mutex>
#include <map>
#include <iostream>
#include <array>
#include <algorithm>
#include <vector>
#include <ranges>
#include <sstream>
#include <stacktrace>

#undef VK_LAYER_EXPORT
#ifdef _WIN32
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define VK_LAYER_EXPORT extern "C"
#endif

void print_variadic() {}

template<typename T, typename ...Ts>
void print_variadic(T&& arg, Ts&& ...args) {
    std::cout << std::forward<T>(arg);
    print_variadic(std::forward<Ts>(args)...);
}

// #define NDEBUG

#ifndef NDEBUG
#define PRINT_DEBUG_FN_ENTER(...) print_variadic("[VK_LAYER_Snapshot] ", __FUNCTION__, "(", __VA_ARGS__, ")\n");
#else
#define PRINT_DEBUG_FN_ENTER(...)
#endif

#ifndef NDEBUG
#define PRINT_ERROR(msg, ...) print_variadic("[VK_LAYER_Snapshot] [ERROR!] ", __FUNCTION__, "(", __VA_ARGS__, "): ", (msg), "\n");
#else
#define PRINT_ERROR(...)
#endif

#define CHECK_RESULT(result, msg, ...)              \
if (const VkResult r = (result); r != VK_SUCCESS) { \
    std::stringstream _ss;                          \
    _ss << msg << " [code: " << r << "]";           \
    PRINT_ERROR(_ss.str(), __VA_ARGS__);            \
}

#define INST_PROC_ADDR(fn) \
    reinterpret_cast<PFN_##fn>(vkGetInstanceProcAddr(GET_INSTANCE, ""#fn))

#define DEVICE_PROC_ADDR(fn) \
    reinterpret_cast<PFN_##fn>(vkGetDeviceProcAddr(GET_DEVICE, ""#fn))

#define LIST_OF_INSTANCE_HOOKS                      \
    ADD_HOOK(GetInstanceProcAddr)                   \
    ADD_HOOK(EnumerateInstanceLayerProperties)      \
    ADD_HOOK(EnumerateInstanceExtensionProperties)  \
    ADD_HOOK(EnumerateDeviceLayerProperties)        \
    ADD_HOOK(EnumerateDeviceExtensionProperties)    \
    ADD_HOOK(EnumeratePhysicalDevices)              \
    ADD_HOOK(CreateDevice)                          \
    ADD_HOOK(CreateInstance)                        \
    ADD_HOOK(DestroyInstance)

#define LIST_OF_DEVICE_HOOKS                        \
    ADD_HOOK(GetDeviceProcAddr)                     \
    ADD_HOOK(DestroyDevice)                         \
    ADD_HOOK(CmdDraw)                               \
    ADD_HOOK(CmdDrawIndexed)                        \
    ADD_HOOK(BeginCommandBuffer)                    \
    ADD_HOOK(EndCommandBuffer)                      \
    ADD_HOOK(QueuePresentKHR)

template<typename DispatchableType>
void *get_key(DispatchableType inst) {
    return *(void **)inst;
}

std::mutex global_lock;

std::map<VkPhysicalDevice, VkInstance> phys_device_to_instance_map;

struct LayerData {
    std::unique_ptr<VkuInstanceDispatchTable> instance_dispatch;
    std::unique_ptr<VkuDeviceDispatchTable> device_dispatch;

    PFN_vkGetInstanceProcAddr sub_instance_gipa = nullptr;

    HWND hwnd = nullptr;

    VkInstance app_instance                = VK_NULL_HANDLE;
    VkInstance instance                    = VK_NULL_HANDLE;
    VkSurfaceKHR surface                   = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device       = VK_NULL_HANDLE;
    VkDevice device                        = VK_NULL_HANDLE;
    VkQueue present_queue                  = VK_NULL_HANDLE;
    VkQueue gfx_queue                      = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain               = VK_NULL_HANDLE;
    VkCommandPool cmd_pool                 = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buffer             = VK_NULL_HANDLE;
    VkDescriptorPool imgui_descriptor_pool = VK_NULL_HANDLE;
    VkSemaphore image_available_semaphore  = VK_NULL_HANDLE;
    VkSemaphore render_finished_semaphore  = VK_NULL_HANDLE;
    VkFence fence                          = VK_NULL_HANDLE;

    VkFormat swapchain_image_format;
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_image_views;
};

class LayerDataMap {
    std::map<void *, std::shared_ptr<LayerData>> layer_data;

public:
    void emplace(void *key) { layer_data.emplace(key, std::make_shared<LayerData>()); }

    void add_device_alias(void *inst_key, void *device_key) { layer_data.emplace(device_key, layer_data.at(inst_key)); }

    LayerData& at(void *key) {
        if (!layer_data.contains(key)) {
            PRINT_ERROR("Tried to access layer data with an unknown key");
            std::cout << std::stacktrace::current() << "\n";
        }

        return *layer_data.at(key);
    }

    LayerData& at_pd(const VkPhysicalDevice physical_device) {
        return at(get_key(phys_device_to_instance_map.at(physical_device)));
    }
};

LayerDataMap layer_data;

bool is_creating_instance = false;
bool is_creating_device   = false;
bool is_rendering_window  = false;

static constexpr uint32_t WINDOW_WIDTH = 1200;
static constexpr uint32_t WINDOW_HEIGHT = 900;

///////////////////////////////////////////////////////////////////////////////
// Window
///////////////////////////////////////////////////////////////////////////////

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

HWND create_window() {
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = TEXT("SnapshotWindow");

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        TEXT("Snapshot Layer"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        nullptr,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (!hwnd) {
        PRINT_ERROR(GetLastError());
        return nullptr;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    return hwnd;
}

///////////////////////////////////////////////////////////////////////////////
// ImGui
///////////////////////////////////////////////////////////////////////////////

void init_imgui(LayerData& ld) {
    const std::vector<VkDescriptorPoolSize> pool_sizes {
        {VK_DESCRIPTOR_TYPE_SAMPLER,                1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,   1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,       1000}
    };

    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1000u,
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };

    vkCreateDescriptorPool(ld.device, &pool_info, nullptr, &ld.imgui_descriptor_pool);

    ImGui_ImplVulkan_InitInfo imgui_init_info = {
        .ApiVersion = VK_API_VERSION_1_3,
        .Instance = ld.instance,
        .PhysicalDevice = ld.physical_device,
        .Device = ld.device,
        .Queue = ld.gfx_queue,
        .DescriptorPool = ld.imgui_descriptor_pool,
        .MinImageCount = static_cast<uint32_t>(ld.swapchain_images.size()),
        .ImageCount = static_cast<uint32_t>(ld.swapchain_images.size()),
        .PipelineInfoMain = {
            .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
            .PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo {
                .colorAttachmentCount = 1u,
                .pColorAttachmentFormats = &ld.swapchain_image_format,
            },
        },
        .UseDynamicRendering = true,
    };

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // io.DisplaySize = ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT);
    ImGui::StyleColorsDark();
    ImGui_ImplVulkan_Init(&imgui_init_info);
    ImGui_ImplWin32_Init(ld.hwnd);
}

///////////////////////////////////////////////////////////////////////////////
// Utils
///////////////////////////////////////////////////////////////////////////////

VkPhysicalDevice find_matching_physical_device(const LayerData& ld, VkPhysicalDevice app_physical_device) {
    VkPhysicalDeviceIDProperties app_physical_device_id {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
    };
    VkPhysicalDeviceProperties2 app_physical_device_properties2 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &app_physical_device_id,
    };

    const auto fn_ptr = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        ld.instance_dispatch->GetInstanceProcAddr(ld.app_instance, "vkGetPhysicalDeviceProperties2"));
    fn_ptr(app_physical_device, &app_physical_device_properties2);

#define GET_INSTANCE ld.instance

    uint32_t device_count = 0;
    INST_PROC_ADDR(vkEnumeratePhysicalDevices)(ld.instance, &device_count, nullptr);
    std::vector<VkPhysicalDevice> physical_devices(device_count);
    INST_PROC_ADDR(vkEnumeratePhysicalDevices)(ld.instance, &device_count, physical_devices.data());

    for (const auto& physical_device : physical_devices) {
        VkPhysicalDeviceIDProperties physical_device_id {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 physical_device_properties2 {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &physical_device_id,
        };
        INST_PROC_ADDR(vkGetPhysicalDeviceProperties2)(physical_device, &physical_device_properties2);

        if (memcmp(physical_device_id.deviceUUID, app_physical_device_id.deviceUUID, VK_UUID_SIZE) == 0) {
            return physical_device;
        }
    }

#undef GET_INSTANCE

    return VK_NULL_HANDLE;
}

///////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown
///////////////////////////////////////////////////////////////////////////////

VK_LAYER_EXPORT VkResult VKAPI_CALL
SnapshotLayer_CreateInstance(
    const VkInstanceCreateInfo*     p_create_info,
    const VkAllocationCallbacks*    p_allocator,
    VkInstance*                     p_instance
) {
    PRINT_DEBUG_FN_ENTER(is_creating_instance);
    auto *layer_create_info = (VkLayerInstanceCreateInfo *) p_create_info->pNext;

    while (
        layer_create_info
        && (layer_create_info->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layer_create_info->function != VK_LAYER_LINK_INFO)
    ) {
        layer_create_info = (VkLayerInstanceCreateInfo *) layer_create_info->pNext;
    }

    if (layer_create_info == nullptr) {
        PRINT_ERROR("Failed to find layer description, returning VK_ERROR_INITIALIZATION_FAILED");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layer_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layer_create_info->u.pLayerInfo = layer_create_info->u.pLayerInfo->pNext;

    const auto create_func = (PFN_vkCreateInstance) gipa(VK_NULL_HANDLE, "vkCreateInstance");
    const VkResult result = create_func(p_create_info, p_allocator, p_instance);
    if (result != VK_SUCCESS) return result;

    VkuInstanceDispatchTable dispatch_table;
#define ADD_HOOK(func) dispatch_table.func = (PFN_vk##func) gipa(*p_instance, "vk" #func);
    LIST_OF_INSTANCE_HOOKS
#undef ADD_HOOK

    LayerData temp_ld {};

    if (!is_creating_instance) {
        temp_ld.hwnd = create_window();

        const std::vector<const char*> extension_names { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };

        const VkApplicationInfo app_info {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = VK_API_VERSION_1_3,
        };

        const VkInstanceCreateInfo instance_create_info {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info,
            .enabledExtensionCount = static_cast<uint32_t>(extension_names.size()),
            .ppEnabledExtensionNames = extension_names.data(),
        };

        is_creating_instance = true;
        CHECK_RESULT(
            vkCreateInstance(&instance_create_info, nullptr, &temp_ld.instance),
            "Failed to create instance"
        );
        is_creating_instance = false;

        const VkWin32SurfaceCreateInfoKHR surface_create_info {
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = reinterpret_cast<HINSTANCE>(GetWindowLong(temp_ld.hwnd, GWLP_HINSTANCE)),
            .hwnd = temp_ld.hwnd,
        };

        CHECK_RESULT(
            vkCreateWin32SurfaceKHR(temp_ld.instance, &surface_create_info, nullptr, &temp_ld.surface),
            "Failed to create surface",
            temp_ld.instance
        );
    }

    {
        std::lock_guard l(global_lock);
        layer_data.emplace(get_key(*p_instance));
        LayerData& ld = layer_data.at(get_key(*p_instance));
        ld.instance_dispatch = std::make_unique<VkuInstanceDispatchTable>(std::move(dispatch_table));
        ld.sub_instance_gipa = gipa;
        ld.hwnd = temp_ld.hwnd;
        ld.app_instance = *p_instance;
        ld.instance = temp_ld.instance;
        ld.surface = temp_ld.surface;
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
SnapshotLayer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* p_allocator) {
    PRINT_DEBUG_FN_ENTER(instance);
    std::lock_guard l(global_lock);
    const LayerData& ld = layer_data.at(get_key(instance));

    vkDestroySurfaceKHR(instance, ld.surface, nullptr);
    DestroyWindow(ld.hwnd);

    // layer_data.erase(get_key(instance));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
SnapshotLayer_CreateDevice(
    VkPhysicalDevice                physical_device,
    const VkDeviceCreateInfo*       p_create_info,
    const VkAllocationCallbacks*    p_allocator,
    VkDevice*                       p_device
) {
    PRINT_DEBUG_FN_ENTER();
    auto* layer_create_info = (VkLayerDeviceCreateInfo *) p_create_info->pNext;

    while (
        layer_create_info
        && (layer_create_info->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layer_create_info->function != VK_LAYER_LINK_INFO)
    ) {
        layer_create_info = (VkLayerDeviceCreateInfo *) layer_create_info->pNext;
    }

    if (layer_create_info == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = layer_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = layer_create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    layer_create_info->u.pLayerInfo = layer_create_info->u.pLayerInfo->pNext;

    const auto create_func = (PFN_vkCreateDevice) gipa(VK_NULL_HANDLE, "vkCreateDevice");
    create_func(physical_device, p_create_info, p_allocator, p_device);

    VkuDeviceDispatchTable dispatch_table;
#define ADD_HOOK(func) dispatch_table.func = (PFN_vk##func) gdpa(*p_device, "vk" #func);
    LIST_OF_DEVICE_HOOKS
#undef ADD_HOOK

    LayerData temp_ld {};

     {
        std::lock_guard l(global_lock);
        layer_data.add_device_alias(get_key(phys_device_to_instance_map.at(physical_device)), get_key(*p_device));
        LayerData& ld = layer_data.at(get_key(*p_device));

        ld.device_dispatch = std::make_unique<VkuDeviceDispatchTable>(std::move(dispatch_table));

        if (!is_creating_device) {
            ld.physical_device = find_matching_physical_device(ld, physical_device);

            temp_ld.hwnd            = ld.hwnd;
            temp_ld.instance        = ld.instance;
            temp_ld.app_instance    = ld.app_instance;
            temp_ld.surface         = ld.surface;
            temp_ld.physical_device = ld.physical_device;
        }
    }

#define GET_INSTANCE temp_ld.instance
#define GET_DEVICE temp_ld.device

    if (!is_creating_device && temp_ld.device == VK_NULL_HANDLE) {
        std::cout << "\thello 1\n";

        // device and queues

        uint32_t queue_family_count;
        INST_PROC_ADDR(vkGetPhysicalDeviceQueueFamilyProperties)(temp_ld.physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
        INST_PROC_ADDR(vkGetPhysicalDeviceQueueFamilyProperties)(temp_ld.physical_device, &queue_family_count, queue_family_properties.data());

        std::cout << "\thello 2\n";

        uint32_t graphics_queue_family_idx;
        {
            const auto suitable_queue_iter = std::ranges::find_if(queue_family_properties, [](const auto& x) {
                return x.queueFlags & VK_QUEUE_GRAPHICS_BIT;
            });
            if (suitable_queue_iter != queue_family_properties.end()) {
                graphics_queue_family_idx = std::distance(queue_family_properties.begin(), suitable_queue_iter);
            } else {
                PRINT_ERROR("Couldn't find a graphics queue");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        std::cout << "\thello 3\n";

        uint32_t present_queue_family_idx;
        {
            const auto indices = std::views::iota(0u, queue_family_count);
            const auto suitable_queue_iter = std::ranges::find_if(indices, [&](const uint32_t& i) {
                VkBool32 present_support = false;
                INST_PROC_ADDR(vkGetPhysicalDeviceSurfaceSupportKHR)(temp_ld.physical_device, i, temp_ld.surface, &present_support);
                return present_support;
            });
            if (suitable_queue_iter != indices.end()) {
                present_queue_family_idx = *suitable_queue_iter;
            } else {
                PRINT_ERROR("Couldn't find a present queue");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        std::cout << "\thello 4\n";

        constexpr float queue_priority = 1.0f;

        const std::array queue_create_infos {
            VkDeviceQueueCreateInfo {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = present_queue_family_idx,
                .queueCount = 1u,
                .pQueuePriorities = &queue_priority,
            },
            VkDeviceQueueCreateInfo {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = graphics_queue_family_idx,
                .queueCount = 1u,
                .pQueuePriorities = &queue_priority,
            },
        };

        const std::array device_extensions {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        };

        const VkDeviceCreateInfo device_create_info {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
            .pQueueCreateInfos = queue_create_infos.data(),
            .enabledExtensionCount = static_cast<uint32_t>(device_extensions.size()),
            .ppEnabledExtensionNames = device_extensions.data(),
        };

        std::cout << "\thello 5\n";

        is_creating_device = true;
        CHECK_RESULT(
            INST_PROC_ADDR(vkCreateDevice)(temp_ld.physical_device, &device_create_info, nullptr, &temp_ld.device),
            "Failed to create device"
        )
        is_creating_device = false;

        std::cout << "\thello 6\n";

        DEVICE_PROC_ADDR(vkGetDeviceQueue)(temp_ld.device, present_queue_family_idx, 0, &temp_ld.present_queue);
        DEVICE_PROC_ADDR(vkGetDeviceQueue)(temp_ld.device, graphics_queue_family_idx, 0, &temp_ld.gfx_queue);

        std::cout << "\thello 7\n";

        // swapchain

        const bool are_queues_uniform = present_queue_family_idx == graphics_queue_family_idx;
        std::array queue_family_indices { graphics_queue_family_idx, present_queue_family_idx };

        VkSurfaceCapabilitiesKHR surface_capabilities;
        INST_PROC_ADDR(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(temp_ld.physical_device, temp_ld.surface, &surface_capabilities);

        std::cout << "\thello 8\n";

        uint32_t format_count;
        INST_PROC_ADDR(vkGetPhysicalDeviceSurfaceFormatsKHR)(temp_ld.physical_device, temp_ld.surface, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        INST_PROC_ADDR(vkGetPhysicalDeviceSurfaceFormatsKHR)(temp_ld.physical_device, temp_ld.surface, &format_count, formats.data());
        temp_ld.swapchain_image_format = formats[0].format;

        std::cout << "\thello 8.1\n";

        const VkSwapchainCreateInfoKHR swapchain_create_info {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = temp_ld.surface,
            .minImageCount = 2u,
            .imageFormat = temp_ld.swapchain_image_format,
            .imageColorSpace = formats[0].colorSpace,
            .imageExtent = VkExtent2D { WINDOW_WIDTH, WINDOW_HEIGHT },
            .imageArrayLayers = 1u,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = are_queues_uniform ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
            .queueFamilyIndexCount = are_queues_uniform ? 0u : 2u,
            .pQueueFamilyIndices = are_queues_uniform ? nullptr : queue_family_indices.data(),
            .preTransform = surface_capabilities.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
            .oldSwapchain = VK_NULL_HANDLE,
        };

        CHECK_RESULT(
            DEVICE_PROC_ADDR(vkCreateSwapchainKHR)(temp_ld.device, &swapchain_create_info, nullptr, &temp_ld.swapchain),
            "Failed to create swapchain"
        );

        std::cout << "\thello 8.2\n";

        // swapchain images

        uint32_t image_count;
        DEVICE_PROC_ADDR(vkGetSwapchainImagesKHR)(temp_ld.device, temp_ld.swapchain, &image_count, nullptr);
        temp_ld.swapchain_images.resize(image_count);
        DEVICE_PROC_ADDR(vkGetSwapchainImagesKHR)(temp_ld.device, temp_ld.swapchain, &image_count, temp_ld.swapchain_images.data());

        std::cout << "\thello 9\n";

        // swapchain image views

        temp_ld.swapchain_image_views.resize(image_count);
        for (uint32_t i = 0; const auto& image : temp_ld.swapchain_images) {
            const VkImageViewCreateInfo image_view_create_info {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = temp_ld.swapchain_image_format,
                .subresourceRange = VkImageSubresourceRange {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0u,
                    .levelCount = 1u,
                    .baseArrayLayer = 0u,
                    .layerCount = 1u,
                }
            };

            CHECK_RESULT(
                DEVICE_PROC_ADDR(vkCreateImageView)(temp_ld.device, &image_view_create_info, nullptr, &temp_ld.swapchain_image_views[i++]),
                "Failed to create image view"
            );
        }

        std::cout << "\thello 10\n";

        // command pool

        const VkCommandPoolCreateInfo pool_create_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = graphics_queue_family_idx,
        };

        CHECK_RESULT(
            DEVICE_PROC_ADDR(vkCreateCommandPool)(temp_ld.device, &pool_create_info, nullptr, &temp_ld.cmd_pool),
            "Failed to create command pool"
        );

        std::cout << "\thello 11\n";

        // command buffer

        const VkCommandBufferAllocateInfo cmd_buf_alloc_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = temp_ld.cmd_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1u,
        };

        CHECK_RESULT(
            DEVICE_PROC_ADDR(vkAllocateCommandBuffers)(temp_ld.device, &cmd_buf_alloc_info, &temp_ld.cmd_buffer),
            "Failed to allocate command buffer"
        );

        std::cout << "\thello 12\n";

        // semaphores and fences

        const VkSemaphoreCreateInfo semaphore_create_info {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        const VkFenceCreateInfo fence_create_info {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };

        CHECK_RESULT(
            DEVICE_PROC_ADDR(vkCreateSemaphore)(temp_ld.device, &semaphore_create_info, nullptr, &temp_ld.image_available_semaphore),
            "Failed to create semaphore #1"
        );
        CHECK_RESULT(
            DEVICE_PROC_ADDR(vkCreateSemaphore)(temp_ld.device, &semaphore_create_info, nullptr, &temp_ld.render_finished_semaphore),
            "Failed to create semaphore #2"
        );
        CHECK_RESULT(
            DEVICE_PROC_ADDR(vkCreateFence)(temp_ld.device, &fence_create_info, nullptr, &temp_ld.fence),
            "Failed to create fence"
        );

        std::cout << "\thello 13\n";

        // imgui

        init_imgui(temp_ld);
    }

#undef GET_INSTANCE
#undef GET_DEVICE

    if (!is_creating_device) {
        std::lock_guard lock(global_lock);
        LayerData& ld = layer_data.at(get_key(*p_device));
        ld.device                       = temp_ld.device;
        ld.present_queue                = temp_ld.present_queue;
        ld.gfx_queue                    = temp_ld.gfx_queue;
        ld.swapchain                    = temp_ld.swapchain;
        ld.cmd_pool                     = temp_ld.cmd_pool;
        ld.cmd_buffer                   = temp_ld.cmd_buffer;
        ld.imgui_descriptor_pool        = temp_ld.imgui_descriptor_pool;
        ld.image_available_semaphore    = temp_ld.image_available_semaphore;
        ld.render_finished_semaphore    = temp_ld.render_finished_semaphore;
        ld.fence                        = temp_ld.fence;

        ld.swapchain_image_format       = temp_ld.swapchain_image_format;
        ld.swapchain_images             = temp_ld.swapchain_images;
        ld.swapchain_image_views        = temp_ld.swapchain_image_views;
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
SnapshotLayer_DestroyDevice(VkDevice device, const VkAllocationCallbacks* p_allocator) {
    PRINT_DEBUG_FN_ENTER();
    std::lock_guard l(global_lock);
    const LayerData& ld = layer_data.at(get_key(device));

    // layer_data.erase(get_key(device));
}

///////////////////////////////////////////////////////////////////////////////
// Queue funcs
///////////////////////////////////////////////////////////////////////////////

VK_LAYER_EXPORT VkResult VKAPI_CALL
SnapshotLayer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* p_present_info) {
    PRINT_DEBUG_FN_ENTER();

    if (is_rendering_window) {
        return layer_data.at(get_key(queue)).device_dispatch->QueuePresentKHR(queue, p_present_info);
    }

    std::lock_guard l(global_lock);
    const LayerData& ld = layer_data.at(get_key(queue));

#define GET_INSTANCE ld.instance
#define GET_DEVICE ld.device

    // record command buffer for our auxiliary window
    if (!is_rendering_window) {
        std::cout << "\tRENDERING IMGUI FOR WINDOW!\n";
        is_rendering_window = true;

        vkWaitForFences(ld.device, 1, &ld.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(ld.device, 1, &ld.fence);

        uint32_t image_index;
        vkAcquireNextImageKHR(ld.device, ld.swapchain, UINT64_MAX, ld.image_available_semaphore, VK_NULL_HANDLE, &image_index);

        vkResetCommandBuffer(ld.cmd_buffer, 0);

        std::cout << "\theyy 1\n";

        const VkCommandBufferBeginInfo begin_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };

        if (vkBeginCommandBuffer(ld.cmd_buffer, &begin_info) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

        std::cout << "\theyy 2\n";

        std::vector<VkRenderingAttachmentInfo> attachment_infos;
        attachment_infos.emplace_back(VkRenderingAttachmentInfo {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = ld.swapchain_image_views[image_index],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = VkClearValue {
                .color = VkClearColorValue { 0.5f, 0.5f, 0.5f, 1.0f }
            }
        });

        const VkRenderingInfo rendering_info {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = VkRect2D {
                .offset = VkOffset2D { 0, 0 },
                .extent = VkExtent2D { WINDOW_WIDTH, WINDOW_HEIGHT }
            },
            .layerCount = 1u,
            .colorAttachmentCount = static_cast<uint32_t>(attachment_infos.size()),
            .pColorAttachments = attachment_infos.data(),
        };

        INST_PROC_ADDR(vkCmdBeginRendering)(ld.cmd_buffer, &rendering_info);

        std::cout << "\theyy 3\n";

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        std::cout << "\theyy 4\n";

        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                                           | ImGuiWindowFlags_NoCollapse
                                           | ImGuiWindowFlags_NoSavedSettings
                                           | ImGuiWindowFlags_NoResize
                                           | ImGuiWindowFlags_NoMove;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT));

        if (ImGui::Begin("main window", nullptr, flags)) {
            ImGui::Text("This is some useful text.");
            std::cout << "\theyy 5\n";
            ImGui::End(); std::cout << "\theyy 5.1\n";
        }

        ImGui::Render(); std::cout << "\theyy 5.2\n";
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ld.cmd_buffer);

        std::cout << "\theyy 6\n";

        vkCmdEndRendering(ld.cmd_buffer);

        std::cout << "\theyy 7\n";

        vkEndCommandBuffer(ld.cmd_buffer);

        std::cout << "\theyy 8\n";

        const VkPipelineStageFlags wait_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkSubmitInfo submit_info {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1u,
            .pWaitSemaphores = &ld.image_available_semaphore,
            .pWaitDstStageMask = &wait_stages,
            .commandBufferCount = 1u,
            .pCommandBuffers = &ld.cmd_buffer,
            .pSignalSemaphores = &ld.render_finished_semaphore,
        };

        vkQueueSubmit(ld.gfx_queue, 1u, &submit_info, ld.fence);

        const VkPresentInfoKHR present_info {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1u,
            .pWaitSemaphores = &ld.render_finished_semaphore,
            .swapchainCount = 1u,
            .pSwapchains = &ld.swapchain,
            .pImageIndices = &image_index,
        };

        vkQueuePresentKHR(ld.present_queue, &present_info);

        is_rendering_window = false;
    }

#undef GET_DEVICE
#undef GET_INSTANCE

    return layer_data.at(get_key(queue)).device_dispatch->QueuePresentKHR(queue, p_present_info);
}

///////////////////////////////////////////////////////////////////////////////
// Command buffer funcs
///////////////////////////////////////////////////////////////////////////////

VK_LAYER_EXPORT VkResult VKAPI_CALL
SnapshotLayer_BeginCommandBuffer(VkCommandBuffer command_buffer, const VkCommandBufferBeginInfo* p_begin_info) {
    PRINT_DEBUG_FN_ENTER();

    if (is_rendering_window) {
        return layer_data.at(get_key(command_buffer)).device_dispatch->BeginCommandBuffer(command_buffer, p_begin_info);
    }

    std::lock_guard l(global_lock);
    return layer_data.at(get_key(command_buffer)).device_dispatch->BeginCommandBuffer(command_buffer, p_begin_info);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
SnapshotLayer_EndCommandBuffer(VkCommandBuffer command_buffer) {
    PRINT_DEBUG_FN_ENTER();

    if (is_rendering_window) {
        return layer_data.at(get_key(command_buffer)).device_dispatch->EndCommandBuffer(command_buffer);
    }

    std::lock_guard l(global_lock);
    return layer_data.at(get_key(command_buffer)).device_dispatch->EndCommandBuffer(command_buffer);
}

VK_LAYER_EXPORT void VKAPI_CALL 
SnapshotLayer_CmdDraw(
    VkCommandBuffer     command_buffer,
    uint32_t            vertex_count,
    uint32_t            instance_count,
    uint32_t            first_vertex,
    uint32_t            first_instance
) {
    PRINT_DEBUG_FN_ENTER();

    if (is_rendering_window) {
        return layer_data.at(get_key(command_buffer)).device_dispatch->
            CmdDraw(command_buffer, vertex_count, instance_count, first_vertex, first_instance);
    }

    std::lock_guard l(global_lock);
    return layer_data.at(get_key(command_buffer)).device_dispatch->
        CmdDraw(command_buffer, vertex_count, instance_count, first_vertex, first_instance);
}

VK_LAYER_EXPORT void VKAPI_CALL 
SnapshotLayer_CmdDrawIndexed(
    VkCommandBuffer     command_buffer,
    uint32_t            index_count,
    uint32_t            instance_count,
    uint32_t            first_index,
    int32_t             vertex_offset,
    uint32_t            first_instance
) {
    PRINT_DEBUG_FN_ENTER();

    if (is_rendering_window) {
        return layer_data.at(get_key(command_buffer)).device_dispatch->
            CmdDrawIndexed(command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    std::lock_guard l(global_lock);
    return layer_data.at(get_key(command_buffer)).device_dispatch->
        CmdDrawIndexed(command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
}

///////////////////////////////////////////////////////////////////////////////
// Enumerating functons
///////////////////////////////////////////////////////////////////////////////

VK_LAYER_EXPORT VkResult VKAPI_CALL 
SnapshotLayer_EnumerateInstanceLayerProperties(uint32_t *p_property_count, VkLayerProperties *p_properties)
{
    PRINT_DEBUG_FN_ENTER();
    if (p_property_count) *p_property_count = 1;

    if (p_properties) {
        std::strcpy(p_properties->layerName, "VK_LAYER_Snapshot");
        std::strcpy(p_properties->description, "Filip's prototype SnapshotLayer");
        p_properties->implementationVersion = 1;
        p_properties->specVersion = VK_API_VERSION_1_3;
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL 
SnapshotLayer_EnumerateDeviceLayerProperties(
    VkPhysicalDevice        physical_device,
    uint32_t*               p_property_count, 
    VkLayerProperties*      p_properties
) {
    PRINT_DEBUG_FN_ENTER();
    return SnapshotLayer_EnumerateInstanceLayerProperties(p_property_count, p_properties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL 
SnapshotLayer_EnumerateInstanceExtensionProperties(
    const char*             p_layer_name, 
    uint32_t*               p_property_count, 
    VkExtensionProperties*  p_properties
) {
    PRINT_DEBUG_FN_ENTER();
    if (p_layer_name == nullptr || strcmp(p_layer_name, "VK_LAYER_Snapshot")) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    if (p_property_count) {
        *p_property_count = 0;
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
SnapshotLayer_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice        physical_device,
    const char*             p_layer_name,
    uint32_t*               p_property_count,
    VkExtensionProperties*  p_properties
) {
    PRINT_DEBUG_FN_ENTER();
    if (p_layer_name == nullptr || strcmp(p_layer_name, "VK_LAYER_Snapshot")) {
        if (physical_device == VK_NULL_HANDLE) {
            return VK_SUCCESS;
        }

        std::lock_guard l(global_lock);
        return layer_data.at_pd(physical_device).instance_dispatch->EnumerateDeviceExtensionProperties(physical_device, p_layer_name, p_property_count, p_properties);
    }

    if (p_property_count) {
        *p_property_count = 0;
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
SnapshotLayer_EnumeratePhysicalDevices(
    VkInstance              instance,
    uint32_t*               p_physical_device_count,
    VkPhysicalDevice*       p_physical_devices
) {
    PRINT_DEBUG_FN_ENTER(instance);

    const VkResult result = layer_data.at(get_key(instance)).instance_dispatch->EnumeratePhysicalDevices(
        instance, p_physical_device_count, p_physical_devices);

    if (p_physical_device_count && p_physical_devices) {
        for (uint32_t i = 0; i < *p_physical_device_count; i++) {
            phys_device_to_instance_map.emplace(p_physical_devices[i], instance);
        }
    }

    return result;
}

///////////////////////////////////////////////////////////////////////////////
// GetProcAddr functions (entry-points)
///////////////////////////////////////////////////////////////////////////////

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
SnapshotLayer_GetInstanceProcAddr(VkInstance instance, const char *p_name) {
    PRINT_DEBUG_FN_ENTER(instance, p_name);

#ifndef NDEBUG
#define ADD_HOOK(func)                                      \
    if (!strcmp(p_name, "vk" #func)) {                      \
        std::cout << "\tentering " #func << "\n";           \
        return (PFN_vkVoidFunction)&SnapshotLayer_##func;   \
    }
#else
#define ADD_HOOK(func) if (!strcmp(p_name, "vk" #func)) return (PFN_vkVoidFunction)&SnapshotLayer_##func;
#endif
    LIST_OF_INSTANCE_HOOKS
#undef ADD_HOOK

    if (instance == nullptr) return nullptr;

    {
        std::lock_guard l(global_lock);
        return layer_data.at(get_key(instance)).instance_dispatch->GetInstanceProcAddr(instance, p_name);
    }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL 
SnapshotLayer_GetDeviceProcAddr(VkDevice device, const char *p_name) {
    PRINT_DEBUG_FN_ENTER(device, p_name);

#ifndef NDEBUG
#define ADD_HOOK(func)                                      \
    if (!strcmp(p_name, "vk" #func)) {                      \
        std::cout << "\tentering " #func << "\n";           \
        return (PFN_vkVoidFunction)&SnapshotLayer_##func;   \
    }
#else
#define ADD_HOOK(func) if (!strcmp(p_name, "vk" #func)) return (PFN_vkVoidFunction)&SnapshotLayer_##func;
#endif
    LIST_OF_DEVICE_HOOKS
#undef ADD_HOOK

    if (device == nullptr) return nullptr;

    {
        std::lock_guard l(global_lock);
        return layer_data.at(get_key(device)).device_dispatch->GetDeviceProcAddr(device, p_name);
    }
}
