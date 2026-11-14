// Vulkan backend for the graphics engine.
//
// The game's render targets live in images of their own ("virtual framebuffers"), keyed by the
// guest address the game renders to, at a multiple of the original resolution. Drawing is
// recorded into one command buffer per flush; presenting scales the displayed target into the
// window.

#include "runtime/gfx/vk/vk_renderer.h"

#include "lib/common/file.h"
#include "lib/common/hash.h"
#include "lib/common/log.h"
#include "runtime/ge/ge.h"
#include "runtime/platform/frame.h"
#include "runtime/platform/host.h"
#include "runtime/mem/memory.h"

#define VK_NO_PROTOTYPES
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <vk_mem_alloc.h>
#pragma clang diagnostic pop

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ge_frag.h"
#include "ge_vert.h"

namespace p2::gfx {

namespace {

using ge::DrawState;
using ge::TextureDesc;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr u32 kDisplayWidth = 480, kDisplayHeight = 272;
constexpr VkDeviceSize kVertexBufferSize = 48u << 20;
constexpr VkDeviceSize kIndexBufferSize = 12u << 20;
constexpr VkDeviceSize kStagingSize = 32u << 20;
constexpr u64 kTextureLifetimeFrames = 900;

// Mirrors the push-constant block in the shaders.
struct PushConstants {
    f32 pixel_to_ndc[2];
    f32 uv_scale[2];
    f32 tex_env_color[4];
    f32 fog_color[4];
    u32 flags;
    u32 alpha_test;
    u32 color_ref;
    u32 color_mask;
    f32 stencil_value;
};

enum : u32 {
    kFlagTexture = 1u << 0,
    kFlagTexAlpha = 1u << 4,
    kFlagColorDouble = 1u << 5,
    kFlagFog = 1u << 6,
    kFlagAlphaTest = 1u << 7,
    kFlagColorTest = 1u << 8,
    kFlagFlat = 1u << 11,
    kFlagClear = 1u << 12,
    kFlagWriteStencil = 1u << 13,
    kFlagDoubleAlpha = 1u << 14,
    kFlagProject = 1u << 15,
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    u32 width = 0, height = 0;
    bool depth = false;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    u8* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize used = 0;
};

// A guest render target.
struct Framebuffer {
    u32 addr = 0;
    u32 stride = 0;
    u32 format = 0;
    u32 width = 0, height = 0; // guest pixels
    Image color;
    Image depth;
    u64 last_used = 0;
    bool drawn = false;

    u32 byte_size() const { return stride * height * (format == 3 ? 4 : 2); }
};

struct Texture {
    Image image;
    u64 content_hash = 0;
    u64 checked_frame = ~0ull;
    u64 checked_epoch = ~0ull; // the memory epoch (see TextureDesc) at the last comparison
    u64 used_frame = 0;
    u32 bytes = 0;
};

struct TextureKey {
    u32 addr, format, width, height, buf_width, clut_bits;
    u64 clut_hash;
    bool operator==(const TextureKey& o) const {
        return addr == o.addr && format == o.format && width == o.width && height == o.height &&
               buf_width == o.buf_width && clut_bits == o.clut_bits && clut_hash == o.clut_hash;
    }
};

struct TextureKeyHash {
    size_t operator()(const TextureKey& k) const {
        u64 h = k.addr * 0x9E3779B97F4A7C15ull;
        h ^= (u64(k.format) << 40) ^ (u64(k.width) << 20) ^ k.height ^ (u64(k.buf_width) << 48) ^ (u64(k.clut_bits) << 8);
        h = (h ^ k.clut_hash) * 0xC2B2AE3D27D4EB4Full;
        return size_t(h ^ (h >> 29));
    }
};

struct PipelineKey {
    u8 topology = 0;
    u8 blend = 0, blend_src = 0, blend_dst = 0, blend_eq = 0;
    u8 color_mask = 0;
    u8 depth_test = 0, depth_write = 0, depth_func = 0;
    u8 pad[7] = {};
    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const {
        u64 a, b;
        std::memcpy(&a, &k, 8);
        std::memcpy(&b, reinterpret_cast<const u8*>(&k) + 8, 8);
        return size_t((a * 0x9E3779B97F4A7C15ull) ^ (b * 0xC2B2AE3D27D4EB4Full));
    }
};
static_assert(sizeof(PipelineKey) == 16);

// Fast non-cryptographic hash for change detection of texture data.
void unpack_color(u32 c, f32 out[4]) {
    out[0] = f32(c & 0xFF) / 255.0f;
    out[1] = f32((c >> 8) & 0xFF) / 255.0f;
    out[2] = f32((c >> 16) & 0xFF) / 255.0f;
    out[3] = f32(c >> 24) / 255.0f;
}

class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override { shutdown(); }

    bool init(const RendererOptions& options, std::string* error);

    void set_scale(u32 scale) override;
    u32 scale() const override { return scale_; }
    void set_vsync(bool on) override;
    void set_fit(Fit fit, bool smooth) override {
        fit_ = fit;
        smooth_ = smooth;
    }
    void set_output_visible(bool visible) override { output_visible_ = visible; }
    void output_size(u32& width, u32& height) const override;
    void set_overlay_builder(std::function<void()> build) override { overlay_builder_ = std::move(build); }
    bool capture_output(std::vector<u8>& rgba, u32& width, u32& height) override;
    u64 create_ui_texture(const u32* rgba, u32 width, u32 height) override;
    void destroy_ui_texture(u64 handle) override;

    void draw(const DrawState& state, ge::Topology topology, const ge::Vertex* vertices, u32 vertex_count,
              const u16* indices, u32 index_count) override;
    void block_transfer(u32 src_addr, u32 src_stride, u32 src_x, u32 src_y, u32 dst_addr, u32 dst_stride, u32 dst_x,
                        u32 dst_y, u32 width, u32 height, u32 bytes_per_pixel) override;
    void invalidate_textures() override {
        for (auto& [key, tex] : textures_) tex.checked_frame = ~0ull;
    }
    void present(const ge::DisplayTarget& target) override;
    bool capture(std::vector<u8>& rgba, u32& width, u32& height) override;

private:
    bool fail(std::string* error, const char* what) {
        if (error) *error = what;
        return false;
    }
    // Warns about a feature the game uses that the renderer only approximates, once per kind.
    template <class... Args>
    void report_once(u32 kind, const char* fmt, Args... args) {
        if (!reported_.insert(kind).second) return;
        std::string text = format(fmt, args...);
        LOG_WARN("gfx", "%s (frame %llu)", text.c_str(), static_cast<unsigned long long>(frame_));
    }
    std::unordered_set<u32> reported_;
    void shutdown();

    bool create_device(std::string* error);
    bool create_swapchain();
    void destroy_swapchain();
    bool create_image(Image& img, u32 w, u32 h, VkFormat format, VkImageUsageFlags usage, bool depth);
    void destroy_image(Image& img);
    bool create_buffer(Buffer& buf, VkDeviceSize size, VkBufferUsageFlags usage);
    void transition(Image& img, VkImageLayout layout);
    void transition_raw(VkImage image, VkImageLayout from, VkImageLayout to);

    void begin_commands();
    void end_pass();
    void flush();
    void begin_pass(Framebuffer* fb);

    Framebuffer* get_framebuffer(u32 addr, u32 stride, u32 format);
    Framebuffer* find_framebuffer(u32 addr);
    Texture* get_texture(const TextureDesc& desc, const u8* clut);
    VkPipeline get_pipeline(const PipelineKey& key);
    VkDescriptorSet get_descriptor(VkImageView view, u32 sampler);
    void upload_rgba(Image& dst, const u32* pixels, u32 w, u32 h);
    Image* display_image(const ge::DisplayTarget& target, u32* src_w, u32* src_h);
    void blit(Image& src, VkOffset3D s0, VkOffset3D s1, Image& dst, VkOffset3D d0, VkOffset3D d1, VkFilter filter);
    void compose(VkImage image, VkImageView view, VkExtent2D extent, VkImageLayout final_layout, Image* source, u32 src_w,
                 u32 src_h);
    bool init_overlay(VkFormat format);
    void load_pipeline_cache();
    void save_pipeline_cache();

    RendererOptions options_;
    SDL_Window* window_ = nullptr;
    u32 scale_ = 1;
    Fit fit_ = Fit::Aspect;
    bool smooth_ = true;
    bool output_visible_ = true;

    std::function<void()> overlay_builder_;
    bool overlay_ready_ = false;
    Image offscreen_; // the final picture when there is no window
    struct UiTexture {
        Image image;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::unordered_map<u64, UiTexture> ui_textures_;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    size_t pipelines_at_load_ = 0;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VmaAllocator allocator_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE; // the current frame slot's command buffer

    // Frames in flight: while the GPU works through one frame the next is recorded into the other
    // slot. Anything a submitted frame may still use is released only once that slot comes round
    // again, which is after its frame has finished.
    static constexpr u32 kFramesInFlight = 2;
    struct FrameSlot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE; // signalled when the swapchain image is ready
        bool pending = false;                  // submitted and not yet waited for
        Buffer vertex, index, staging;         // streaming data for this slot's commands
        std::vector<Image> dead_images;
        std::vector<VkDescriptorSet> dead_sets;
        std::vector<UiTexture> dead_ui;
    };
    FrameSlot slots_[kFramesInFlight];
    u32 slot_index_ = 0;
    FrameSlot& slot() { return slots_[slot_index_]; }
    void submit(VkSubmitInfo& si);
    void retire(FrameSlot& s);
    void advance_slot();

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    VkFormat swap_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent_{};
    std::vector<VkSemaphore> render_sems_; // one per swapchain image: its picture is complete

    VkShaderModule vert_ = VK_NULL_HANDLE, frag_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkSampler samplers_[16] = {};

    Buffer readback_;
    Image white_, raw_display_, transfer_tmp_, capture_;

    std::unordered_map<u32, Framebuffer> framebuffers_;
    std::unordered_map<TextureKey, Texture, TextureKeyHash> textures_;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
    std::unordered_map<u64, VkDescriptorSet> descriptors_;

    Framebuffer* pass_fb_ = nullptr;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet bound_set_ = VK_NULL_HANDLE;
    u64 frame_ = 0;
    std::vector<u32> decode_scratch_;

    Image* last_display_ = nullptr;
    u32 last_display_w_ = 0, last_display_h_ = 0;
};

// --- device -----------------------------------------------------------------------------------------------------

bool VulkanRenderer::create_device(std::string* error) {
    if (volkInitialize() != VK_SUCCESS) return fail(error, "no Vulkan loader was found; install a current graphics driver");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "p2ep";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> instance_ext;
    if (window_) {
        Uint32 count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        for (Uint32 i = 0; i < count; ++i) instance_ext.push_back(names[i]);
    }
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = u32(instance_ext.size());
    ici.ppEnabledExtensionNames = instance_ext.data();
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS)
        return fail(error, "cannot create a Vulkan 1.3 instance; update the graphics driver");
    volkLoadInstance(instance_);

    if (window_ && !SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_))
        return fail(error, "cannot create a Vulkan surface for the window");

    u32 count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    int best_score = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(pd, &features);
        if (!features.dualSrcBlend || !features.depthClamp) continue;

        u32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, queues.data());
        int family = -1;
        for (u32 q = 0; q < qcount; ++q) {
            if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 can_present = VK_TRUE;
            if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &can_present);
            if (can_present) {
                family = int(q);
                break;
            }
        }
        if (family < 0) continue;
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            physical_ = pd;
            queue_family_ = u32(family);
        }
    }
    if (!physical_) return fail(error, "no graphics device supports Vulkan 1.3 with the required features");

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    LOG_INFO("gfx", "Vulkan device: %s", props.deviceName);

    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.dualSrcBlend = VK_TRUE;
    f2.features.depthClamp = VK_TRUE;
    std::vector<const char*> device_ext;
    if (surface_) device_ext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = u32(device_ext.size());
    dci.ppEnabledExtensionNames = device_ext.data();
    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) return fail(error, "cannot create the Vulkan device");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VmaVulkanFunctions vma_fns{};
    vma_fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &vma_fns;
    if (vmaCreateAllocator(&aci, &allocator_) != VK_SUCCESS) return fail(error, "cannot create the GPU memory allocator");

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &pci, nullptr, &pool_);
    for (FrameSlot& s : slots_) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &cai, &s.cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(device_, &fci, nullptr, &s.fence);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &sci, nullptr, &s.acquired);
    }
    cmd_ = slot().cmd;
    return true;
}

bool VulkanRenderer::create_swapchain() {
    if (!surface_) return true;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS) return false;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) extent = {u32(w), u32(h)};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) return false; // minimised

    u32 count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }

    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!options_.vsync) {
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    }

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount) sci.minImageCount = caps.maxImageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &fresh) != VK_SUCCESS) return false;
    for (VkImageView view : swap_views_) vkDestroyImageView(device_, view, nullptr);
    swap_views_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = fresh;
    swap_extent_ = extent;
    swap_format_ = chosen.format;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swap_images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swap_images_.data());
    for (VkImage image : swap_images_) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = chosen.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device_, &vci, nullptr, &view);
        swap_views_.push_back(view);
    }
    // One "picture complete" semaphore per image: presentation of an image has finished with its
    // semaphore by the time the image can be acquired again.
    while (render_sems_.size() < swap_images_.size()) {
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        vkCreateSemaphore(device_, &semci, nullptr, &sem);
        render_sems_.push_back(sem);
    }
    return true;
}

void VulkanRenderer::destroy_swapchain() {
    for (VkSemaphore sem : render_sems_) vkDestroySemaphore(device_, sem, nullptr);
    render_sems_.clear();
    for (VkImageView view : swap_views_) vkDestroyImageView(device_, view, nullptr);
    swap_views_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    swap_images_.clear();
}

bool VulkanRenderer::create_image(Image& img, u32 w, u32 h, VkFormat format, VkImageUsageFlags usage, bool depth) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator_, &ici, &aci, &img.image, &img.alloc, nullptr) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &img.view) != VK_SUCCESS) return false;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.width = w;
    img.height = h;
    img.depth = depth;
    return true;
}

void VulkanRenderer::destroy_image(Image& img) {
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vmaDestroyImage(allocator_, img.image, img.alloc);
    img = Image{};
}

bool VulkanRenderer::create_buffer(Buffer& buf, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator_, &bci, &aci, &buf.buffer, &buf.alloc, &info) != VK_SUCCESS) return false;
    buf.mapped = static_cast<u8*>(info.pMappedData);
    buf.size = size;
    buf.used = 0;
    return true;
}

void VulkanRenderer::transition_raw(VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

void VulkanRenderer::transition(Image& img, VkImageLayout layout) {
    if (img.layout == layout) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = img.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VkImageAspectFlags(img.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    img.layout = layout;
}

bool VulkanRenderer::init(const RendererOptions& options, std::string* error) {
    options_ = options;
    window_ = static_cast<SDL_Window*>(options.sdl_window);
    scale_ = std::clamp<u32>(options.scale, 1, 10);
    if (!create_device(error)) return false;
    if (surface_ && !create_swapchain()) return fail(error, "cannot create the swapchain");

    // Shaders, layouts, samplers.
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(ge_vert_spv);
    smi.pCode = ge_vert_spv;
    vkCreateShaderModule(device_, &smi, nullptr, &vert_);
    smi.codeSize = sizeof(ge_frag_spv);
    smi.pCode = ge_frag_spv;
    vkCreateShaderModule(device_, &smi, nullptr, &frag_);

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &binding;
    vkCreateDescriptorSetLayout(device_, &dli, nullptr, &set_layout_);
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_);

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 8192;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_);

    for (u32 i = 0; i < 16; ++i) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.minFilter = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.magFilter = (i & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.addressModeU = (i & 4) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = (i & 8) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device_, &si, nullptr, &samplers_[i]);
    }

    const VkImageUsageFlags transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = true;
    for (FrameSlot& s : slots_)
        ok = ok && create_buffer(s.vertex, kVertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) &&
             create_buffer(s.index, kIndexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) &&
             create_buffer(s.staging, kStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    ok = ok && create_buffer(readback_, 1024 * 1024 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT) &&
              create_image(white_, 1, 1, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | transfer, false) &&
              create_image(raw_display_, 512, kDisplayHeight, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | transfer, false) &&
              create_image(transfer_tmp_, 1024, 1024, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | transfer, false) &&
              create_image(capture_, kDisplayWidth, kDisplayHeight, kColorFormat, transfer, false);
    if (!ok) return fail(error, "cannot allocate GPU resources");

    if (!surface_ && options.offscreen_width && options.offscreen_height) {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | transfer;
        if (!create_image(offscreen_, options.offscreen_width, options.offscreen_height, kColorFormat, usage, false))
            return fail(error, "cannot allocate the off-screen output");
    }
    load_pipeline_cache();
    if (options.overlay && (surface_ || offscreen_.image) && !init_overlay(surface_ ? swap_format_ : kColorFormat))
        return fail(error, "cannot set up the overlay renderer");

    begin_commands();
    const u32 white = 0xFFFFFFFFu;
    upload_rgba(white_, &white, 1, 1);
    return true;
}

bool VulkanRenderer::init_overlay(VkFormat format) {
    if (!ImGui::GetCurrentContext()) return false;
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = instance_;
    info.PhysicalDevice = physical_;
    info.Device = device_;
    info.QueueFamily = queue_family_;
    info.Queue = queue_;
    info.DescriptorPoolSize = 256;
    info.MinImageCount = 2;
    info.ImageCount = kFramesInFlight + 1; // the overlay's buffers rotate at least as often as frames
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineCache = pipeline_cache_;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
    overlay_ready_ = ImGui_ImplVulkan_Init(&info);
    return overlay_ready_;
}

// The pipeline cache makes every pipeline after the first run cheap to create, so new render
// states do not hitch.
void VulkanRenderer::load_pipeline_cache() {
    std::vector<u8> blob;
    if (!options_.cache_dir.empty()) read_file(options_.cache_dir / "pipelines.bin", blob);
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.data();
    if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        // Data from another driver version is rejected: start empty.
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_);
    }
}

void VulkanRenderer::save_pipeline_cache() {
    if (!pipeline_cache_ || options_.cache_dir.empty() || pipelines_.size() == pipelines_at_load_) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<u8> blob(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, blob.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(options_.cache_dir, ec);
    write_file(options_.cache_dir / "pipelines.bin", blob.data(), size);
}

void VulkanRenderer::shutdown() {
    if (!device_) {
        if (instance_) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        return;
    }
    vkDeviceWaitIdle(device_);
    for (auto& [handle, tex] : ui_textures_) slot().dead_ui.push_back(tex);
    ui_textures_.clear();
    for (FrameSlot& s : slots_) retire(s);
    if (overlay_ready_) {
        ImGui_ImplVulkan_Shutdown();
        overlay_ready_ = false;
    }
    save_pipeline_cache();
    if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    destroy_image(offscreen_);
    for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
    for (auto& [key, tex] : textures_) destroy_image(tex.image);
    for (auto& [addr, fb] : framebuffers_) {
        destroy_image(fb.color);
        destroy_image(fb.depth);
    }
    for (Image* img : {&white_, &raw_display_, &transfer_tmp_, &capture_}) destroy_image(*img);
    for (FrameSlot& s : slots_) {
        for (Buffer* buf : {&s.vertex, &s.index, &s.staging})
            if (buf->buffer) vmaDestroyBuffer(allocator_, buf->buffer, buf->alloc);
        if (s.fence) vkDestroyFence(device_, s.fence, nullptr);
        if (s.acquired) vkDestroySemaphore(device_, s.acquired, nullptr);
    }
    if (readback_.buffer) vmaDestroyBuffer(allocator_, readback_.buffer, readback_.alloc);
    for (VkSampler s : samplers_)
        if (s) vkDestroySampler(device_, s, nullptr);
    if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (vert_) vkDestroyShaderModule(device_, vert_, nullptr);
    if (frag_) vkDestroyShaderModule(device_, frag_, nullptr);
    destroy_swapchain();
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (allocator_) vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

// --- command flow -----------------------------------------------------------------------------------------------

void VulkanRenderer::begin_commands() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    pass_fb_ = nullptr;
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_set_ = VK_NULL_HANDLE;
}

void VulkanRenderer::end_pass() {
    if (!pass_fb_) return;
    vkCmdEndRendering(cmd_);
    pass_fb_ = nullptr;
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_set_ = VK_NULL_HANDLE;
}

// Ends the current slot's commands and hands them to the GPU.
void VulkanRenderer::submit(VkSubmitInfo& si) {
    end_pass();
    vkEndCommandBuffer(cmd_);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(queue_, 1, &si, slot().fence);
    slot().pending = true;
}

// Waits until a slot's frame has finished, then releases what was waiting on it.
void VulkanRenderer::retire(FrameSlot& s) {
    if (s.pending) {
        vkWaitForFences(device_, 1, &s.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &s.fence);
        s.pending = false;
    }
    for (Image& img : s.dead_images) destroy_image(img);
    s.dead_images.clear();
    if (!s.dead_sets.empty()) vkFreeDescriptorSets(device_, desc_pool_, u32(s.dead_sets.size()), s.dead_sets.data());
    s.dead_sets.clear();
    for (UiTexture& tex : s.dead_ui) {
        ImGui_ImplVulkan_RemoveTexture(tex.set);
        destroy_image(tex.image);
    }
    s.dead_ui.clear();
    s.vertex.used = s.index.used = s.staging.used = 0;
}

// Moves recording on to the next slot once its previous frame is done.
void VulkanRenderer::advance_slot() {
    slot_index_ = (slot_index_ + 1) % kFramesInFlight;
    retire(slot());
    cmd_ = slot().cmd;
    begin_commands();
}

// Submits everything recorded so far and waits until the GPU is idle: results can be read back
// and any resource can be released afterwards.
void VulkanRenderer::flush() {
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit(si);
    for (FrameSlot& s : slots_) retire(s);
    begin_commands();
}

void VulkanRenderer::begin_pass(Framebuffer* fb) {
    if (pass_fb_ == fb) return;
    end_pass();
    transition(fb->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(fb->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = fb->color.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = fb->depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {fb->color.width, fb->color.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd_, &ri);

    VkViewport vp{0.0f, 0.0f, f32(fb->color.width), f32(fb->color.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    pass_fb_ = fb;
    fb->drawn = true;
    fb->last_used = frame_;
}

// --- render targets ---------------------------------------------------------------------------------------------

Framebuffer* VulkanRenderer::get_framebuffer(u32 addr, u32 stride, u32 format) {
    addr &= 0x041FFFFFu;
    auto it = framebuffers_.find(addr);
    if (it != framebuffers_.end() && it->second.stride == stride) {
        it->second.format = format;
        return &it->second;
    }
    if (it != framebuffers_.end()) { // same address, different shape: start over
        if (pass_fb_ == &it->second) end_pass();
        flush();
        destroy_image(it->second.color);
        destroy_image(it->second.depth);
        framebuffers_.erase(it);
    }

    Framebuffer fb;
    fb.addr = addr;
    fb.stride = stride ? stride : 512;
    fb.format = format;
    fb.width = std::min<u32>(fb.stride, 512);
    fb.height = fb.stride >= kDisplayWidth ? kDisplayHeight : fb.width;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(fb.color, fb.width * scale_, fb.height * scale_, kColorFormat, usage, false) ||
        !create_image(fb.depth, fb.width * scale_, fb.height * scale_, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true))
        P2_FATAL("out of GPU memory creating a %ux%u render target", fb.width * scale_, fb.height * scale_);
    LOG_CH("ge", "new render target at %08x: %ux%u (stride %u, format %u)", addr, fb.width, fb.height, fb.stride, format);

    Framebuffer& stored = framebuffers_[addr] = fb;
    // Start from black with zero alpha and the far depth, like freshly cleared video memory.
    end_pass();
    transition(stored.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, stored.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    transition(stored.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue far_depth{0.0f, 0};
    VkImageSubresourceRange drange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd_, stored.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_depth, 1, &drange);
    return &stored;
}

Framebuffer* VulkanRenderer::find_framebuffer(u32 addr) {
    if ((mem::physical(addr) >> 24) != 0x04) return nullptr;
    addr &= 0x041FFFFFu;
    for (auto& [base, fb] : framebuffers_)
        if (addr >= base && addr < base + fb.byte_size()) return &fb;
    return nullptr;
}

// --- textures ---------------------------------------------------------------------------------------------------

void VulkanRenderer::upload_rgba(Image& dst, const u32* pixels, u32 w, u32 h) {
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    if (slot().staging.used + bytes > slot().staging.size) flush();
    end_pass();
    std::memcpy(slot().staging.mapped + slot().staging.used, pixels, bytes);
    transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = slot().staging.used;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd_, slot().staging.buffer, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    slot().staging.used += align_up<VkDeviceSize>(bytes, 16);
}

Texture* VulkanRenderer::get_texture(const TextureDesc& desc, const u8* clut) {
    TextureKey key{desc.addr, desc.format | (desc.swizzled ? 0x100u : 0u), desc.width, desc.height, desc.buf_width,
                   desc.clut_format | (desc.clut_shift << 4) | (desc.clut_mask << 12) | (desc.clut_base << 20),
                   desc.clut_hash};
    auto it = textures_.find(key);
    // A cached texture is good without looking at its pixels again when they were compared this
    // frame and either the game has not touched the texture state since, or its code has not run
    // since the comparison and so cannot have changed them.
    if (it != textures_.end() && it->second.checked_frame == frame_ &&
        (!desc.revalidate || it->second.checked_epoch == desc.memory_epoch)) {
        it->second.used_frame = frame_;
        return &it->second;
    }

    const u32 bytes = ge::texture_byte_size(desc);
    if (!mem::valid(desc.addr, bytes)) return nullptr;
    const u64 hash = hash_bytes(mem::ptr(desc.addr), bytes);
    if (it != textures_.end() && it->second.content_hash == hash) {
        it->second.checked_frame = it->second.used_frame = frame_;
        it->second.checked_epoch = desc.memory_epoch;
        return &it->second;
    }

    if (!ge::decode_texture(desc, clut, decode_scratch_)) return nullptr;
    if (!host::options().texture_dump_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(host::options().texture_dump_dir, ec);
        std::vector<u32> opaque = decode_scratch_; // show colour and alpha side by side
        opaque.resize(decode_scratch_.size() * 2);
        std::vector<u32> sheet(size_t(desc.width) * 2 * desc.height);
        for (u32 y = 0; y < desc.height; ++y) {
            for (u32 x = 0; x < desc.width; ++x) {
                u32 p = decode_scratch_[size_t(y) * desc.width + x], a = p >> 24;
                sheet[size_t(y) * desc.width * 2 + x] = p | 0xFF000000u;
                sheet[size_t(y) * desc.width * 2 + desc.width + x] = a | (a << 8) | (a << 16) | 0xFF000000u;
            }
        }
        std::string name = format("%06llu_%08x_f%u%s_%ux%u_bw%u_%016llx.png", static_cast<unsigned long long>(frame_), desc.addr,
                                  desc.format, desc.swizzled ? "s" : "", desc.width, desc.height, desc.buf_width,
                                  static_cast<unsigned long long>(hash));
        host::write_png((host::options().texture_dump_dir / name).string(), desc.width * 2, desc.height, sheet.data());
    }
    Texture* tex;
    if (it != textures_.end()) {
        tex = &it->second;
    } else {
        Texture fresh;
        if (!create_image(fresh.image, desc.width, desc.height, kColorFormat,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false))
            return nullptr;
        tex = &(textures_[key] = fresh);
    }
    upload_rgba(tex->image, decode_scratch_.data(), desc.width, desc.height);
    tex->content_hash = hash;
    tex->bytes = bytes;
    tex->checked_frame = tex->used_frame = frame_;
    tex->checked_epoch = desc.memory_epoch;
    return tex;
}

VkDescriptorSet VulkanRenderer::get_descriptor(VkImageView view, u32 sampler) {
    const u64 key = (u64(reinterpret_cast<uintptr_t>(view)) << 4) ^ sampler;
    auto it = descriptors_.find(key);
    if (it != descriptors_.end()) return it->second;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
        // Pool exhausted: finish pending work, start over with an empty cache.
        flush();
        vkResetDescriptorPool(device_, desc_pool_, 0);
        descriptors_.clear();
        if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) P2_FATAL("cannot allocate a descriptor set");
    }
    VkDescriptorImageInfo info{samplers_[sampler], view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    descriptors_[key] = set;
    return set;
}

// --- pipelines --------------------------------------------------------------------------------------------------

VkPipeline VulkanRenderer::get_pipeline(const PipelineKey& key) {
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbinding{0, sizeof(ge::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[5] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ge::Vertex, x)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ge::Vertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ge::Vertex, color)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ge::Vertex, specular)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(ge::Vertex, fog)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbinding;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = key.topology == u8(ge::Topology::Triangles) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                  : key.topology == u8(ge::Topology::Lines) ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                            : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = VK_TRUE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // culling happens while assembling primitives
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    static constexpr VkCompareOp compare[8] = {VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_ALWAYS,          VK_COMPARE_OP_EQUAL,
                                               VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS,          VK_COMPARE_OP_LESS_OR_EQUAL,
                                               VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = key.depth_test || key.depth_write;
    ds.depthWriteEnable = key.depth_write;
    ds.depthCompareOp = key.depth_test ? compare[key.depth_func & 7] : VK_COMPARE_OP_ALWAYS;

    // Source-alpha factors read the second fragment output so the stored alpha can differ from it.
    static constexpr VkBlendFactor src_factor[11] = {
        VK_BLEND_FACTOR_DST_COLOR,        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,       VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_SRC1_ALPHA,       VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR};
    static constexpr VkBlendFactor dst_factor[11] = {
        VK_BLEND_FACTOR_SRC_COLOR,        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,       VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_SRC1_ALPHA,       VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR};
    static constexpr VkBlendOp ops[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                         VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,      VK_BLEND_OP_MAX};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = key.color_mask;
    att.blendEnable = key.blend;
    // The key's factor values 11..14 encode the constant-colour variants chosen in draw().
    auto factor = [](u8 v, const VkBlendFactor* table) {
        switch (v) {
        case 11: return VK_BLEND_FACTOR_ZERO;
        case 12: return VK_BLEND_FACTOR_ONE;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return table[v <= 10 ? v : 10];
        }
    };
    att.srcColorBlendFactor = factor(key.blend_src, src_factor);
    att.dstColorBlendFactor = factor(key.blend_dst, dst_factor);
    att.colorBlendOp = ops[key.blend_eq < 6 ? key.blend_eq : 0];
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // alpha is the stencil value: stored as is
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 3;
    dyn.pDynamicStates = dynamic;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kColorFormat;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = pipeline_layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pci, nullptr, &pipeline) != VK_SUCCESS)
        P2_FATAL("cannot create a graphics pipeline");
    pipelines_[key] = pipeline;
    return pipeline;
}

// --- drawing ----------------------------------------------------------------------------------------------------

void VulkanRenderer::draw(const DrawState& s, ge::Topology topology, const ge::Vertex* vertices, u32 vertex_count,
                          const u16* indices, u32 index_count) {
    const VkDeviceSize vbytes = VkDeviceSize(vertex_count) * sizeof(ge::Vertex), ibytes = VkDeviceSize(index_count) * 2;
    if (slot().vertex.used + vbytes > slot().vertex.size || slot().index.used + ibytes > slot().index.size) flush();

    Framebuffer* fb = get_framebuffer(s.fb_addr, s.fb_stride, s.fb_format);

    // Resolve the texture first: uploads and layout changes cannot happen inside a render pass.
    VkImageView view = white_.view;
    f32 uv_scale[2] = {1.0f, 1.0f};
    bool textured = s.texture;
    if (textured) {
        if (Framebuffer* source = find_framebuffer(s.tex.addr)) {
            if (source == fb) {
                textured = false; // sampling the target being drawn to is not supported yet
                report_once(1, "a draw samples the render target it draws to (%08x); it is drawn untextured", fb->addr);
            } else {
                if (pass_fb_ == source) end_pass();
                if (source->color.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                    end_pass();
                    transition(source->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                view = source->color.view;
                uv_scale[0] = f32(s.tex.width) / f32(source->width);
                uv_scale[1] = f32(s.tex.height) / f32(source->height);
                source->last_used = frame_;
            }
        } else if (Texture* tex = get_texture(s.tex, ge::state().clut)) {
            if (tex->image.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                end_pass();
                transition(tex->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            view = tex->image.view;
        } else {
            textured = false;
        }
    }
    if (white_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        end_pass();
        transition(white_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Scissor, in target pixels.
    s32 x1 = std::clamp(s.scissor_x1, 0, s32(fb->width)), y1 = std::clamp(s.scissor_y1, 0, s32(fb->height));
    s32 x2 = std::clamp(s.scissor_x2, 0, s32(fb->width)), y2 = std::clamp(s.scissor_y2, 0, s32(fb->height));
    if (x2 <= x1 || y2 <= y1) return;

    PipelineKey key;
    key.topology = u8(topology);
    PushConstants pc{};
    pc.pixel_to_ndc[0] = 2.0f / f32(fb->width);
    pc.pixel_to_ndc[1] = 2.0f / f32(fb->height);
    pc.uv_scale[0] = uv_scale[0];
    pc.uv_scale[1] = uv_scale[1];
    f32 blend_constant[4] = {0, 0, 0, 0};

    if (s.clear_mode) {
        key.color_mask = (s.clear_color ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT) : 0) |
                         (s.clear_alpha ? VK_COLOR_COMPONENT_A_BIT : 0);
        key.depth_write = s.clear_depth;
        pc.flags = kFlagClear;
    } else {
        u32 mask = 0;
        if ((s.write_mask_rgb & 0x0000FF) != 0x0000FF) mask |= VK_COLOR_COMPONENT_R_BIT;
        if ((s.write_mask_rgb & 0x00FF00) != 0x00FF00) mask |= VK_COLOR_COMPONENT_G_BIT;
        if ((s.write_mask_rgb & 0xFF0000) != 0xFF0000) mask |= VK_COLOR_COMPONENT_B_BIT;

        // Alpha doubles as the stencil buffer: it only changes when a stencil operation writes it.
        if (s.stencil_test && s.write_mask_alpha != 0xFF) {
            if (s.stencil_zpass == 2) { // replace with the reference value
                mask |= VK_COLOR_COMPONENT_A_BIT;
                pc.flags |= kFlagWriteStencil;
                pc.stencil_value = f32(s.stencil_ref) / 255.0f;
            } else if (s.stencil_zpass == 1) { // zero
                mask |= VK_COLOR_COMPONENT_A_BIT;
                pc.flags |= kFlagWriteStencil;
                pc.stencil_value = 0.0f;
            } else if (s.stencil_zpass > 2) {
                report_once(0x100 + s.stencil_zpass, "stencil operation %u on depth pass is not carried out", s.stencil_zpass);
            }
            if (s.stencil_sfail != 0 || s.stencil_zfail != 0)
                report_once(0x200 + s.stencil_sfail * 8 + s.stencil_zfail,
                            "stencil operations on test or depth failure (%u, %u) are not carried out", s.stencil_sfail,
                            s.stencil_zfail);
        }
        key.color_mask = u8(mask);
        key.depth_test = s.depth_test;
        key.depth_func = s.depth_func;
        key.depth_write = s.depth_test && s.depth_write;

        if (s.blend) {
            key.blend = 1;
            key.blend_eq = s.blend_eq;
            // Fixed factors share Vulkan's single blend constant; fold exact 0 and 1 into plain factors.
            auto fixed = [&](u32 value, u8 other_is_fixed, u32 other_value) -> u8 {
                value &= 0xFFFFFF;
                if (value == 0) return 11;
                if (value == 0xFFFFFF) return 12;
                if (other_is_fixed && blend_constant[3] == 1.0f && ((other_value ^ value) & 0xFFFFFF) == 0xFFFFFF) return 13;
                f32 c[4];
                unpack_color(value, c);
                blend_constant[0] = c[0], blend_constant[1] = c[1], blend_constant[2] = c[2], blend_constant[3] = 1.0f;
                return 10;
            };
            key.blend_src = s.blend_src <= 10 ? s.blend_src : 10;
            key.blend_dst = s.blend_dst <= 10 ? s.blend_dst : 10;
            if (key.blend_src == 10) key.blend_src = fixed(s.blend_fix_a, false, 0);
            if (key.blend_dst == 10) key.blend_dst = fixed(s.blend_fix_b, s.blend_src == 10, s.blend_fix_a);
            if (s.blend_src == 6 || s.blend_src == 7 || s.blend_dst == 6 || s.blend_dst == 7) pc.flags |= kFlagDoubleAlpha;
        }

        if (textured) {
            pc.flags |= kFlagTexture | (u32(s.tex_func & 7) << 1);
            if (s.tex_alpha) pc.flags |= kFlagTexAlpha;
            if (s.color_double) pc.flags |= kFlagColorDouble;
        }
        if (s.fog) pc.flags |= kFlagFog;
        if (s.alpha_test) {
            pc.flags |= kFlagAlphaTest;
            pc.alpha_test = s.alpha_func | (u32(s.alpha_ref) << 8) | (u32(s.alpha_mask) << 16);
        }
        if (s.color_test) {
            pc.flags |= kFlagColorTest | (u32(s.color_func & 3) << 9);
            pc.color_ref = s.color_ref & 0xFFFFFF;
            pc.color_mask = s.color_mask & 0xFFFFFF;
        }
        if (s.flat_shade) pc.flags |= kFlagFlat;
        unpack_color(s.tex_env_color, pc.tex_env_color);
        unpack_color(s.fog_color, pc.fog_color);
    }

    VkPipeline pipeline = get_pipeline(key);
    u32 sampler = (s.tex.linear_min ? 1u : 0u) | (s.tex.linear_mag ? 2u : 0u) | (s.tex.clamp_s ? 4u : 0u) | (s.tex.clamp_t ? 8u : 0u);
    if (view == white_.view) sampler = 0;
    VkDescriptorSet set = get_descriptor(view, sampler); // may flush, so it comes before the pass begins

    begin_pass(fb);
    if (pipeline != bound_pipeline_) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bound_pipeline_ = pipeline;
    }
    if (set != bound_set_) {
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
        bound_set_ = set;
    }
    VkRect2D scissor{{x1 * s32(scale_), y1 * s32(scale_)}, {u32(x2 - x1) * scale_, u32(y2 - y1) * scale_}};
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdSetBlendConstants(cmd_, blend_constant);
    vkCmdPushConstants(cmd_, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    std::memcpy(slot().vertex.mapped + slot().vertex.used, vertices, vbytes);
    std::memcpy(slot().index.mapped + slot().index.used, indices, ibytes);
    VkDeviceSize voffset = slot().vertex.used;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &slot().vertex.buffer, &voffset);
    vkCmdBindIndexBuffer(cmd_, slot().index.buffer, slot().index.used, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd_, index_count, 1, 0, 0, 0);
    slot().vertex.used += align_up<VkDeviceSize>(vbytes, 8);
    slot().index.used += align_up<VkDeviceSize>(ibytes, 4);
}

void VulkanRenderer::blit(Image& src, VkOffset3D s0, VkOffset3D s1, Image& dst, VkOffset3D d0, VkOffset3D d1, VkFilter filter) {
    end_pass();
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = s0;
    region.srcOffsets[1] = s1;
    region.dstOffsets[0] = d0;
    region.dstOffsets[1] = d1;
    vkCmdBlitImage(cmd_, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region, filter);
}

void VulkanRenderer::block_transfer(u32 src_addr, u32 src_stride, u32 sx, u32 sy, u32 dst_addr, u32 dst_stride, u32 dx,
                                    u32 dy, u32 w, u32 h, u32 bpp) {
    Framebuffer* sfb = find_framebuffer(src_addr);
    Framebuffer* dfb = find_framebuffer(dst_addr);
    if (sfb && !sfb->drawn) sfb = nullptr;
    const s32 k = s32(scale_);

    if (!sfb && !dfb) { // plain memory to memory
        for (u32 y = 0; y < h; ++y) {
            u32 from = src_addr + ((sy + y) * src_stride + sx) * bpp, to = dst_addr + ((dy + y) * dst_stride + dx) * bpp;
            if (!mem::valid(from, w * bpp) || !mem::valid(to, w * bpp)) return;
            std::memmove(mem::ptr(to), mem::ptr(from), size_t(w) * bpp);
        }
        invalidate_textures();
        return;
    }
    if (sfb && dfb) {
        if (sfb == dfb) return;
        blit(sfb->color, {s32(sx) * k, s32(sy) * k, 0}, {s32(sx + w) * k, s32(sy + h) * k, 1}, dfb->color,
             {s32(dx) * k, s32(dy) * k, 0}, {s32(dx + w) * k, s32(dy + h) * k, 1}, VK_FILTER_NEAREST);
        dfb->drawn = true;
        return;
    }
    if (dfb) { // guest memory into a render target
        if (w > transfer_tmp_.width || h > transfer_tmp_.height) return;
        decode_scratch_.resize(size_t(w) * h);
        const u32 format = bpp == 4 ? 3 : dfb->format;
        for (u32 y = 0; y < h; ++y) {
            u32 from = src_addr + ((sy + y) * src_stride + sx) * bpp;
            if (!mem::valid(from, w * bpp)) return;
            ge::convert_pixels(mem::ptr(from), format, w, decode_scratch_.data() + size_t(y) * w);
        }
        upload_rgba(transfer_tmp_, decode_scratch_.data(), w, h);
        blit(transfer_tmp_, {0, 0, 0}, {s32(w), s32(h), 1}, dfb->color, {s32(dx) * k, s32(dy) * k, 0},
             {s32(dx + w) * k, s32(dy + h) * k, 1}, scale_ > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        dfb->drawn = true;
        return;
    }

    // Render target back into guest memory.
    if (w > transfer_tmp_.width || h > transfer_tmp_.height) return;
    blit(sfb->color, {s32(sx) * k, s32(sy) * k, 0}, {s32(sx + w) * k, s32(sy + h) * k, 1}, transfer_tmp_, {0, 0, 0},
         {s32(w), s32(h), 1}, VK_FILTER_NEAREST);
    transition(transfer_tmp_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd_, transfer_tmp_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
    flush();
    for (u32 y = 0; y < h; ++y) {
        u32 to = dst_addr + ((dy + y) * dst_stride + dx) * bpp;
        if (!mem::valid(to, w * bpp)) return;
        const u8* row = readback_.mapped + size_t(y) * w * 4;
        if (bpp == 4) {
            std::memcpy(mem::ptr(to), row, size_t(w) * 4);
        } else {
            for (u32 x = 0; x < w; ++x) {
                u32 r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2], a = row[x * 4 + 3];
                u16 out;
                switch (sfb->format) {
                case 0: out = u16((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); break;
                case 1: out = u16((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a >= 128 ? 0x8000 : 0)); break;
                default: out = u16((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12)); break;
                }
                mem::w16(to + x * 2, out);
            }
        }
    }
    invalidate_textures();
}

// --- presentation -----------------------------------------------------------------------------------------------

// Picks the image holding what the game wants on screen: a render target, or failing that the raw
// contents of video memory (for frames the game drew with the CPU).
Image* VulkanRenderer::display_image(const ge::DisplayTarget& target, u32* src_w, u32* src_h) {
    if (!target.addr) return nullptr;
    auto it = framebuffers_.find(target.addr & 0x041FFFFFu);
    if (it != framebuffers_.end() && it->second.drawn) {
        *src_w = std::min(kDisplayWidth, it->second.width) * scale_;
        *src_h = std::min(kDisplayHeight, it->second.height) * scale_;
        it->second.last_used = frame_;
        return &it->second.color;
    }
    const u32 stride = std::clamp<u32>(target.stride, kDisplayWidth, 512);
    const u32 bpp = target.format == 3 ? 4 : 2;
    if (!mem::valid(target.addr, stride * kDisplayHeight * bpp)) return nullptr;
    decode_scratch_.assign(size_t(512) * kDisplayHeight, 0xFF000000u);
    for (u32 y = 0; y < kDisplayHeight; ++y)
        ge::convert_pixels(mem::ptr(target.addr + y * stride * bpp), target.format, kDisplayWidth, decode_scratch_.data() + size_t(y) * 512);
    upload_rgba(raw_display_, decode_scratch_.data(), 512, kDisplayHeight);
    *src_w = kDisplayWidth;
    *src_h = kDisplayHeight;
    return &raw_display_;
}

// Builds the final picture in `image`: the game's display scaled to fit, then the overlay.
void VulkanRenderer::compose(VkImage image, VkImageView view, VkExtent2D extent, VkImageLayout final_layout, Image* source,
                             u32 src_w, u32 src_h) {
    end_pass();
    transition_raw(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (source) {
        s32 w = s32(extent.width), h = s32(extent.height);
        if (fit_ != Fit::Stretch) {
            // Largest rectangle of the display's aspect ratio that fits, optionally at a whole multiple.
            f32 factor = std::min(f32(extent.width) / f32(kDisplayWidth), f32(extent.height) / f32(kDisplayHeight));
            if (fit_ == Fit::Integer && factor >= 1.0f) factor = std::floor(factor);
            w = s32(f32(kDisplayWidth) * factor);
            h = s32(f32(kDisplayHeight) * factor);
        }
        const s32 x = (s32(extent.width) - w) / 2, y = (s32(extent.height) - h) / 2;
        transition(*source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = {s32(src_w), s32(src_h), 1};
        region.dstOffsets[0] = {x, y, 0};
        region.dstOffsets[1] = {x + w, y + h, 1};
        vkCmdBlitImage(cmd_, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, smooth_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }

    ImDrawData* overlay = nullptr;
    if (overlay_ready_ && overlay_builder_) {
        ImGui_ImplVulkan_NewFrame();
        overlay_builder_();
        overlay = ImGui::GetDrawData();
        if (overlay && overlay->CmdListsCount == 0) overlay = nullptr;
    }
    if (!overlay) {
        transition_raw(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout);
        return;
    }
    for (auto& [handle, tex] : ui_textures_) transition(tex.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition_raw(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd_, &ri);
    ImGui_ImplVulkan_RenderDrawData(overlay, cmd_);
    vkCmdEndRendering(cmd_);
    transition_raw(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout);
}

void VulkanRenderer::set_scale(u32 scale) {
    scale = std::clamp<u32>(scale, 1, 10);
    if (scale == scale_) return;
    // Render targets are tied to the scale: drop them all and let the game redraw.
    flush();
    for (auto& [addr, fb] : framebuffers_) {
        destroy_image(fb.color);
        destroy_image(fb.depth);
    }
    framebuffers_.clear();
    last_display_ = nullptr;
    scale_ = scale;
}

void VulkanRenderer::set_vsync(bool on) {
    if (options_.vsync == on) return;
    options_.vsync = on;
    if (swapchain_) {
        flush();
        vkDeviceWaitIdle(device_); // presentation may still hold the old images
        create_swapchain();
    }
}

void VulkanRenderer::output_size(u32& width, u32& height) const {
    if (surface_) {
        width = swap_extent_.width;
        height = swap_extent_.height;
    } else {
        width = offscreen_.width;
        height = offscreen_.height;
    }
}

bool VulkanRenderer::capture_output(std::vector<u8>& rgba, u32& width, u32& height) {
    if (!offscreen_.image || VkDeviceSize(offscreen_.width) * offscreen_.height * 4 > readback_.size) return false;
    width = offscreen_.width;
    height = offscreen_.height;
    transition(offscreen_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd_, offscreen_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
    flush();
    rgba.assign(readback_.mapped, readback_.mapped + size_t(width) * height * 4);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return true;
}

u64 VulkanRenderer::create_ui_texture(const u32* rgba, u32 width, u32 height) {
    if (!overlay_ready_ || !width || !height) return 0;
    UiTexture tex;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(tex.image, width, height, kColorFormat, usage, false)) return 0;
    upload_rgba(tex.image, rgba, width, height);
    transition(tex.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    tex.set = ImGui_ImplVulkan_AddTexture(samplers_[1 | 2 | 4 | 8], tex.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const u64 handle = u64(reinterpret_cast<uintptr_t>(tex.set));
    ui_textures_[handle] = tex;
    return handle;
}

void VulkanRenderer::destroy_ui_texture(u64 handle) {
    auto it = ui_textures_.find(handle);
    if (it == ui_textures_.end()) return;
    slot().dead_ui.push_back(it->second);
    ui_textures_.erase(it);
}

void VulkanRenderer::present(const ge::DisplayTarget& target) {
    u32 src_w = 0, src_h = 0;
    Image* source = display_image(target, &src_w, &src_h);
    last_display_ = source;
    last_display_w_ = src_w;
    last_display_h_ = src_h;

    bool recreate = false;
    bool submitted = false;
    if (!output_visible_) {
        // Nothing to show: the frame's work is submitted below like any other.
    } else if (swapchain_ || (surface_ && create_swapchain())) {
        u32 index = 0;
        VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, slot().acquired, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate = true;
        } else if (acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR) {
            compose(swap_images_[index], swap_views_[index], swap_extent_, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, source, src_w, src_h);
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &slot().acquired;
            si.pWaitDstStageMask = &wait_stage;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &render_sems_[index];
            submit(si);
            submitted = true;
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &render_sems_[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain_;
            pi.pImageIndices = &index;
            VkResult result = vkQueuePresentKHR(queue_, &pi);
            recreate = result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR;
        }
    } else if (offscreen_.image) {
        compose(offscreen_.image, offscreen_.view, {offscreen_.width, offscreen_.height}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                source, src_w, src_h);
        offscreen_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (!submitted) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit(si);
    }
    // Record the next frame while the GPU works on this one.
    advance_slot();
    if (recreate) {
        flush();
        vkDeviceWaitIdle(device_); // presentation may still hold the old images
        create_swapchain();
    }

    // Drop textures that have not been used for a while. A frame still in flight may refer to
    // them, so they go with the current slot and are released when it next comes round.
    ++frame_;
    if ((frame_ & 63) == 0) {
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (frame_ - it->second.used_frame > kTextureLifetimeFrames) {
                for (u32 s = 0; s < 16; ++s) {
                    auto d = descriptors_.find((u64(reinterpret_cast<uintptr_t>(it->second.image.view)) << 4) ^ s);
                    if (d != descriptors_.end()) {
                        slot().dead_sets.push_back(d->second);
                        descriptors_.erase(d);
                    }
                }
                slot().dead_images.push_back(it->second.image);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool VulkanRenderer::capture(std::vector<u8>& rgba, u32& width, u32& height) {
    width = kDisplayWidth;
    height = kDisplayHeight;
    rgba.assign(size_t(width) * height * 4, 0);
    if (last_display_ && last_display_->image) {
        blit(*last_display_, {0, 0, 0}, {s32(last_display_w_), s32(last_display_h_), 1}, capture_, {0, 0, 0},
             {s32(width), s32(height), 1}, VK_FILTER_LINEAR);
        transition(capture_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd_, capture_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
        flush();
        std::memcpy(rgba.data(), readback_.mapped, rgba.size());
    }
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255; // the alpha channel is the stencil buffer, not opacity
    return true;
}

} // namespace

std::unique_ptr<Renderer> create_vulkan_renderer(const RendererOptions& options, std::string* error) {
    auto renderer = std::make_unique<VulkanRenderer>();
    if (!renderer->init(options, error)) return nullptr;
    return renderer;
}

} // namespace p2::gfx
