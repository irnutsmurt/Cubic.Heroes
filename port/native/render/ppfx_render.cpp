// The native PPFX renderer (docs/NATIVE_R3.md, docs/NATIVE_R3B.md): a thread
// of its own, with a Vulkan queue and command pool of its own on the existing
// renderer's device, rendering PPFX's records (ppfx_recorder.cpp) into images
// of its own.
//
// From a record's explicit description (ppfx_native.h State): its programs go
// through the same translators and the shared SPIR-V cache, with the
// parameters the record's own fields give (the textures' formats, the
// samplers, the surface); the vertex inputs through the toolkit's vertex
// fetch, from the record's snapshot of their bytes; the render state straight
// into the pipeline. No register file is read or written here: where a
// toolkit helper takes a hardware word (the translators' shader control,
// the remap crossbar), it is made right at the call from the record's
// fields.
//
// Images. A surface address PPFX renders to gets a native image of its own
// (the target registry); the engine's surfaces are never bound here. What
// PPFX reads that other work produced arrives through the bridge: the walker
// copies the engine's surface, in the engine's own command stream, into a
// slot image, and the record names the slot and the semaphore that orders
// it; the copy into the native image happens here, after the wait. A small
// static texture comes from the bytes the recorder took. A draw that samples
// its own target samples a copy taken just before it, as the engine's does.
//
// Synchronisation within the native queue (DOTHEROES_PPFX_BARRIERS): by
// default each image's last write and reads since are tracked and each
// access gets the barrier its hazard needs, in the layout it needs
// (R2's executor, r1_replay.c r2_access); "full" is R3's way -- every image
// in GENERAL and a full barrier between operations -- kept for comparison.
// Across queues: the engine's copies into slots are ordered by the
// semaphore their submission signals, waited on before the first record that
// needs them.
//
// Lifetime. Work is recorded into batches (a command buffer, a fence, a
// descriptor pool, an upload buffer and timestamp queries each), submitted at
// frame ends and when they fill. A batch's completion (its fence) releases
// what it used: slots go back to the bridge, retired images are destroyed,
// comparisons run. Records are released to the recorder (`done`) once
// recorded: nothing here reads a record after that.
//
// R4's handbacks (docs/NATIVE_R4.md): the walker asks for the native image of
// a surface the engine is about to use (handback_take); this thread
// finishes every record walked, puts the image in TRANSFER_SRC (or GENERAL),
// and submits with a semaphore the engine's next submission waits on. The
// record the walker reaches next waits for an engine signal past that copy
// (so nothing here touches the image while the engine reads it) and frees
// the semaphore once its own batch is done.
//
// Comparison. For the records the walker copied the engine's output of
// (compare_wanted), the native output is read back after the record and
// compared byte for byte once both copies are complete. Every comparison
// asked for ends as exactly one of: equal, differ, skipped (and why), or
// unfinished at exit (render_drain waits for the in-flight ones first).
// DOTHEROES_PPFX_DUMP=<dir>: the first differing outputs, both sides, raw.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ppfx_native.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

#include "rsx_dispatch.h"
#include "rsx_draw_engine.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_texture_layout.h"
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"
#include "rsx_vulkan_bridge.h"

extern "C" uint8_t* vm_base;
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);

namespace ppfx {
namespace {

// ---- Vulkan entry points ------------------------------------------------------

#define PPFX_VK_DEVICE(X) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkCreatePipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdCopyBuffer) \
    X(vkCmdClearAttachments) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCreateQueryPool) \
    X(vkGetQueryPoolResults) X(vkCreateFence) X(vkGetFenceStatus) X(vkResetFences) X(vkWaitForFences) \
    X(vkQueueSubmit) X(vkCreateSemaphore) X(vkCmdBlitImage) X(vkCmdBeginQuery) X(vkCmdEndQuery)

struct Fn {
#define X(f) PFN_##f f;
    PPFX_VK_DEVICE(X)
#undef X
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
} F;

// R5: asp's consecutive draws into the same targets share one render pass
// (DOTHEROES_ASP_PASS_MERGE=0: a pass a draw, as before). The pass is left
// open after a draw; anything that must be recorded outside a render pass
// -- a barrier, a copy, a clear of an image, a query reset, another pass,
// the command buffer's end -- ends it first: those entry points of F are
// wrapped at init. A command buffer reset forgets it; a framebuffer
// destroyed while its pass is open ends it.
int asp_native();
int pass_merge()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_PASS_MERGE"); on = asp_native() && !(e && *e == '0'); }
    return on;
}
struct OpenPass { VkCommandBuffer cb; VkRenderPass rp; VkFramebuffer fb; uint32_t w, h; } s_open = {};
uint64_t s_pass_merged = 0;
PFN_vkCmdEndRenderPass s_real_end_pass;
void end_open_pass(VkCommandBuffer cb)
{
    if (s_open.cb && s_open.cb == cb) { s_open.cb = VK_NULL_HANDLE; s_real_end_pass(cb); }
}
#define PASS_WRAP(name, params, args) \
    PFN_##name s_real_##name; \
    VKAPI_ATTR void VKAPI_CALL w_##name params { end_open_pass(cb); s_real_##name args; }
PASS_WRAP(vkCmdPipelineBarrier, (VkCommandBuffer cb, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags c, uint32_t d,
                                 const VkMemoryBarrier* e, uint32_t f, const VkBufferMemoryBarrier* g, uint32_t h,
                                 const VkImageMemoryBarrier* i), (cb, a, b, c, d, e, f, g, h, i))
PASS_WRAP(vkCmdCopyImage, (VkCommandBuffer cb, VkImage a, VkImageLayout b, VkImage c, VkImageLayout d, uint32_t e, const VkImageCopy* f),
          (cb, a, b, c, d, e, f))
PASS_WRAP(vkCmdCopyImageToBuffer, (VkCommandBuffer cb, VkImage a, VkImageLayout b, VkBuffer c, uint32_t d, const VkBufferImageCopy* e),
          (cb, a, b, c, d, e))
PASS_WRAP(vkCmdCopyBufferToImage, (VkCommandBuffer cb, VkBuffer a, VkImage b, VkImageLayout c, uint32_t d, const VkBufferImageCopy* e),
          (cb, a, b, c, d, e))
PASS_WRAP(vkCmdClearColorImage, (VkCommandBuffer cb, VkImage a, VkImageLayout b, const VkClearColorValue* c, uint32_t d,
                                 const VkImageSubresourceRange* e), (cb, a, b, c, d, e))
PASS_WRAP(vkCmdClearDepthStencilImage, (VkCommandBuffer cb, VkImage a, VkImageLayout b, const VkClearDepthStencilValue* c, uint32_t d,
                                        const VkImageSubresourceRange* e), (cb, a, b, c, d, e))
PASS_WRAP(vkCmdCopyBuffer, (VkCommandBuffer cb, VkBuffer a, VkBuffer b, uint32_t c, const VkBufferCopy* d), (cb, a, b, c, d))
PASS_WRAP(vkCmdResetQueryPool, (VkCommandBuffer cb, VkQueryPool a, uint32_t b, uint32_t c), (cb, a, b, c))
PASS_WRAP(vkCmdBlitImage, (VkCommandBuffer cb, VkImage a, VkImageLayout b, VkImage c, VkImageLayout d, uint32_t e, const VkImageBlit* f,
                           VkFilter g), (cb, a, b, c, d, e, f, g))
PASS_WRAP(vkCmdBeginRenderPass, (VkCommandBuffer cb, const VkRenderPassBeginInfo* a, VkSubpassContents b), (cb, a, b))
#undef PASS_WRAP
PFN_vkEndCommandBuffer s_real_vkEndCommandBuffer;
VKAPI_ATTR VkResult VKAPI_CALL w_vkEndCommandBuffer(VkCommandBuffer cb)
{
    end_open_pass(cb);
    return s_real_vkEndCommandBuffer(cb);
}
PFN_vkResetCommandBuffer s_real_vkResetCommandBuffer;
VKAPI_ATTR VkResult VKAPI_CALL w_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f)
{
    if (s_open.cb == cb) s_open.cb = VK_NULL_HANDLE;
    return s_real_vkResetCommandBuffer(cb, f);
}
PFN_vkDestroyFramebuffer s_real_vkDestroyFramebuffer;
VKAPI_ATTR void VKAPI_CALL w_vkDestroyFramebuffer(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks* a)
{
    if (s_open.cb && s_open.fb == fb) end_open_pass(s_open.cb);
    s_real_vkDestroyFramebuffer(dev, fb, a);
}
void pass_wrap_install()
{
    if (!pass_merge()) return;
    s_real_end_pass = F.vkCmdEndRenderPass;
#define W(f) s_real_##f = F.f; F.f = w_##f;
    W(vkCmdPipelineBarrier) W(vkCmdCopyImage) W(vkCmdCopyImageToBuffer) W(vkCmdCopyBufferToImage) W(vkCmdClearColorImage)
    W(vkCmdClearDepthStencilImage) W(vkCmdCopyBuffer) W(vkCmdResetQueryPool) W(vkCmdBlitImage) W(vkCmdBeginRenderPass) W(vkEndCommandBuffer)
    W(vkResetCommandBuffer) W(vkDestroyFramebuffer)
#undef W
}

rsx_vk_bridge_dev D;
VkPhysicalDeviceMemoryProperties s_mem;
float s_ts_period = 1.0f;
std::atomic<int> s_ready{0};
// R6: the vertex constants as the records so far set them (Rec::cdelta, or
// a whole copy), applied in record order by this thread
uint32_t s_vc[512][4];
void apply_vconst(const Rec* r)
{
    if (r->vc_full) { memcpy(s_vc, r->vconst, sizeof s_vc); return; }
    const uint32_t* p = r->cdelta.data();
    const uint32_t* e = p + r->cdelta.size();
    while (p < e) {
        const uint32_t start = *p >> 16, count = *p & 0xFFFFu;
        p++;
        if (start + count <= 512 && p + 4 * count <= e) memcpy(&s_vc[start][0], p, 16u * count);
        p += 4 * count;
    }
}
VkSemaphore s_present_sem[2];   // R6: the present's submission waits on one; the next-but-one present reuses it
int s_occ_on = -1;              // R6: DOTHEROES_R6_OCCLUSION
uint32_t s_present_n = 0;
int s_full = 0;   // DOTHEROES_PPFX_BARRIERS=full: R3's full barriers, every image in GENERAL
char s_dump[512];   // DOTHEROES_PPFX_DUMP: where the first differing outputs go
std::atomic<int> s_draining{0};   // render_drain: the engine has stopped (exit)
std::atomic<int> s_stop{0}, s_stopped{0};   // R6: render_drain asks the native thread to end; it has
// R6 (a diagnostic for a request that times out): where the native thread is
// -- 1 recording, 2 idle (waiting for the walker), 3 waiting for an engine
// signal before a submission, 4 serving a readback, 5 presenting -- and the
// next record it takes
std::atomic<int> s_where{0};
std::atomic<uint64_t> s_next{0};

uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void fail(const char* what, VkResult r)
{
    char line[160];
    snprintf(line, sizeof line, "render: %s failed (%d)", what, (int)r);
    report_add(line);
    fprintf(stderr, "[ppfx] %s\n", line);
}

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < s_mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (s_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

// ---- buffers and images ----------------------------------------------------------

struct Buf { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint8_t* p = nullptr; VkDeviceSize n = 0; };

// Host-visible memory; a buffer the CPU reads back (readback) is cached
// memory, which the CPU reads at its own speed -- write-combined memory read
// by the CPU costs hundreds of milliseconds a frame.
bool make_buffer(Buf& o, VkDeviceSize n, VkBufferUsageFlags usage, bool readback = false, bool device = false)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = n; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (F.vkCreateBuffer(D.device, &ci, nullptr, &o.b) != VK_SUCCESS) return false;
    VkMemoryRequirements rq;
    F.vkGetBufferMemoryRequirements(D.device, o.b, &rq);
    uint32_t t = ~0u;
    // the batches' uploads (vertices, constants) in video memory the CPU can
    // write (the default; DOTHEROES_PPFX_UPLOAD=host: system memory, R3's way, 0.45 ms
    // a frame more of GPU time), when the device has such memory
    if (device)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (readback)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t == ~0u)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size; ai.memoryTypeIndex = t;
    if (t == ~0u || F.vkAllocateMemory(D.device, &ai, nullptr, &o.m) != VK_SUCCESS) return false;
    if (device) {   // which memory the uploads got, for the report
        static int said = 0;
        if (!said++) report_add((s_mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                    ? "render: uploads in device-local memory the CPU writes"
                                    : "render: uploads in system memory (no device-local host-visible memory)");
    }
    F.vkBindBufferMemory(D.device, o.b, o.m, 0);
    void* p = nullptr;
    F.vkMapMemory(D.device, o.m, 0, VK_WHOLE_SIZE, 0, &p);
    o.p = (uint8_t*)p; o.n = n;
    return true;
}

struct Img {
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t w = 0, h = 0, levels = 1, bpp = 0;
    VkFramebuffer fb = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkImageView> views;   // (remap, format byte) -> a view with that crossbar
    uint32_t written = 0;                              // records have written it (or a handoff filled it)
    VkDeviceSize bytes = 0;                            // its memory
    // the hazard tracking (the native queue's view of it): its layout, the
    // stage and access of its last write, the stages that read it since
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags wstage = 0, rstages = 0;
    VkAccessFlags waccess = 0;
    bool external = false;                             // a slot: the engine's queue writes it, a semaphore orders it
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;   // R5: a depth image's depth (and stencil) aspects
    uint32_t asp_frame = 0;                            // R5: 1 + the frame asp's records last drew into it
    uint8_t main_dirty = 0;                            // R6: a main-memory target drawn since its last write-back
    uint32_t guest_pitch = 0;                          // R6: its pitch in guest memory
};

VkImageView make_view(VkImage img, VkFormat fmt, uint32_t levels, VkComponentMapping map)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.components = map;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    VkImageView v = VK_NULL_HANDLE;
    F.vkCreateImageView(D.device, &vi, nullptr, &v);
    return v;
}

Img* make_image(VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, uint32_t bpp, bool target)
{
    Img* o = new Img;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = { w, h, 1 }; ci.mipLevels = levels;
    ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               (target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (F.vkCreateImage(D.device, &ci, nullptr, &o->img) != VK_SUCCESS) { delete o; return nullptr; }
    VkMemoryRequirements rq;
    F.vkGetImageMemoryRequirements(D.device, o->img, &rq);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, 0);
    if (F.vkAllocateMemory(D.device, &ai, nullptr, &o->mem) != VK_SUCCESS) {
        F.vkDestroyImage(D.device, o->img, nullptr); delete o; return nullptr;
    }
    F.vkBindImageMemory(D.device, o->img, o->mem, 0);
    o->fmt = fmt; o->w = w; o->h = h; o->levels = levels; o->bpp = bpp;
    o->view = make_view(o->img, fmt, levels, { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY });
    o->bytes = rq.size;
    stats().n_images++;
    stats().n_image_bytes += rq.size;
    return o;
}

// R5: a depth (and stencil) image: an attachment asp's draws test and
// write, and a texture the shadow pass's readers sample (its depth aspect)
bool has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
Img* make_depth_image(VkFormat fmt, uint32_t w, uint32_t h)
{
    Img* o = new Img;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = { w, h, 1 }; ci.mipLevels = 1;
    ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (F.vkCreateImage(D.device, &ci, nullptr, &o->img) != VK_SUCCESS) { delete o; return nullptr; }
    VkMemoryRequirements rq;
    F.vkGetImageMemoryRequirements(D.device, o->img, &rq);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, 0);
    if (F.vkAllocateMemory(D.device, &ai, nullptr, &o->mem) != VK_SUCCESS) {
        F.vkDestroyImage(D.device, o->img, nullptr); delete o; return nullptr;
    }
    F.vkBindImageMemory(D.device, o->img, o->mem, 0);
    o->fmt = fmt; o->w = w; o->h = h; o->levels = 1; o->bpp = 4;
    o->aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    // the attachment's view: both aspects; a sampled view: depth alone (made on use)
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = o->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = { o->aspect, 0, 1, 0, 1 };
    F.vkCreateImageView(D.device, &vi, nullptr, &o->view);
    o->bytes = rq.size;
    stats().n_images++;
    stats().n_image_bytes += rq.size;
    return o;
}

// The depth image's view for sampling (its depth aspect)
VkImageView depth_sample_view(Img* o)
{
    auto it = o->views.find(~0ull);
    if (it != o->views.end()) return it->second;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = o->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = o->fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    VkImageView v = VK_NULL_HANDLE;
    F.vkCreateImageView(D.device, &vi, nullptr, &v);
    return o->views[~0ull] = v;
}

void forget_framebuffers(Img* o);
void destroy_image(Img* o)
{
    if (!o) return;
    forget_framebuffers(o);
    for (auto& kv : o->views) F.vkDestroyImageView(D.device, kv.second, nullptr);
    if (o->fb) F.vkDestroyFramebuffer(D.device, o->fb, nullptr);
    if (o->view) F.vkDestroyImageView(D.device, o->view, nullptr);
    if (o->img) F.vkDestroyImage(D.device, o->img, nullptr);
    if (o->mem) F.vkFreeMemory(D.device, o->mem, nullptr);
    stats().n_images--;
    stats().n_image_bytes -= o->bytes;
    delete o;
}

// ---- synchronisation ---------------------------------------------------------------

// R3's way (DOTHEROES_PPFX_BARRIERS=full): a full barrier between operations
// (the engine's vk_cmd_after_everything).
void barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    F.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    stats().n_barriers++;
    stats().n_barriers_full++;
}

// One access to an image, with the barrier its hazard needs: read after
// write, write after read or write, a layout change (R2's r2_access). In
// full mode: the full barrier, and every image stays in GENERAL.
enum Use { kAttach, kSampled, kCopySrc, kCopyDst, kDepthAttach };
void access(VkCommandBuffer cb, Img* o, Use use)
{
    if (s_full) {
        if (o->layout != VK_IMAGE_LAYOUT_GENERAL) {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = o->img; b.subresourceRange = { o->aspect, 0, o->levels, 0, 1 };
            F.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &b);
            o->layout = VK_IMAGE_LAYOUT_GENERAL;
            stats().n_barriers++;
        }
        barrier_all(cb);
        return;
    }
    VkImageLayout want; VkPipelineStageFlags stage; VkAccessFlags acc; bool write;
    switch (use) {
    case kAttach: want = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                  acc = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; write = true; break;
    case kSampled: want = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                   acc = VK_ACCESS_SHADER_READ_BIT; write = false; break;
    case kDepthAttach: want = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                   stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                   acc = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; write = true; break;
    case kCopySrc: want = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                   acc = VK_ACCESS_TRANSFER_READ_BIT; write = false; break;
    default: want = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
             acc = VK_ACCESS_TRANSFER_WRITE_BIT; write = true; break;
    }
    // a slot stays in GENERAL, as the engine's copy left it; the semaphore
    // has ordered the native queue after that copy
    if (o->external) want = VK_IMAGE_LAYOUT_GENERAL;
    VkPipelineStageFlags src = 0;
    VkAccessFlags srca = 0;
    bool need = false;
    if (o->layout != want) {                 // a transition: after every earlier access
        src |= o->wstage | o->rstages; srca |= o->waccess; need = true;
    } else if (write) {                      // WAW, WAR
        if (o->wstage || o->rstages) { src |= o->wstage | o->rstages; srca |= o->waccess; need = true; }
    } else if (o->wstage && !(o->rstages & stage)) {   // RAW, once per reading stage
        src |= o->wstage; srca |= o->waccess; need = true;
    }
    if (need) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = srca; b.dstAccessMask = acc;
        b.oldLayout = o->layout; b.newLayout = want;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = o->img; b.subresourceRange = { o->aspect, 0, o->levels, 0, 1 };
        F.vkCmdPipelineBarrier(cb, src ? src : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stage, 0, 0, nullptr, 0, nullptr, 1, &b);
        stats().n_barriers++;
    }
    if (write) { o->wstage = stage; o->waccess = acc; o->rstages = 0; }
    else if (need && o->layout != want) o->rstages = stage;
    else o->rstages |= stage;
    o->layout = want;
}

// The layout an image is sampled in.
VkImageLayout sampled_layout() { return s_full ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
VkImageLayout layout_of(const Img* o) { return o->layout; }

// ---- the bridge's slots ----------------------------------------------------------------
// Staging images (handoffs), buffers (the engine's outputs for comparison)
// and binary semaphores. The walker takes one (slot_*), the record names it,
// and the batch that used it gives it back once complete.

struct SlotImg { Img* im; bool busy; };
struct SlotBuf { Buf b; bool busy; int prio; };
struct SlotSem { VkSemaphore s; bool busy; };
std::mutex s_slot_lock;
std::vector<SlotImg> s_simg;
std::vector<SlotBuf> s_sbuf;
std::vector<SlotSem> s_ssem;
const size_t kMaxSlotImages = 128, kMaxSlotBuffers = 512, kMaxSlotSems = 64;
// Buffers kept back for the comparisons that must run: final composites (one
// a frame) and the frames after a structure change (an area's first frames).
const size_t kFinalReserve = 32, kChangeReserve = 160;
const VkDeviceSize kSlotBufferBytes = 768ull << 20;   // the engine-side reference buffers, at most
const VkDeviceSize kReadbackPoolBytes = 256ull << 20; // free native readback buffers kept for reuse

// ---- batches --------------------------------------------------------------------------

// What a stretch of a batch's GPU time went to: the time from one timestamp
// to the next goes to the operation recorded between them (its barriers
// included).
enum Sect : uint8_t { kSectDraw = 0, kSectCopy = 1, kSectReadback = 2, kSectOther = 3 };
const uint32_t kQueries = 2048;

struct Batch {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkQueryPool qp = VK_NULL_HANDLE;
    Buf up;
    VkDeviceSize used = 0;
    bool open = false, pending = false;
    uint32_t frame = 0, records = 0, sets = 0, nq = 0;
    std::vector<uint8_t> sect;                   // per timestamp after the first: its section
    std::vector<int> sems;                       // semaphore slots to wait on ...
    std::vector<long long> ords;                 // ... and their signals' ordinals
    std::vector<VkSemaphore> signals;            // R4: handbacks' semaphores this batch signals
    std::vector<int> release;                    // R4: handbacks' semaphore slots, free once this batch is done
    std::vector<std::function<void()>> done;     // after completion
    // R6: occlusion queries (DOTHEROES_R6_OCCLUSION=1): one a counted draw,
    // each with its global number; and the batch's place in submission order
    VkQueryPool occ = VK_NULL_HANDLE;
    uint32_t nocc = 0;
    std::vector<uint64_t> occ_gid;
    uint64_t seq = 0;
};
const int kBatches = 4;
const uint32_t kOcc = 1024;
uint64_t s_batch_seq = 0;   // R6: submissions so far (the batches retire in this order)
const VkDeviceSize kUpload = 16u << 20;
const uint32_t kSets = 1024;
Batch s_b[kBatches];
int s_cur = 0;

VkCommandPool s_pool;
VkDescriptorSetLayout s_dsl;
VkPipelineLayout s_pl;
Img* s_dummy;
VkSampler s_dummy_smp;

// A timestamp: the GPU time since the one before goes to `s`.
void mark(Batch& b, Sect s)
{
    if (b.nq >= kQueries) return;
    F.vkCmdWriteTimestamp(b.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b.qp, b.nq++);
    b.sect.push_back(s);
}

uint64_t s_occ_val[1u << 16];   // R6: occlusion results by global number (mod 2^16), read at completion
bool s_occ_open = false;        // R6: a query begun for the draw being recorded
void retire(Batch& b)
{
    if (!b.pending) return;
    // R6: in submission order -- a later batch's completion work may use what
    // an earlier batch's completion reads (occlusion counts)
    for (Batch& o : s_b) if (&o != &b && o.pending && o.seq < b.seq) retire(o);
    F.vkWaitForFences(D.device, 1, &b.fence, VK_TRUE, UINT64_MAX);
    if (b.nocc) {
        std::vector<uint64_t> v(b.nocc);
        if (F.vkGetQueryPoolResults(D.device, b.occ, 0, b.nocc, v.size() * 8, v.data(), 8,
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            for (uint32_t i = 0; i < b.nocc; i++) s_occ_val[b.occ_gid[i] & 0xFFFFu] = v[i];
        b.nocc = 0;
        b.occ_gid.clear();
    }
    if (b.qp && b.nq >= 2) {
        std::vector<uint64_t> ts(b.nq);
        if (F.vkGetQueryPoolResults(D.device, b.qp, 0, b.nq, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            Stats& st = stats();
            st.gpu_ns += (uint64_t)((double)(ts[b.nq - 1] - ts[0]) * s_ts_period);
            for (uint32_t i = 1; i < b.nq; i++) {
                const uint64_t d = (uint64_t)((double)(ts[i] - ts[i - 1]) * s_ts_period);
                switch (b.sect[i - 1]) {
                case kSectDraw: st.gpu_draw_ns += d; break;
                case kSectCopy: st.gpu_copy_ns += d; break;
                case kSectReadback: st.gpu_readback_ns += d; break;
                default: st.gpu_other_ns += d; break;
                }
            }
        }
    }
    for (auto& f : b.done) f();
    b.done.clear();
    {
        std::lock_guard<std::mutex> g(s_slot_lock);
        for (int s : b.sems) if (s >= 0 && (size_t)s < s_ssem.size()) s_ssem[s].busy = false;
        for (int s : b.release) if (s >= 0 && (size_t)s < s_ssem.size()) s_ssem[s].busy = false;
    }
    b.sems.clear();
    b.ords.clear();
    b.release.clear();
    b.pending = false;
}

bool retire_ready(Batch& b) { return b.pending && F.vkGetFenceStatus(D.device, b.fence) == VK_SUCCESS; }

Batch& batch(uint32_t frame)
{
    Batch& b = s_b[s_cur];
    if (b.open) return b;
    const int was = s_where.exchange(6);   // 6: waiting for a batch slot's previous use to complete
    retire(b);
    s_where = was;
    F.vkResetFences(D.device, 1, &b.fence);
    F.vkResetDescriptorPool(D.device, b.dp, 0);
    F.vkResetCommandBuffer(b.cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    F.vkBeginCommandBuffer(b.cb, &bi);
    F.vkCmdResetQueryPool(b.cb, b.qp, 0, kQueries);
    if (b.occ) F.vkCmdResetQueryPool(b.cb, b.occ, 0, kOcc);
    b.nocc = 0; b.occ_gid.clear(); b.seq = ++s_batch_seq;
    b.nq = 0; b.sect.clear();
    F.vkCmdWriteTimestamp(b.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, b.qp, b.nq++);
    b.used = 0; b.open = true; b.frame = frame; b.records = 0; b.sets = 0;
    return b;
}

// R6 (docs/NATIVE_R6.md, synchronisation): a batch whose engine signal
// never came is not submitted -- its commands read what the engine's copies
// would have written, and waiting on a semaphore whose signal was never
// submitted is invalid (VUID-vkQueueSubmit-pWaitSemaphores-03238; R4/R5 did
// that at exit after 100 ms). It is cancelled instead: recorded but never
// run, its completion work done once every batch before it has completed,
// its slots given back. The semaphores it would have signalled stay
// unsignalled: only an engine submission made after its last one could wait
// on them, and at exit there is none.
uint64_t s_cancelled = 0;
void cancel(Batch& b)
{
    F.vkEndCommandBuffer(b.cb);
    for (Batch& o : s_b) if (&o != &b) retire(o);   // the GPU done with what the cancelled batch's completion frees
    for (auto& f : b.done) f();
    b.done.clear();
    {
        std::lock_guard<std::mutex> g(s_slot_lock);
        for (int k : b.sems) if (k >= 0 && (size_t)k < s_ssem.size()) s_ssem[k].busy = false;
        for (int k : b.release) if (k >= 0 && (size_t)k < s_ssem.size()) s_ssem[k].busy = false;
    }
    b.sems.clear(); b.ords.clear(); b.release.clear(); b.signals.clear();
    b.open = false; b.pending = false;
    s_cancelled++;
    s_cur = (s_cur + 1) % kBatches;
}

void submit()
{
    Batch& b = s_b[s_cur];
    if (!b.open) return;
    // the engine's submissions that signal what we wait on must be made
    // before ours waits (the engine submits at its own points: a frame at most)
    if (!b.ords.empty()) {
        const uint64_t t0 = now_ns();
        bool missing = false;
        uint64_t say = 10000000000ull;
        const int was = s_where.exchange(3);
        for (long long o : b.ords)
            while (!rsx_vk_bridge_submitted(o)) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                // at exit the engine may have stopped before its next
                // submission: after a short wait the batch is cancelled (counted)
                if (s_draining && now_ns() - t0 > 100000000ull) { stats().exit_signal_skips++; missing = true; break; }
                // in play: a bug (nothing the engine does any more waits a
                // frame to submit); said, and the wait goes on -- never a
                // submission that waits on an unsubmitted signal
                if (now_ns() - t0 > say) { fail("waiting for the engine's signal (still waiting)", VK_TIMEOUT); say += 10000000000ull; }
            }
        stats().wait_signal_us += (now_ns() - t0) / 1000;
        s_where = was;
        if (missing) { cancel(b); return; }
    }
    // the readback copies visible to the host for the comparisons
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    F.vkCmdPipelineBarrier(b.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    mark(b, kSectOther);
    F.vkEndCommandBuffer(b.cb);
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags> stages;
    {
        std::lock_guard<std::mutex> g(s_slot_lock);
        for (int s : b.sems) { waits.push_back(s_ssem[s].s); stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); }
    }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = (uint32_t)waits.size(); si.pWaitSemaphores = waits.data();
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1; si.pCommandBuffers = &b.cb;
    si.signalSemaphoreCount = (uint32_t)b.signals.size();
    si.pSignalSemaphores = b.signals.empty() ? nullptr : b.signals.data();
    rsx_vk_bridge_queue_lock();
    const VkResult r = F.vkQueueSubmit(D.queue, 1, &si, b.fence);
    rsx_vk_bridge_queue_unlock();
    b.signals.clear();
    b.open = false;
    if (r != VK_SUCCESS) {
        // nothing was submitted: its fence will never signal (R6: it was left
        // pending, and retiring it waited forever)
        fail("vkQueueSubmit", r);
        for (auto& f : b.done) f();   // a lost device: nothing runs any more
        b.done.clear(); b.sems.clear(); b.ords.clear(); b.release.clear();
        s_cur = (s_cur + 1) % kBatches;
        return;
    }
    b.pending = true;
    stats().n_batches++;
    s_cur = (s_cur + 1) % kBatches;
}

// Upload bytes into the batch's buffer; the offset, or ~0 when full.
VkDeviceSize upload(Batch& b, const void* p, VkDeviceSize n, VkDeviceSize align = 256)
{
    VkDeviceSize at = (b.used + align - 1) & ~(align - 1);
    if (at + n > b.up.n) return ~(VkDeviceSize)0;
    memcpy(b.up.p + at, p, (size_t)n);
    b.used = at + n;
    return at;
}

// ---- formats ----------------------------------------------------------------------------

VkFormat be_vkformat(rsx_be_format f, uint32_t* bpp)
{
    switch (f) {   // the backend's vk_eng_format
    case RSX_BE_FMT_R8: *bpp = 1; return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8: *bpp = 2; return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8: *bpp = 4; return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16: *bpp = 2; return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16: *bpp = 4; return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F: *bpp = 4; return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F: *bpp = 4; return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1: *bpp = 8; return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;   // per block (R5: asp's DXT textures)
    case RSX_BE_FMT_BC2: *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3: *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default: *bpp = 0; return VK_FORMAT_UNDEFINED;
    }
}

rsx_be_format texfmt_be(rsx_texfmt f)
{
    switch (f) {   // the engine's eng_texfmt
    case RSX_TEXFMT_R8: return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8: return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8: return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_R16: return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16: return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F: return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F: return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F: return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F: return RSX_BE_FMT_R32G32B32A32F;
    case RSX_TEXFMT_BC1: return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2: return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3: return RSX_BE_FMT_BC3;
    default: return RSX_BE_FMT_NONE;
    }
}

VkComponentSwizzle remap_sel(uint8_t s)
{
    switch (s) {   // the backend's vk_remap_sel
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}

// A texture's view swizzle: its CellGcmTexture.remap crossbar, as the asset
// helper reads it for the texture's format.
VkComponentMapping remap(uint32_t crossbar, uint32_t fmt)
{
    uint8_t sel[4];
    rsx_texture_component_remap(crossbar, fmt & 0x9Fu, sel);
    return { remap_sel(sel[1]), remap_sel(sel[2]), remap_sel(sel[3]), remap_sel(sel[0]) };
}

VkSamplerAddressMode gcm_wrap(uint32_t w)
{
    switch (w & 0xFu) {   // CELL_GCM_TEXTURE_WRAP, _MIRROR, _CLAMP_TO_EDGE ... (the backend's vk_gcm_wrap)
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

std::map<uint64_t, VkSampler> s_samplers;

// A sampler from cellGcmSetTextureFilter's filters and cellGcmSetTextureControl's
// levels of detail and cellGcmSetTextureAddress's wrap modes (the engine's
// eng_decode_sampler and the backend's vk_eng_sampler).
VkSampler sampler(const Sampler& s)
{
    const uint32_t minf = s.min_filter, magf = s.mag_filter;
    const int min_linear = minf == 2 || minf == 4 || minf == 6, mag_linear = magf == 2;
    const int mip_present = minf >= 3, mip_linear = minf == 5 || minf == 6;
    float min_lod = (float)s.min_lod / 256.0f;
    float max_lod = mip_present ? (float)s.max_lod / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    if (!mip_present) max_lod = min_lod;
    uint32_t lo, hi;
    memcpy(&lo, &min_lod, 4); memcpy(&hi, &max_lod, 4);
    const uint64_t key = (uint64_t)min_linear | (uint64_t)mag_linear << 1 | (uint64_t)mip_linear << 2 |
                         (uint64_t)mip_present << 3 | (uint64_t)(s.wrap_s & 0xF) << 4 | (uint64_t)(s.wrap_t & 0xF) << 8 |
                         (uint64_t)(s.wrap_r & 0xF) << 12 | (uint64_t)(lo >> 16) << 16 | (uint64_t)(hi >> 16) << 32;
    auto it = s_samplers.find(key);
    if (it != s_samplers.end()) return it->second;
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = gcm_wrap(s.wrap_s); ci.addressModeV = gcm_wrap(s.wrap_t); ci.addressModeW = gcm_wrap(s.wrap_r);
    ci.minLod = min_lod; ci.maxLod = max_lod;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler v = VK_NULL_HANDLE;
    F.vkCreateSampler(D.device, &ci, nullptr, &v);
    return s_samplers[key] = v;
}

// The translators' per-unit texture parameters, from the record's texture
// and sampler (what rsx_texture_gamma_mask, rsx_texture_expand_mask and
// rsx_fp_shadow_desc_from_texture compute, on the explicit fields).
uint8_t gamma_mask(const Texture& t, const Sampler& s)
{
    switch (t.format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    uint8_t gamma = s.gamma & 15u;
    if (!gamma) return 0;
    uint32_t crossbar = t.remap & 0xFFFFu;
    if (!crossbar) crossbar = 0xAAE4u;
    uint8_t sel[4]; rsx_texture_component_remap(crossbar, t.format, sel);
    static const uint8_t rgba_lane[4] = { 3, 0, 1, 2 };
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t source = (crossbar >> (2u * i)) & 3u;
        if (sel[i] < RSX_REMAP_ZERO && (s.filter_signed >> source & 1u)) gamma &= (uint8_t)~(1u << rgba_lane[i]);
    }
    return gamma;
}

uint8_t expand_mask(const Texture& t, const Sampler& s)
{
    switch (t.format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x98:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    if (!(s.unsigned_remap & 1u)) return 0;   // CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL
    uint32_t crossbar = t.remap & 0xFFFFu;
    if (!crossbar) crossbar = 0xAAE4u;
    uint8_t sel[4]; rsx_texture_component_remap(crossbar, t.format, sel);
    static const uint8_t rgba_lane[4] = { 3, 0, 1, 2 };
    uint8_t mask = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t source = (crossbar >> (2u * i)) & 3u;
        if (sel[i] < RSX_REMAP_ZERO && !(s.filter_signed >> source & 1u)) mask |= (uint8_t)(1u << rgba_lane[i]);
    }
    if (mask && ((t.format & 0x9Fu) == 0x94u || (t.format & 0x9Fu) == 0x95u)) mask |= 0x10u;
    return mask;
}

void shadow_desc(rsx_fp_shadow_desc* out, const Texture& t, const Sampler& s)
{
    memset(out, 0, sizeof *out);
    const uint32_t base = t.format & 0x9Fu, func = s.zfunc & 15u;
    if (!s.enable || (base != 0x90 && base != 0x91 && base != 0x92 && base != 0x93) || func == 0 || func == 7) return;
    const uint32_t minf = s.min_filter, magf = s.mag_filter;
    const int minlin = minf == 2 || minf == 4 || minf == 6;
    const int maglin = magf == 2;
    out->enabled = (base == 0x91 || base == 0x93 || func > 7 || t.dimension != 2 || t.cubemap || t.mipmap > 1 ||
                    minf < 1 || minf > 6 || magf < 1 || magf > 2 || minlin != maglin) ? 2 : 1;
    out->func = (uint8_t)func;
    out->linear = (uint8_t)minlin;
    out->unnormalized = (uint8_t)((t.format & 0x40u) != 0);
}

std::map<VkFormat, VkRenderPass> s_rps;

VkRenderPass render_pass(VkFormat f)
{
    auto it = s_rps.find(f);
    if (it != s_rps.end()) return it->second;
    const VkImageLayout l = s_full ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription a = {};
    a.format = f; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = l; a.finalLayout = l;
    VkAttachmentReference ar = { 0, l };
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1; ci.pAttachments = &a; ci.subpassCount = 1; ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    F.vkCreateRenderPass(D.device, &ci, nullptr, &rp);
    return s_rps[f] = rp;
}

VkFramebuffer framebuffer(Img* o)
{
    if (o->fb) return o->fb;
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = render_pass(o->fmt); ci.attachmentCount = 1; ci.pAttachments = &o->view;
    ci.width = o->w; ci.height = o->h; ci.layers = 1;
    F.vkCreateFramebuffer(D.device, &ci, nullptr, &o->fb);
    return o->fb;
}

// R5: a pass with a colour attachment, a depth (and stencil) attachment, or
// both -- asp's draws; both loaded and stored, as the guest's surfaces are
std::map<std::pair<VkFormat, VkFormat>, VkRenderPass> s_rps2;
VkRenderPass render_pass2(VkFormat c, VkFormat d)
{
    auto it = s_rps2.find({ c, d });
    if (it != s_rps2.end()) return it->second;
    VkAttachmentDescription a[2] = {};
    VkAttachmentReference cr = {}, dr = {};
    uint32_t n = 0;
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (c != VK_FORMAT_UNDEFINED) {
        const VkImageLayout l = s_full ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a[n].format = c; a[n].samples = VK_SAMPLE_COUNT_1_BIT;
        a[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[n].initialLayout = l; a[n].finalLayout = l;
        cr = { n, l };
        sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr;
        n++;
    }
    if (d != VK_FORMAT_UNDEFINED) {
        const VkImageLayout l = s_full ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a[n].format = d; a[n].samples = VK_SAMPLE_COUNT_1_BIT;
        a[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[n].stencilLoadOp = has_stencil(d) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[n].stencilStoreOp = has_stencil(d) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[n].initialLayout = l; a[n].finalLayout = l;
        dr = { n, l };
        sp.pDepthStencilAttachment = &dr;
        n++;
    }
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = n; ci.pAttachments = a; ci.subpassCount = 1; ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    F.vkCreateRenderPass(D.device, &ci, nullptr, &rp);
    return s_rps2[{ c, d }] = rp;
}

// The framebuffer of a colour image and a depth image (either may be null),
// kept until either is destroyed (with the colour image's, or the depth's
// when there is no colour)
std::map<std::pair<Img*, Img*>, VkFramebuffer> s_fbs2;
VkFramebuffer framebuffer2(Img* c, Img* d)
{
    auto it = s_fbs2.find({ c, d });
    if (it != s_fbs2.end()) return it->second;
    VkImageView v[2]; uint32_t n = 0;
    if (c) v[n++] = c->view;
    if (d) v[n++] = d->view;
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = render_pass2(c ? c->fmt : VK_FORMAT_UNDEFINED, d ? d->fmt : VK_FORMAT_UNDEFINED);
    ci.attachmentCount = n; ci.pAttachments = v;
    ci.width = c ? c->w : d->w; ci.height = c ? c->h : d->h; ci.layers = 1;
    if (c && d) { ci.width = c->w < d->w ? c->w : d->w; ci.height = c->h < d->h ? c->h : d->h; }
    VkFramebuffer fb = VK_NULL_HANDLE;
    F.vkCreateFramebuffer(D.device, &ci, nullptr, &fb);
    return s_fbs2[{ c, d }] = fb;
}
// An image going away: its framebuffers with it
void forget_framebuffers(Img* o)
{
    for (auto it = s_fbs2.begin(); it != s_fbs2.end();)
        if (it->first.first == o || it->first.second == o) { F.vkDestroyFramebuffer(D.device, it->second, nullptr); it = s_fbs2.erase(it); }
        else ++it;
}

// ---- the registries -------------------------------------------------------------------------

std::unordered_map<uint64_t, Img*> s_targets;     // a surface address PPFX renders to, or other work's surface handed over
struct StaticTex { Img* img; uint32_t frame; };
std::unordered_map<uint64_t, StaticTex> s_static;  // static textures by content, with the frame of their last use
std::map<std::tuple<VkFormat, uint32_t, uint32_t>, Img*> s_snap;   // own-target snapshots
std::vector<Img*> s_retired;                      // destroyed once the batches in flight are done

uint64_t akey(uint32_t loc, uint32_t off) { return (uint64_t)loc << 32 | off; }

Img* target_image(uint32_t loc, uint32_t off, VkFormat fmt, uint32_t bpp, uint32_t w, uint32_t h)
{
    Img*& t = s_targets[akey(loc, off)];
    if (t && t->fmt == fmt && t->w >= w && t->h >= h) return t;
    // new, or other than this record needs: a new image (resource reuse); the
    // old one goes once nothing in flight can use it
    if (t) { s_retired.push_back(t); stats().n_retired++; }
    t = make_image(fmt, w, h, 1, bpp, true);
    return t;
}

// ---- programs and pipelines -----------------------------------------------------------------

struct Pipe { VkPipeline p; uint32_t nconst; };
std::unordered_map<uint64_t, Pipe> s_pipes;

// The engine's eng_programs_text, step for step, under the translators' lock.
struct ProgParams {
    const uint8_t* vp; uint32_t vp_bytes; const uint8_t* fp; uint32_t fp_size;
    uint32_t fp_ctrl, cube_mask, vol_mask, vtex_mask, layout_mask, vp_branch_bits, ucp, srgb_output, alpha_test,
             alpha_func, unnorm_mask, shadow_mask, gamma_units, expand_units;
    uint32_t wpos[2]; int wpos_scaled;
    const rsx_fp_shadow_desc* shadow; const uint8_t* gamma_masks; const uint8_t* expand_masks;
    const uint32_t (*unnorm_dim)[3];
};

bool programs_text(const ProgParams& p, std::string& vs_out, std::string& ps_out, uint32_t* nconst)
{
    static char vs[16384], ps[65536];
    rsx_shader_text_lock();
    rsx_vp_set_branch_bits(p.vp_branch_bits);
    int vi = rsx_vp_decompile_compact_ex(p.vp, p.vp_bytes, p.vtex_mask, p.layout_mask, vs, sizeof vs);
    if (vi > 0 && p.ucp && rsx_vp_apply_user_clip(vs, sizeof vs, p.ucp) < 0) vi = -1;
    int fi = rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(p.fp, p.fp_size, p.fp_ctrl, p.cube_mask, p.vol_mask,
            p.shadow_mask ? p.shadow : nullptr, p.gamma_masks, p.expand_units ? p.expand_masks : nullptr, ps, sizeof ps,
            nconst);
    if (fi > 0 && p.wpos_scaled && rsx_fp_apply_wpos_grid(ps, sizeof ps, p.wpos[0], p.wpos[1]) < 0) fi = -1;
    if (fi > 0 && rsx_fp_apply_unnorm_scale(ps, sizeof ps, p.unnorm_mask, p.unnorm_dim, p.cube_mask, p.vol_mask) < 0)
        fi = -1;
    if (fi > 0 && p.srgb_output && rsx_fp_apply_srgb_output(ps, sizeof ps) < 0) fi = -1;
    if (fi > 0 && p.alpha_test && rsx_fp_apply_alpha_test_buffered(ps, sizeof ps, p.alpha_func) < 0) fi = -1;
    if (vi > 0 && fi > 0) { vs_out = vs; ps_out = ps; }
    rsx_shader_text_unlock();
    return vi > 0 && fi > 0;
}

VkShaderModule spirv_module(const std::string& hlsl, int stage)
{
    static std::vector<uint32_t> words(262144);
    uint32_t n = 0;
    char log[2048];
    if (rsx_hlsl_to_spirv(hlsl.c_str(), stage, words.data(), (uint32_t)words.size(), &n, log, sizeof log) || !n) {
        report_add((std::string("render: SPIR-V translation failed: ") + log).c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = n * 4u; ci.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    F.vkCreateShaderModule(D.device, &ci, nullptr, &m);
    return m;
}

// The fragment translator's control word, made from the program's
// CgBinaryFragmentProgram fields (registerCount, outputFromH0, depthReplace,
// pixelKill) and the bind's txp: the translator's parameter, as libgcm's
// cellGcmSetFragmentProgram makes it.
uint32_t fp_control(const FragmentProgram& f)
{
    return (uint32_t)(f.reg_count < 2 ? 2 : f.reg_count) << 24 | (uint32_t)f.txp << 15 | 0x400u |
           (f.output_from_h0 ? 0 : 0x40u) | (f.depth_replace ? 0xEu : 0) | (f.pixel_kill ? 0x80u : 0);
}

// The colour write mask, from cellGcmSetColorMask's (or asp's) CELL_GCM_COLOR_MASK bits.
VkColorComponentFlags write_mask(uint32_t m)
{
    VkColorComponentFlags wm = 0;
    if (m >> 16 & 0xFF) wm |= VK_COLOR_COMPONENT_R_BIT;
    if (m >> 8 & 0xFF) wm |= VK_COLOR_COMPONENT_G_BIT;
    if (m & 0xFF) wm |= VK_COLOR_COMPONENT_B_BIT;
    if (m >> 24 & 0xFF) wm |= VK_COLOR_COMPONENT_A_BIT;
    return wm;
}

// The pipeline for this record: its programs as the engine makes them, with
// the record's texture, sampler and surface parameters, and its render state.
// 0 when it cannot be made.
const Pipe* pipeline(const Rec* r, const rsx_vertex_layout_plan& layout, VkFormat rt, rsx_fp_constant_block* consts)
{
    const State& st = r->st;
    const uint8_t* vp = r->vp->bytes.data();
    const uint32_t vp_bytes = (uint32_t)r->vp->bytes.size();
    const uint8_t* fp = r->fp->bytes.data();
    const uint32_t fp_size = (uint32_t)r->fp->bytes.size();
    const uint32_t fp_ctrl = fp_control(st.fp);
    const uint32_t srgb = rsx_fp_srgb_output_enabled(st.fp.gamma, fp_ctrl, st.surface.color_format & 0x1Fu);
    const uint32_t sampled = rsx_fp_texture_units(fp, fp_size);
    uint32_t cube = 0, vol = 0, shadow_mask = 0, unnorm_mask = 0;
    rsx_fp_shadow_desc shadow[16] = {};
    uint8_t gamma[16] = {}, expand[16] = {};
    uint32_t unnorm[16][3] = {};
    for (uint32_t u = 0; u < 16; u++) {
        const Texture& t = st.tex[u];
        const Sampler& s = st.samp[u];
        if (!s.enable) continue;
        if (t.cubemap && t.width && t.width == t.height) cube |= 1u << u;
        if (!t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height) vol |= 1u << u;
        gamma[u] = gamma_mask(t, s);
        expand[u] = expand_mask(t, s);
        if (sampled >> u & 1) {
            shadow_desc(&shadow[u], t, s);
            if (shadow[u].enabled) shadow_mask |= 1u << u;
        }
        if ((t.format & RSX_TEX_FMT_UNNORM) && t.width && t.height) {
            unnorm_mask |= 1u << u;
            unnorm[u][0] = t.width; unnorm[u][1] = t.height; unnorm[u][2] = t.depth;
        }
    }
    const uint32_t gamma_units = rsx_fp_texture_gamma_units(fp, fp_size, gamma);
    for (uint32_t u = 0; u < 16; u++) if (!(gamma_units >> u & 1)) gamma[u] = 0;
    const uint32_t expand_units = rsx_fp_texture_expand_units(fp, fp_size, expand);
    for (uint32_t u = 0; u < 16; u++) if (!(expand_units >> u & 1)) expand[u] = 0;
    unnorm_mask &= sampled & ~shadow_mask;
    for (uint32_t u = 0; u < 16; u++) if (!(unnorm_mask >> u & 1)) unnorm[u][0] = unnorm[u][1] = unnorm[u][2] = 0;
    consts->count = 0;
    if (rsx_fp_collect_constants(fp, fp_size, consts) < 0) return nullptr;
    // vertex textures, branch bits, user clip planes: never set by PPFX (the
    // explicit state has none); a single-sample surface: no position grid
    const uint32_t vtex = 0, bb = 0, ucp = 0;
    const uint32_t wpos[2] = { 1u, 1u };
    const Raster& ra = st.raster;
    // identity: everything the translation and the pipeline are made from
    uint64_t k = fnv(vp, vp_bytes);
    k = rsx_fp_structural_hash(fp, fp_size, k);
    const uint32_t ints[] = { fp_ctrl & 0x4Eu, srgb, cube, vol, shadow_mask, unnorm_mask, gamma_units, expand_units,
                              layout.mask, layout.stride, (uint32_t)rt, ra.color_mask, ra.front_face };
    k = fnv(ints, sizeof ints, k);
    k = fnv(gamma, sizeof gamma, k); k = fnv(expand, sizeof expand, k);
    k = fnv(unnorm, sizeof unnorm, k); k = fnv(shadow, sizeof shadow, k);
    auto it = s_pipes.find(k);
    if (it != s_pipes.end()) return it->second.p ? &it->second : nullptr;
    Pipe& P = s_pipes[k];
    P.p = VK_NULL_HANDLE;
    ProgParams pp = { vp, vp_bytes, fp, fp_size, fp_ctrl, cube, vol, vtex, layout.mask, bb, ucp, srgb,
                      0u, 0u, unnorm_mask, shadow_mask, gamma_units, expand_units,
                      { wpos[0], wpos[1] }, 0, shadow, gamma, expand, (const uint32_t (*)[3])unnorm };
    std::string vs, ps;
    uint32_t nconst = 0;
    if (!programs_text(pp, vs, ps, &nconst) || nconst != consts->count) {
        report_add("render: a record's programs did not translate");
        return nullptr;
    }
    VkShaderModule vsm = spirv_module(vs, RSX_SHADER_STAGE_VERTEX), fsm = spirv_module(ps, RSX_SHADER_STAGE_FRAGMENT);
    if (!vsm || !fsm) return nullptr;
    VkPipelineShaderStageCreateInfo stg[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main", nullptr } };
    VkVertexInputBindingDescription vb = { 0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[16];
    for (uint32_t s = 0; s < layout.count; s++) va[s] = { s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = layout.count; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo ras = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    ras.polygonMode = VK_POLYGON_MODE_FILL;
    ras.cullMode = VK_CULL_MODE_NONE;   // culling off (checked by the caller)
    ras.frontFace = ra.front_face == 0x0901u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    ras.depthBiasEnable = VK_TRUE; ras.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE; cba.colorWriteMask = write_mask(ra.color_mask);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    const VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 5; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gci.stageCount = 2; gci.pStages = stg; gci.pVertexInputState = &vi; gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vps; gci.pRasterizationState = &ras; gci.pMultisampleState = &ms; gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy; gci.layout = s_pl; gci.renderPass = render_pass(rt);
    const VkResult res = F.vkCreateGraphicsPipelines(D.device, VK_NULL_HANDLE, 1, &gci, nullptr, &P.p);
    F.vkDestroyShaderModule(D.device, vsm, nullptr);
    F.vkDestroyShaderModule(D.device, fsm, nullptr);
    if (res != VK_SUCCESS) { fail("vkCreateGraphicsPipelines", res); P.p = VK_NULL_HANDLE; return nullptr; }
    P.nconst = nconst;
    stats().n_pipelines++;
    return &P;
}

// ---- R5: asp's pipelines ---------------------------------------------------------------------
// libgcm's constants (CELL_GCM_*, as the record keeps them) to Vulkan's
VkCompareOp gcm_compare(uint32_t f) { return (VkCompareOp)((f - 0x0200u) & 7u); }   // NEVER..ALWAYS in the same order
VkBlendFactor gcm_blend_factor(uint32_t f)
{
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp gcm_blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
VkStencilOp gcm_stencil_op(uint32_t o)
{
    switch (o) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

// DOTHEROES_ASP_DESCRIBE=<seq> (a diagnostic): the record's translated
// programs, its fragment constants, vertex inputs and sampled textures,
// into the dump directory (only when its pipeline is made: a fresh one)
void asp_describe(const Rec* r, const std::string& vs, const std::string& ps, const rsx_vertex_layout_plan& layout,
                  const rsx_fp_constant_block* fc)
{
    static long long want = -2;
    if (want == -2) { const char* e = getenv("DOTHEROES_ASP_DESCRIBE"); want = e && *e ? atoll(e) : -1; }
    if (want < 0 || (long long)r->seq < want || !s_dump[0]) return;
    want = -1;   // the first fresh pipeline at or after it
    char path[600];
    snprintf(path, sizeof path, "%s/describe_r%llu.txt", s_dump, (unsigned long long)r->seq);
    FILE* f = fopen(path, "w");
    if (!f) return;
    const State& st = r->st;
    fprintf(f, "record %llu frame %u prim %u count %u vcount %u idx %08X type %u\n", (unsigned long long)r->seq, r->frame,
            r->prim, r->count, r->vcount, r->idx_ea, r->idx_type);
    fprintf(f, "fp offset %08X ctl reg %u h0 %u txp %u kill %u input %08X tex %04X 2d %04X\n", st.fp.offset, st.fp.reg_count,
            st.fp.output_from_h0, st.fp.txp, st.fp.pixel_kill, st.fp.input_mask, st.fp.tex_mask, st.fp.tex_2d);
    for (uint32_t i = 0; i < 16; i++)
        if (st.vin[i].size)
            fprintf(f, "vin%u size %u type %u stride %u loc %u off %08X freq %u\n", i, st.vin[i].size, st.vin[i].type,
                    st.vin[i].stride, st.vin[i].location, st.vin[i].offset, st.vin[i].frequency);
    fprintf(f, "layout mask %04X stride %u count %u\n", layout.mask, layout.stride, layout.count);
    for (uint32_t u = 0; u < 16; u++)
        if (st.samp[u].enable)
            fprintf(f, "tex%u fmt %02X %ux%u mips %u remap %08X loc %u off %08X | min %u mag %u wrap %u/%u\n", u, st.tex[u].format,
                    st.tex[u].width, st.tex[u].height, st.tex[u].mipmap, st.tex[u].remap, st.tex[u].location, st.tex[u].offset,
                    st.samp[u].min_filter, st.samp[u].mag_filter, st.samp[u].wrap_s, st.samp[u].wrap_t);
    for (uint32_t k = 0; k < fc->count; k++)
        fprintf(f, "fpconst %u: %g %g %g %g\n", k, fc->values[k][0], fc->values[k][1], fc->values[k][2], fc->values[k][3]);
    fprintf(f, "raster blend %u %04X/%04X alpha %u %04X ref %u depth %u/%u %04X cull %u %04X/%04X cmask %08X\n",
            st.raster.blend, st.raster.blend_sf_rgb, st.raster.blend_df_rgb, st.raster.alpha_test, st.raster.alpha_func,
            st.raster.alpha_ref, st.raster.depth_test, st.raster.depth_write, st.raster.depth_func, st.raster.cull,
            st.raster.cull_face, st.raster.front_face, st.raster.color_mask);
    fprintf(f, "\n---- vertex program (HLSL)\n%s\n---- fragment program (HLSL)\n%s\n", vs.c_str(), ps.c_str());
    fclose(f);
}

// The pipeline for an asp record: its programs as for PPFX's (pipeline()),
// with the alpha test in the fragment program, and its whole render state:
// blend, depth, stencil, cull, polygon offset (dynamic), the colour mask;
// a colour format of VK_FORMAT_UNDEFINED for a depth-only pass. Null when it
// cannot be made.
// R5: what asp's draws read from a fragment program's bytes, once a
// program -- records' programs are interned by content, so a blob's hash
// names its bytes: the units it samples, its structural hash, its buffered
// constants, and (by the textures' gamma and expansion masks) the units
// those apply to
struct FpInfo {
    uint32_t sampled = 0;
    bool ok = false;
    uint64_t shash = 0;
    uint32_t nconst = 0;
    std::vector<uint32_t> consts;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> units;
};
std::unordered_map<uint64_t, FpInfo> s_fpinfo;
FpInfo& fp_info(const Blob* b)
{
    const uint64_t key = b->hash ^ ((uint64_t)b->bytes.size() << 40);
    auto it = s_fpinfo.find(key);
    if (it != s_fpinfo.end()) return it->second;
    if (s_fpinfo.size() > 8192) s_fpinfo.clear();
    FpInfo& f = s_fpinfo[key];
    const uint8_t* fp = b->bytes.data();
    const uint32_t n = (uint32_t)b->bytes.size();
    f.sampled = rsx_fp_texture_units(fp, n);
    f.shash = rsx_fp_structural_hash(fp, n, 0xF5ull);
    rsx_fp_constant_block cb;
    cb.count = 0;
    f.ok = rsx_fp_collect_constants(fp, n, &cb) >= 0;
    if (f.ok) { f.nconst = cb.count; f.consts.assign(&cb.values[0][0], &cb.values[0][0] + (size_t)cb.count * 4); }
    return f;
}
// a vertex program's inputs (the layout's mask), once a program
std::unordered_map<uint64_t, uint32_t> s_vpmask;

const Pipe* pipeline_asp(const Rec* r, const rsx_vertex_layout_plan& layout, VkFormat rt, VkFormat df,
                         rsx_fp_constant_block* consts)
{
    const State& st = r->st;
    const uint8_t* vp = r->vp->bytes.data();
    const uint32_t vp_bytes = (uint32_t)r->vp->bytes.size();
    const uint8_t* fp = r->fp->bytes.data();
    const uint32_t fp_size = (uint32_t)r->fp->bytes.size();
    const uint32_t fp_ctrl = fp_control(st.fp);
    const uint32_t srgb = rt != VK_FORMAT_UNDEFINED ? rsx_fp_srgb_output_enabled(st.fp.gamma, fp_ctrl, st.surface.color_format & 0x1Fu) : 0;
    FpInfo& fi = fp_info(r->fp.get());
    const uint32_t sampled = fi.sampled;
    uint32_t cube = 0, vol = 0, shadow_mask = 0, unnorm_mask = 0;
    rsx_fp_shadow_desc shadow[16] = {};
    uint8_t gamma[16] = {}, expand[16] = {};
    uint32_t unnorm[16][3] = {};
    for (uint32_t u = 0; u < 16; u++) {
        const Texture& t = st.tex[u];
        const Sampler& sm = st.samp[u];
        if (!sm.enable) continue;
        if (t.cubemap && t.width && t.width == t.height) cube |= 1u << u;
        if (!t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height) vol |= 1u << u;
        gamma[u] = gamma_mask(t, sm);
        expand[u] = expand_mask(t, sm);
        if (sampled >> u & 1) {
            shadow_desc(&shadow[u], t, sm);
            if (shadow[u].enabled) shadow_mask |= 1u << u;
        }
        if ((t.format & RSX_TEX_FMT_UNNORM) && t.width && t.height) {
            unnorm_mask |= 1u << u;
            unnorm[u][0] = t.width; unnorm[u][1] = t.height; unnorm[u][2] = t.depth;
        }
    }
    uint32_t gamma_units, expand_units;
    {
        const uint64_t gk = fnv(gamma, sizeof gamma, fnv(expand, sizeof expand));
        auto gt = fi.units.find(gk);
        if (gt != fi.units.end()) { gamma_units = gt->second.first; expand_units = gt->second.second; }
        else {
            gamma_units = rsx_fp_texture_gamma_units(fp, fp_size, gamma);
            expand_units = rsx_fp_texture_expand_units(fp, fp_size, expand);
            if (fi.units.size() < 64) fi.units[gk] = { gamma_units, expand_units };
        }
    }
    for (uint32_t u = 0; u < 16; u++) if (!(gamma_units >> u & 1)) gamma[u] = 0;
    for (uint32_t u = 0; u < 16; u++) if (!(expand_units >> u & 1)) expand[u] = 0;
    unnorm_mask &= sampled & ~shadow_mask;
    for (uint32_t u = 0; u < 16; u++) if (!(unnorm_mask >> u & 1)) unnorm[u][0] = unnorm[u][1] = unnorm[u][2] = 0;
    consts->count = 0;
    if (fi.ok) { consts->count = fi.nconst; if (fi.nconst) memcpy(consts->values, fi.consts.data(), (size_t)fi.nconst * 16); }
    if (!fi.ok) {
        static int said = 0;
        if (said++ < 4) {
            char line[200];
            snprintf(line, sizeof line, "render: asp: the fragment program's constants could not be collected (%u bytes, first %02X%02X%02X%02X)",
                     fp_size, fp[0], fp[1], fp[2], fp[3]);
            report_add(line);
        }
        return nullptr;
    }
    const Raster& ra = st.raster;
    const uint32_t alpha_test = ra.alpha_test ? 1u : 0u, alpha_func = ra.alpha_func;
    const uint64_t vk[2] = { r->vp->hash, (uint64_t)vp_bytes };
    uint64_t k = fnv(&fi.shash, 8, fnv(vk, sizeof vk, 0xA5Bull));
    const uint32_t ints[] = { fp_ctrl & 0x4Eu, srgb, cube, vol, shadow_mask, unnorm_mask, gamma_units, expand_units,
                              layout.mask, layout.stride, (uint32_t)rt, (uint32_t)df, ra.color_mask, ra.front_face,
                              ra.cull, ra.cull_face, ra.blend, ra.blend_sf_rgb, ra.blend_df_rgb, ra.blend_sf_a, ra.blend_df_a,
                              ra.blend_eq_rgb, ra.blend_eq_a, ra.depth_test, ra.depth_write, ra.depth_func,
                              ra.stencil_test, ra.stencil_func, ra.stencil_func_mask, ra.stencil_write_mask,
                              ra.stencil_fail, ra.stencil_zfail, ra.stencil_zpass, alpha_test, alpha_test ? alpha_func : 0 };
    k = fnv(ints, sizeof ints, k);
    k = fnv(gamma, sizeof gamma, k); k = fnv(expand, sizeof expand, k);
    k = fnv(unnorm, sizeof unnorm, k); k = fnv(shadow, sizeof shadow, k);
    auto it = s_pipes.find(k);
    if (it != s_pipes.end()) return it->second.p ? &it->second : nullptr;
    Pipe& P = s_pipes[k];
    P.p = VK_NULL_HANDLE;
    const uint32_t wpos[2] = { 1u, 1u };
    ProgParams pp = { vp, vp_bytes, fp, fp_size, fp_ctrl, cube, vol, 0u, layout.mask, 0u, 0u, srgb,
                      alpha_test, alpha_func, unnorm_mask, shadow_mask, gamma_units, expand_units,
                      { wpos[0], wpos[1] }, 0, shadow, gamma, expand, (const uint32_t (*)[3])unnorm };
    std::string vs, ps;
    uint32_t nconst = 0;
    if (!programs_text(pp, vs, ps, &nconst) || nconst != consts->count) {
        report_add("render: an asp record's programs did not translate");
        return nullptr;
    }
    asp_describe(r, vs, ps, layout, consts);
    VkShaderModule vsm = spirv_module(vs, RSX_SHADER_STAGE_VERTEX), fsm = spirv_module(ps, RSX_SHADER_STAGE_FRAGMENT);
    if (!vsm || !fsm) return nullptr;
    VkPipelineShaderStageCreateInfo stg[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main", nullptr } };
    VkVertexInputBindingDescription vb = { 0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[16];
    for (uint32_t i = 0; i < layout.count; i++) va[i] = { i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16u };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = layout.count; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // every primitive rebuilt as a triangle list
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    // as the engine (rsx_vulkan_backend.c): CULL_FACE FRONT and FRONT_AND_BACK
    // cull front faces; the winding judged on screen after the vertex stage's Y flip
    VkPipelineRasterizationStateCreateInfo ras = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    ras.polygonMode = VK_POLYGON_MODE_FILL;
    ras.cullMode = !ra.cull ? VK_CULL_MODE_NONE
                 : (ra.cull_face == 0x0404u || ra.cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    ras.frontFace = ra.front_face == 0x0901u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    {   // DOTHEROES_ASP_CULL_FLIP=1 (a test of the winding's orientation): the other face front
        static int flip = -1;
        if (flip < 0) { const char* e = getenv("DOTHEROES_ASP_CULL_FLIP"); flip = e && *e == '1'; }
        if (flip) ras.frontFace = ras.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    }
    ras.depthBiasEnable = VK_TRUE; ras.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkStencilOpState face = {};
    face.failOp = gcm_stencil_op(ra.stencil_fail); face.depthFailOp = gcm_stencil_op(ra.stencil_zfail);
    face.passOp = gcm_stencil_op(ra.stencil_zpass); face.compareOp = gcm_compare(ra.stencil_func);
    face.compareMask = ra.stencil_func_mask; face.writeMask = ra.stencil_write_mask; face.reference = 0;   // dynamic
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = df != VK_FORMAT_UNDEFINED && ra.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = ds.depthTestEnable && ra.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = gcm_compare(ra.depth_func);
    ds.stencilTestEnable = df != VK_FORMAT_UNDEFINED && has_stencil(df) && ra.stencil_test ? VK_TRUE : VK_FALSE;
    ds.front = face; ds.back = face;
    ds.minDepthBounds = 0.0f; ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = ra.blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = gcm_blend_factor(ra.blend_sf_rgb); cba.dstColorBlendFactor = gcm_blend_factor(ra.blend_df_rgb);
    cba.colorBlendOp = gcm_blend_op(ra.blend_eq_rgb);
    cba.srcAlphaBlendFactor = gcm_blend_factor(ra.blend_sf_a); cba.dstAlphaBlendFactor = gcm_blend_factor(ra.blend_df_a);
    cba.alphaBlendOp = gcm_blend_op(ra.blend_eq_a);
    cba.colorWriteMask = write_mask(ra.color_mask);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = rt != VK_FORMAT_UNDEFINED ? 1u : 0u; cb.pAttachments = &cba;
    const VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 5; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gci.stageCount = 2; gci.pStages = stg; gci.pVertexInputState = &vi; gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vps; gci.pRasterizationState = &ras; gci.pMultisampleState = &ms; gci.pColorBlendState = &cb;
    gci.pDepthStencilState = &ds; gci.pDynamicState = &dy; gci.layout = s_pl; gci.renderPass = render_pass2(rt, df);
    const VkResult res = F.vkCreateGraphicsPipelines(D.device, VK_NULL_HANDLE, 1, &gci, nullptr, &P.p);
    F.vkDestroyShaderModule(D.device, vsm, nullptr);
    F.vkDestroyShaderModule(D.device, fsm, nullptr);
    if (res != VK_SUCCESS) { fail("vkCreateGraphicsPipelines (asp)", res); P.p = VK_NULL_HANDLE; return nullptr; }
    P.nconst = nconst;
    stats().n_pipelines++;
    return &P;
}

// ---- a record's inputs ---------------------------------------------------------------------

// The vertex fetch reads guest memory through this: the record's snapshot.
const uint8_t* snap_ptr(void* user, uint32_t location, uint32_t offset, uint32_t n)
{
    const Rec* r = (const Rec*)user;
    for (uint32_t k = 0; k < r->nvr; k++) {
        const VtxRange& v = r->vr[k];
        if (v.location == location && offset >= v.offset && (uint64_t)offset + n <= (uint64_t)v.offset + v.bytes)
            return &r->vdata[v.at + (offset - v.offset)];
    }
    return nullptr;
}

// The toolkit's vertex fetch plan, from the record's vertex inputs (what
// rsx_vertex_fetch_plan_init fills from a register file).
void fetch_plan(rsx_vertex_fetch_plan* plan, const Rec* r, const rsx_vertex_layout_plan& layout)
{
    memset(plan, 0, sizeof *plan);
    plan->layout = layout;
    plan->base_offset = r->st.vdata.base_offset;
    plan->divider_mask = r->st.vdata.divider_op;
    plan->guest_ptr = snap_ptr;
    plan->guest_user = (void*)r;
    for (uint32_t a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        const VertexInput& in = r->st.vin[a];
        rsx_vertex_fetch_attr& f = plan->attr[a];
        f.desc.type = in.type & 7u; f.desc.size = in.size; f.desc.stride = in.stride;
        f.desc.frequency = in.frequency; f.desc.offset = in.offset & 0x7FFFFFFFu; f.desc.location = in.location;
        f.elem_size = rsx_vertex_attrib_size(f.desc.type, f.desc.size);
        f.stride = f.desc.stride ? f.desc.stride : f.elem_size;
        memcpy(f.default_value, r->st.vdata.current[a], 16);
        if (a == 3 && f.default_value[0] == 0.0f && f.default_value[1] == 0.0f && f.default_value[2] == 0.0f &&
            f.default_value[3] == 1.0f)
            f.default_value[0] = f.default_value[1] = f.default_value[2] = 1.0f;
    }
}

// A static texture from the bytes the recorder took (the engine's
// eng_texture_upload, one face), by content.
uint64_t fast_hash(const uint8_t* p, size_t n, uint64_t seed);
// R6 (R5's open item: the water's texture made a new image every frame):
// static textures the sweep drops go into a pool once every batch that may
// have used them has completed (as retired images are destroyed: in the open
// batch's completion, or at once with none in flight), and a texture of the
// same shape takes one from there instead of a new image.
std::multimap<uint64_t, Img*> s_static_pool;   // shape -> images no batch uses
std::vector<Img*> s_static_swept;              // dropped, not yet known unused by the GPU
uint64_t s_static_reused = 0, s_static_pool_destroyed = 0;
uint64_t shape_key(VkFormat f, uint32_t w, uint32_t h, uint32_t lv)
{
    return (uint64_t)f << 48 ^ (uint64_t)w << 32 ^ (uint64_t)h << 16 ^ lv;
}
void static_pool_put(Img* o)
{
    if (s_static_pool.size() >= 128) { destroy_image(o); s_static_pool_destroyed++; return; }
    s_static_pool.emplace(shape_key(o->fmt, o->w, o->h, o->levels), o);
}
Img* static_pool_take(VkFormat f, uint32_t w, uint32_t h, uint32_t lv)
{
    auto it = s_static_pool.find(shape_key(f, w, h, lv));
    if (it == s_static_pool.end()) return nullptr;
    Img* o = it->second;
    s_static_pool.erase(it);
    if (o->fmt != f || o->w != w || o->h != h || o->levels != lv) { destroy_image(o); return nullptr; }
    s_static_reused++;
    return o;
}

Img* static_texture(Batch& b, const Rec* r, uint32_t unit, const Texture& t)
{
    const VtxRange* snap = nullptr;
    for (uint32_t k = 0; k < r->ntex; k++) if (r->tex[k].location == unit) snap = &r->tex[k];
    if (!snap || t.cubemap || t.dimension == 3) return nullptr;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const uint32_t nlv = rsx_texture_mip_chain(t.format, t.width, t.height, t.mipmap, t.pitch, lv);
    if (!nlv || lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes > snap->bytes) return nullptr;
    uint32_t bpp;
    const VkFormat vf = be_vkformat(texfmt_be(lv[0].tl.fmt), &bpp);
    if (vf == VK_FORMAT_UNDEFINED) return nullptr;
    const uint8_t* src = &r->tdata[snap->at];
    uint64_t h = fast_hash(src, snap->bytes, 0x57A7ull);
    const uint32_t key[] = { t.format, t.width, t.height, nlv, t.pitch };
    h = fnv(key, sizeof key, h);
    auto it = s_static.find(h);
    if (it != s_static.end()) { it->second.frame = r->frame; return it->second.img; }
    Img* o = static_pool_take(vf, t.width, t.height, nlv);
    if (!o) o = make_image(vf, t.width, t.height, nlv, bpp, false);
    if (!o) return nullptr;
    access(b.cb, o, kCopyDst);
    std::vector<uint8_t> dec;
    for (uint32_t m = 0; m < nlv; m++) {
        const rsx_tex_layout* tl = &lv[m].tl;
        dec.assign((size_t)tl->dst_row_bytes * tl->rows, 0);
        rsx_texture_decode(dec.data(), tl->dst_row_bytes, src + lv[m].offset, lv[m].w, lv[m].h, tl,
                           rsx_texture_argb_is_rgba());
        const VkDeviceSize at = upload(b, dec.data(), dec.size(), 16);
        if (at == ~(VkDeviceSize)0) { destroy_image(o); return nullptr; }
        VkBufferImageCopy c = {};
        c.bufferOffset = at; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
        c.imageExtent = { lv[m].w, lv[m].h, 1 };
        F.vkCmdCopyBufferToImage(b.cb, b.up.b, o->img, layout_of(o), 1, &c);
    }
    mark(b, kSectCopy);
    o->written = 1;
    stats().n_static_textures++;
    // those not used for 8 frames go, swept once a frame (R5: a texture the
    // game rewrites each frame, the water's, adds one a frame; retiring 256
    // at once -- all of them, or the unused ones -- held the native thread
    // ~33 ms every 3.4 s: every texture uploaded again, or 200 images freed)
    static uint32_t swept = ~0u;
    if (swept != r->frame) {
        swept = r->frame;
        for (auto jt = s_static.begin(); jt != s_static.end();)
            if (r->frame - jt->second.frame > 8) { s_static_swept.push_back(jt->second.img); jt = s_static.erase(jt); }
            else ++jt;
    }
    s_static[h] = { o, r->frame };
    return o;
}

VkImageView view_of(Img* o, uint32_t crossbar, uint32_t fmt)
{
    const uint64_t k = (uint64_t)crossbar << 32 | fmt;
    auto it = o->views.find(k);
    if (it != o->views.end()) return it->second;
    return o->views[k] = make_view(o->img, o->fmt, o->levels, remap(crossbar, fmt));
}

int asp_native();
void asp_compare(Batch& b, const Rec* r, Img* mine, Img* eng, uint32_t off);

// The handoffs of a record: the engine's copies, from their slots into the
// native images of their addresses.
void apply_handoffs(Batch& b, const Rec* r)
{
    for (uint32_t k = 0; k < r->nneed; k++) {
        const int s = r->walk.hand[k];
        if (s < 0) continue;
        Img* src;
        { std::lock_guard<std::mutex> g(s_slot_lock); src = s_simg[s].im; }
        const Need& n = r->need[k];
        Img*& dst = s_targets[akey(n.location, n.offset)];
        if (!dst || dst->fmt != src->fmt || dst->w < src->w || dst->h < src->h) {
            if (dst) { s_retired.push_back(dst); stats().n_retired++; }
            dst = make_image(src->fmt, src->w, src->h, 1, src->bpp, true);
            if (!dst) continue;
        }
        // R5: asp drew this target natively this frame -- the engine's copy
        // of it is the reference: both read back and compared, once the batch is done
        if (asp_native() && dst->asp_frame == r->frame + 1 && dst->fmt == src->fmt && dst->w == src->w && dst->h == src->h)
            asp_compare(b, r, dst, src, n.offset);
        access(b.cb, src, kCopySrc);
        access(b.cb, dst, kCopyDst);
        VkImageCopy c = {};
        c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.extent = { src->w, src->h, 1 };
        F.vkCmdCopyImage(b.cb, src->img, layout_of(src), dst->img, layout_of(dst), 1, &c);
        mark(b, kSectCopy);
        stats().n_handoff_copies++;
        dst->written = 1;
        b.done.push_back([s] { std::lock_guard<std::mutex> g(s_slot_lock); s_simg[s].busy = false; });
    }
}

// ---- chain variants -----------------------------------------------------------------------
// A frame's chain structure: the sequence of its records' targets (address
// and size) and, as a second level, of their fragment programs' content. Each
// frame's comparisons are counted under its structure and program variant.

struct FrameAcc { uint64_t shape = 1469598103934665603ull, progs = 1469598103934665603ull; uint32_t draws = 0;
                  uint32_t eq = 0, diff = 0, fin = 0, fin_eq = 0; int pending = 0; std::string desc; };
struct VarAcc { std::string desc; uint64_t frames = 0, cmp_frames = 0, eq = 0, diff = 0, fin = 0, fin_eq = 0;
                std::map<uint64_t, uint64_t> progs; };
std::mutex s_var_lock;
std::map<uint32_t, FrameAcc> s_frames;   // open and recent frames
std::map<uint64_t, VarAcc> s_vars;

void frame_note(const Rec* r, uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    FrameAcc& f = s_frames[r->frame];
    // the final composite alternates between the display buffers: one place
    const uint32_t t[3] = { r->need[0].format == 0x08 ? 0xD15Bu : r->need[0].offset, w, h };
    f.shape = fnv(t, sizeof t, f.shape);
    // programs by structure: the constants PPFX patches each frame are not a variant
    const uint64_t ph = r->fp ? rsx_fp_structural_hash(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size(), 0) : 0;
    f.progs = fnv(&ph, 8, f.progs);
    if (f.draws < 3 || f.desc.size() < 200) {
        char b[48];
        snprintf(b, sizeof b, "%s%ux%u", f.draws ? " " : "", w, h);
        if (f.draws < 64) f.desc += b;
    }
    f.draws++;
}

std::mutex s_inh_lock;
std::set<uint32_t> s_inherited;   // frames the cross-check flagged (bounded: the latest 4096)

bool frame_is_inherited(uint32_t frame)
{
    std::lock_guard<std::mutex> g(s_inh_lock);
    return s_inherited.count(frame) != 0;
}

void frame_result(uint32_t frame, bool equal, bool final_)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    FrameAcc& f = s_frames[frame];
    (equal ? f.eq : f.diff)++;
    if (final_) { f.fin++; if (equal) f.fin_eq++; }
}

void frame_pending(uint32_t frame, int d)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    s_frames[frame].pending += d;
}

// Frames well behind the renderer whose comparisons have all run are
// complete: into their variant's totals.
void frames_close(uint32_t before)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    for (auto it = s_frames.begin(); it != s_frames.end() && it->first + 8 < before;) {
        FrameAcc& f = it->second;
        if (f.pending > 0) { ++it; continue; }
        char key[64];
        snprintf(key, sizeof key, "%u draws", f.draws);
        VarAcc& v = s_vars[f.shape];
        if (v.desc.empty()) v.desc = std::string(key) + ": " + f.desc;
        v.frames++;
        v.progs[f.progs]++;
        if (f.eq + f.diff > f.fin) v.cmp_frames++;
        v.eq += f.eq; v.diff += f.diff; v.fin += f.fin; v.fin_eq += f.fin_eq;
        it = s_frames.erase(it);
    }
}

// ---- comparison ----------------------------------------------------------------------------

std::atomic<int> s_dumped{0};

void dump(const char* side, uint32_t frame, uint64_t seq, uint32_t off, const uint8_t* p, size_t n, uint32_t w,
          uint32_t h, VkFormat f)
{
    char path[700];
    snprintf(path, sizeof path, "%s/f%u_r%llu_%08X_%ux%u_vk%d_%s.bin", s_dump, frame, (unsigned long long)seq, off, w,
             h, (int)f, side);
    FILE* o = fopen(path, "wb");
    if (o) { fwrite(p, 1, n, o); fclose(o); }
}

// The comparisons run on a thread of their own, so the native renderer is
// not held up by them: a job per compared record, queued at its batch's
// completion (both copies are then complete), bounded (the renderer waits
// for room: counted). Readback buffers come from a pool.
struct CmpJob { Buf mine; int ref; VkDeviceSize bytes; uint32_t w, h, bpp, ref_w, ref_h, frame, off; uint64_t seq;
                int final_; int change; VkFormat f; char desc[200]; };
std::mutex s_cmp_lock;
std::condition_variable s_cmp_cv;
std::vector<CmpJob> s_cmp_q;
std::atomic<int> s_cmp_running{0};
const size_t kCmpQ = 512;
std::multimap<VkDeviceSize, Buf> s_rb_pool;   // free readback buffers by size

Buf rb_take(VkDeviceSize bytes)
{
    {
        std::lock_guard<std::mutex> g(s_cmp_lock);
        auto it = s_rb_pool.lower_bound(bytes);
        if (it != s_rb_pool.end() && it->first < bytes * 2) { Buf b = it->second; s_rb_pool.erase(it); return b; }
    }
    Buf b;
    if (!make_buffer(b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return Buf();
    return b;
}

void release_ref(int ref)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (ref >= 0 && (size_t)ref < s_sbuf.size()) s_sbuf[ref].busy = false;
}

void cmp_run(CmpJob& j)
{
    const uint64_t t0 = now_ns();
    Stats& st = stats();
    st.cmp_records++;
    if (j.final_) st.cmp_finals++;
    Buf eng;
    { std::lock_guard<std::mutex> g(s_slot_lock); eng = s_sbuf[j.ref].b; }
    const bool same = j.ref_w == j.w && j.ref_h == j.h && eng.n >= j.bytes;
    const bool equal = same && !memcmp(j.mine.p, eng.p, (size_t)j.bytes);
    {   // DOTHEROES_ASP_DESCRIBE_FRAME: every comparison of that frame, a line each
        static long long want = -2;
        static FILE* cf = nullptr;
        if (want == -2) { const char* e = getenv("DOTHEROES_ASP_DESCRIBE_FRAME"); want = e && *e ? atoll(e) : -1; }
        if (want >= 0 && (long long)j.frame == want && s_dump[0] && same) {
            if (!cf) { char path[600]; snprintf(path, sizeof path, "%s/compare_f%u.txt", s_dump, j.frame); cf = fopen(path, "w"); }
            if (cf) {
                uint64_t px = 0;
                if (!equal) for (VkDeviceSize i = 0; i < j.bytes; i += j.bpp) px += memcmp(j.mine.p + i, eng.p + i, j.bpp) != 0;
                fprintf(cf, "%llu %08X %llu\n", (unsigned long long)j.seq, j.off, (unsigned long long)px);
                fflush(cf);
            }
        }
    }
    if (!same) st.cmp_skip_extent++;
    else {
        if (j.change) st.cmp_change_done++;
        frame_result(j.frame, equal, j.final_);
        if (equal) { st.cmp_equal++; if (j.final_) st.cmp_finals_equal++; }
        else {
            st.cmp_differ++;
            if (frame_is_inherited(j.frame)) st.cmp_differ_inherited++;
            uint64_t differ = 0;
            for (VkDeviceSize i = 0; i < j.bytes; i += j.bpp) differ += memcmp(j.mine.p + i, eng.p + i, j.bpp) != 0;
            // the first differing record of each (frame, target): the one that names the cause
            static std::set<uint64_t> firsts;
            const bool first = firsts.size() < 4096 && firsts.insert((uint64_t)j.frame << 32 | j.off).second;
            static int said = 0;
            if (first && said++ < 64) {
                char line[480];
                snprintf(line, sizeof line, "compare: record %llu frame %u target %08X %ux%u: %llu of %u pixels differ (the first there) %s",
                         (unsigned long long)j.seq, j.frame, j.off, j.w, j.h, (unsigned long long)differ, j.w * j.h, j.desc);
                report_add(line);
            }
            if (first && s_dump[0] && s_dumped++ < 16) {
                dump("native", j.frame, j.seq, j.off, j.mine.p, (size_t)j.bytes, j.w, j.h, j.f);
                dump("engine", j.frame, j.seq, j.off, eng.p, (size_t)j.bytes, j.w, j.h, j.f);
            }
        }
    }
    frame_pending(j.frame, -1);
    release_ref(j.ref);
    {
        std::lock_guard<std::mutex> g(s_cmp_lock);
        VkDeviceSize pooled = 0;
        for (auto& kv : s_rb_pool) pooled += kv.first;
        if (pooled + j.mine.n <= kReadbackPoolBytes) s_rb_pool.emplace(j.mine.n, j.mine);
        else { F.vkDestroyBuffer(D.device, j.mine.b, nullptr); F.vkFreeMemory(D.device, j.mine.m, nullptr); }
    }
    st.compare_ns += now_ns() - t0;
}

void cmp_thread()
{
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"ppfx compare");
#endif
    for (;;) {
        CmpJob j;
        {
            std::unique_lock<std::mutex> lk(s_cmp_lock);
            s_cmp_cv.wait(lk, [] { return !s_cmp_q.empty(); });
            j = s_cmp_q.front();
            s_cmp_q.erase(s_cmp_q.begin());
            s_cmp_running = 1;
        }
        s_cmp_cv.notify_all();
        cmp_run(j);
        s_cmp_running = 0;
        s_cmp_cv.notify_all();
    }
}

// A record whose output the walker copied: its native output read back
// after it, compared once both copies are complete. Every record the
// comparison asked for ends counted (wanted = equal + differ + skipped +
// unfinished).
void compare_after(Batch& b, const Rec* r, Img* t, uint32_t w, uint32_t h)
{
    const int ref = r->walk.ref;
    if (ref < 0) return;
    CmpJob j;
    j.desc[0] = 0;
    if (r->kind == kAspDraw) {   // what the draw draws with, for the first differing one's line
        const Raster& ra = r->st.raster;
        int at = snprintf(j.desc, sizeof j.desc, "| asp prim %u n %u fp %016llx blend %u %X/%X alpha %u depth %u/%u stencil %u cmask %08X tex",
                          r->prim, r->count, r->fp ? (unsigned long long)r->fp->hash : 0ull, ra.blend, ra.blend_sf_rgb, ra.blend_df_rgb,
                          ra.alpha_test, ra.depth_test, ra.depth_write, ra.stencil_test, ra.color_mask);
        for (uint32_t u = 0; u < 16 && at < (int)sizeof j.desc - 24; u++)
            if (r->st.samp[u].enable) at += snprintf(j.desc + at, sizeof j.desc - at, " %u:%02X@%08X", u, r->st.tex[u].format, r->st.tex[u].offset);
    }
    j.bytes = (VkDeviceSize)w * h * t->bpp;
    j.mine = rb_take(j.bytes);
    if (!j.mine.b) { stats().cmp_skip_no_readback++; release_ref(ref); return; }
    access(b.cb, t, kCopySrc);
    VkBufferImageCopy c = {};
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { w, h, 1 };
    F.vkCmdCopyImageToBuffer(b.cb, t->img, layout_of(t), j.mine.b, 1, &c);
    mark(b, kSectReadback);
    // what the comparison needs after the record is gone
    j.ref = ref; j.w = w; j.h = h; j.bpp = t->bpp; j.ref_w = r->walk.ref_w; j.ref_h = r->walk.ref_h;
    j.frame = r->frame; j.off = r->need[0].offset; j.seq = r->seq; j.final_ = r->need[0].format == 0x08; j.f = t->fmt;
    j.change = r->walk.cmp_why == kCmpChange;
    frame_pending(j.frame, +1);
    b.done.push_back([j] {
        const uint64_t t0 = now_ns();
        std::unique_lock<std::mutex> lk(s_cmp_lock);
        if (s_cmp_q.size() >= kCmpQ) stats().cmp_queue_waits++;
        s_cmp_cv.wait(lk, [] { return s_cmp_q.size() < kCmpQ; });
        s_cmp_q.push_back(j);
        lk.unlock();
        s_cmp_cv.notify_all();
        stats().cmp_queue_wait_ns += now_ns() - t0;
    });
}

// ---- one record ------------------------------------------------------------------------------

std::mutex s_why_lock;
std::map<std::string, uint64_t> s_why;   // records not rendered, by reason (the report)
void unsupported(const Rec* r, const char* why)
{
    stats().n_unsupported++;
    {
        std::lock_guard<std::mutex> g(s_why_lock);
        std::string k(why);
        const size_t cut = k.find(" at ");   // a texture's address: one reason per kind
        if (cut != std::string::npos && k.compare(0, 9, "asp: unit") == 0) k = k.substr(0, cut);
        s_why[k]++;
    }
    // a comparison it was copied for cannot run
    if (r->walk.ref >= 0) { stats().cmp_skip_not_drawn++; release_ref(r->walk.ref); }
    static int said = 0;
    if (said++ < 20) {
        char line[200];
        snprintf(line, sizeof line, "render: record %llu (frame %u) not rendered: %s", (unsigned long long)r->seq,
                 r->frame, why);
        report_add(line);
    }
}

// The engine's eng_scissor_rect, on the record's scissor.
void scissor_rect(const Scissor& s, uint32_t w, uint32_t h, uint32_t out[4])
{
    uint32_t sx = 0, sy = 0, sw = w, sh = h;
    const uint32_t gx = s.x, gw = s.w, gy = s.y, gh = s.h;
    if (gw > 0 && gh > 0) {
        uint32_t right = sx + sw, bottom = sy + sh;
        if (gx > sx) sx = gx;
        if (gy > sy) sy = gy;
        if (gx + gw < right) right = gx + gw;
        if (gy + gh < bottom) bottom = gy + gh;
        sw = right > sx ? right - sx : 0;
        sh = bottom > sy ? bottom - sy : 0;
    }
    out[0] = sx; out[1] = sy; out[2] = sw; out[3] = sh;
}

long long s_waited = -1;   // the last engine signal a batch waits on

// The engine's signal a record's copies are ordered by: waited on once, in
// the batch of the first record that needs it.
void wait_for(Batch& b, const Rec* r)
{
    for (int16_t k : r->walk.release_sem) if (k >= 0) b.release.push_back(k);
    if (r->walk.sem < 0 || r->walk.ordinal <= s_waited) return;
    b.sems.push_back(r->walk.sem);
    b.ords.push_back(r->walk.ordinal);
    s_waited = r->walk.ordinal;
}

void asp_clears(Batch& b, const Rec* r);
void occ_begin(Batch& b, const Rec* r);   // R6: a counted draw's occlusion query
void occ_end(Batch& b);
void draw(const Rec* r)
{
    Batch& b = batch(r->frame);
    wait_for(b, r);
    if (r->naclear) asp_clears(b, r);   // R6: the clears made before it (the game's, asp's)
    if (r->why) { unsupported(r, "no programs or vertex data"); return; }
    const State& st = r->st;
    const Surface& sf = st.surface;
    if (sf.color_target != 1 || sf.antialias) { unsupported(r, "MRT or antialiased target"); return; }
    const Raster& ra = st.raster;
    if (ra.blend || ra.depth_test || ra.stencil_test || ra.alpha_test || ra.cull) {
        unsupported(r, "render state beyond PPFX's (blend, depth, stencil, alpha test, cull)");
        return;
    }
    uint32_t bpp = 0;
    const VkFormat rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
    const uint32_t W = sf.width, H = sf.height;
    if (!W || !H) { unsupported(r, "no clip"); return; }
    apply_handoffs(b, r);
    Img* t = target_image(sf.color_location[0] ? 1u : 0u, sf.color_offset[0], rt, bpp, W, H);
    if (!t) { unsupported(r, "target image"); return; }
    if (sf.color_location[0]) { t->main_dirty = 1; t->guest_pitch = sf.color_pitch[0]; }   // R6: main memory: written back
    // the vertex layout and the vertices
    rsx_vertex_layout_plan layout;
    {
        uint32_t mask = 0xFFFFu;
        rsx_vp_input_analysis an = { 0xFFFFu, 0 };
        const uint32_t instrs = (uint32_t)r->vp->bytes.size() / 16;
        if (rsx_vp_analyze_inputs(r->vp->bytes.data(), instrs * 16u, &an) == (int)instrs && an.exact && an.input_mask)
            mask = an.input_mask;
        rsx_vertex_layout_plan_init(&layout, mask);
    }
    int indexed = 0;
    if (!rsx_vertex_topology_plan(r->prim, 0, &indexed)) { unsupported(r, "a primitive the engine does not rebuild"); return; }
    const uint32_t n = r->count;
    std::vector<rsx_vertex_ref> refs(n);
    for (uint32_t i = 0; i < n; i++) refs[i] = { r->first + i, 0 };
    rsx_vertex_fetch_plan plan;
    fetch_plan(&plan, r, layout);
    rsx_vertex_fetch_plan_prepare(&plan, refs.data(), n);
    std::vector<uint8_t> verts((size_t)n * layout.stride);
    if (!rsx_vertex_fetch_many(&plan, refs.data(), n, verts.data())) { unsupported(r, "vertex fetch"); return; }
    const uint32_t n_draw = indexed ? rsx_draw_engine_topology_index_count(r->prim, n, nullptr, 0) : n - n % 3;
    std::vector<uint32_t> idx(n_draw ? n_draw : 1);
    if (indexed) rsx_draw_engine_write_topology_indices(r->prim, n, nullptr, 0, nullptr, idx.data());
    else for (uint32_t i = 0; i < n_draw; i++) idx[i] = i;
    // the pipeline
    rsx_fp_constant_block fc;
    const Pipe* P = pipeline(r, layout, rt, &fc);
    if (!P) { unsupported(r, "pipeline"); return; }
    // constants: the vertex program's and the viewport epilogue; the fragment
    // program's buffered constants and the alpha/window slot
    const Viewport& vp = st.viewport;
    const float Wf = (float)W, Hf = (float)H;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.offset[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wf * 0.5f);
        xf[1] = -(vp.scale[1] / (Hf * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.offset[0] - Wf * 0.5f) / (Wf * 0.5f);
        xf[5] = -((vp.offset[1] - Hf * 0.5f) / (Hf * 0.5f));
        xf[6] = vp.offset[2];
    }
    uint8_t vcb[512 * 16 + 32];
    memcpy(vcb, s_vc, 512 * 16);
    memcpy(vcb + 512 * 16, xf, sizeof xf);
    const uint32_t nslots = fc.count ? fc.count : 1u;
    std::vector<uint8_t> pcb((nslots + 1) * 16, 0);
    if (fc.count) memcpy(pcb.data(), fc.values, fc.count * 16u);
    {
        // alpha test off (checked above): no reference; the window: its
        // height and origin as cellGcmSetSurface sets them (bottom left,
        // pixel centres at half)
        float* alpha = (float*)(pcb.data() + nslots * 16);
        alpha[0] = 0.0f;
        alpha[1] = (float)(sf.height & 0xFFFu);
        alpha[2] = 0.0f;
        alpha[3] = 0.0f;
    }
    // textures
    VkDescriptorImageInfo ii[16], si[16];
    const uint32_t sampled = rsx_fp_texture_units(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size());
    Img* srcs[16] = {};
    for (uint32_t u = 0; u < 16; u++) {
        if (!(sampled >> u & 1) || !st.samp[u].enable) continue;
        const Texture& tx = st.tex[u];
        Img* src = nullptr;
        auto it = s_targets.find(akey(tx.location ? 1u : 0u, tx.offset));
        if (it != s_targets.end() && it->second->written) src = it->second;
        if (src && src == t) {
            // its own target: a copy taken just before the draw
            Img*& snap = s_snap[std::make_tuple(t->fmt, t->w, t->h)];
            if (!snap) snap = make_image(t->fmt, t->w, t->h, 1, t->bpp, false);
            if (!snap) { unsupported(r, "snapshot image"); return; }
            access(b.cb, t, kCopySrc);
            access(b.cb, snap, kCopyDst);
            VkImageCopy c = {};
            c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.extent = { t->w, t->h, 1 };
            F.vkCmdCopyImage(b.cb, t->img, layout_of(t), snap->img, layout_of(snap), 1, &c);
            mark(b, kSectCopy);
            stats().n_snapshot_copies++;
            src = snap;
            stats().n_feedback++;
        }
        if (!src) src = static_texture(b, r, u, tx);
        if (!src) {
            char why[200];
            snprintf(why, sizeof why, "unit %u at %u:%08X (format %02X %ux%u pitch %u) has no native source%s", u,
                     tx.location, tx.offset, tx.format, tx.width, tx.height, tx.pitch,
                     it != s_targets.end() ? " (a native image not yet written)" : "");
            unsupported(r, why);
            return;
        }
        srcs[u] = src;
    }
    // every sampled image in the layout it is read in, after its writes
    for (uint32_t u = 0; u < 16; u++) if (srcs[u]) access(b.cb, srcs[u], kSampled);
    for (uint32_t u = 0; u < 16; u++) {
        ii[u] = { VK_NULL_HANDLE, s_dummy->view, sampled_layout() };
        si[u] = { s_dummy_smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        if (!srcs[u]) continue;
        ii[u].imageView = view_of(srcs[u], st.tex[u].remap & 0xFFFFu, st.tex[u].format);
        si[u].sampler = sampler(st.samp[u]);
    }
    // uploads
    const VkDeviceSize o_vc = upload(b, vcb, sizeof vcb), o_pc = upload(b, pcb.data(), pcb.size());
    const VkDeviceSize o_v = upload(b, verts.data(), verts.size(), 16), o_i = upload(b, idx.data(), idx.size() * 4, 16);
    if (o_vc == ~(VkDeviceSize)0 || o_pc == ~(VkDeviceSize)0 || o_v == ~(VkDeviceSize)0 || o_i == ~(VkDeviceSize)0 ||
        b.sets >= kSets) {
        unsupported(r, "the batch is full");   // not expected: batches are submitted well before
        return;
    }
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = b.dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dsl;
    VkDescriptorSet set;
    if (F.vkAllocateDescriptorSets(D.device, &dai, &set) != VK_SUCCESS) { unsupported(r, "descriptor set"); return; }
    b.sets++;
    VkDescriptorBufferInfo bi[2] = { { b.up.b, o_vc, sizeof vcb }, { b.up.b, o_pc, pcb.size() } };
    VkWriteDescriptorSet wr[4] = {};
    for (int k = 0; k < 4; k++) { wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = set; }
    wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[0].pBufferInfo = &bi[0];
    wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[1].pBufferInfo = &bi[1];
    wr[2].dstBinding = 16; wr[2].descriptorCount = 16; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[2].pImageInfo = ii;
    wr[3].dstBinding = 48; wr[3].descriptorCount = 16; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; wr[3].pImageInfo = si;
    F.vkUpdateDescriptorSets(D.device, 4, wr, 0, nullptr);
    // the draw. DOTHEROES_PPFX_HAZARD_TEST=1 (the synchronisation check's
    // negative control): before one draw in 5000, the target is copied twice
    // into a scratch image with no barrier between the copies -- a write
    // after write the validation layer must report
    static int hazard = -1;
    if (hazard < 0) { const char* e = getenv("DOTHEROES_PPFX_HAZARD_TEST"); hazard = e && *e == '1'; }
    if (hazard && stats().n_draws % 5000 == 4999) {
        static Img* scratch = nullptr;
        if (!scratch || scratch->fmt != t->fmt || scratch->w < t->w || scratch->h < t->h)
            scratch = make_image(t->fmt, t->w, t->h, 1, t->bpp, false);
        if (scratch) {
            access(b.cb, t, kCopySrc);
            access(b.cb, scratch, kCopyDst);
            VkImageCopy c = {};
            c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.extent = { t->w, t->h, 1 };
            F.vkCmdCopyImage(b.cb, t->img, layout_of(t), scratch->img, layout_of(scratch), 1, &c);
            F.vkCmdCopyImage(b.cb, t->img, layout_of(t), scratch->img, layout_of(scratch), 1, &c);
            report_add("render: hazard test: two copies into one image without a barrier");
        }
    }
    access(b.cb, t, kAttach);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = render_pass(rt); rbi.framebuffer = framebuffer(t);
    rbi.renderArea = { { 0, 0 }, { t->w, t->h } };
    F.vkCmdBeginRenderPass(b.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    F.vkCmdBindPipeline(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->p);
    VkViewport v = { 0.0f, 0.0f, Wf, Hf, 0.0f, 1.0f };
    F.vkCmdSetViewport(b.cb, 0, 1, &v);
    uint32_t sc[4];
    scissor_rect(st.scissor, t->w, t->h, sc);
    VkRect2D s = { { (int32_t)sc[0], (int32_t)sc[1] }, { sc[2], sc[3] } };
    F.vkCmdSetScissor(b.cb, 0, 1, &s);
    F.vkCmdSetStencilReference(b.cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);   // stencil off (checked above)
    F.vkCmdSetDepthBias(b.cb, 0.0f, 0.0f, 0.0f);
    const float bc[4] = { 0, 0, 0, 0 };   // blend is off (checked above): the constant is never read
    F.vkCmdSetBlendConstants(b.cb, bc);
    F.vkCmdBindDescriptorSets(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl, 0, 1, &set, 0, nullptr);
    F.vkCmdBindVertexBuffers(b.cb, 0, 1, &b.up.b, &o_v);
    F.vkCmdBindIndexBuffer(b.cb, b.up.b, o_i, VK_INDEX_TYPE_UINT32);
    occ_begin(b, r);
    F.vkCmdDrawIndexed(b.cb, n_draw, 1, 0, 0, 0);
    occ_end(b);
    F.vkCmdEndRenderPass(b.cb);
    mark(b, kSectDraw);
    t->written = 1;
    b.records++;
    stats().n_draws++;
    frame_note(r, W, H);
    compare_after(b, r, t, W, H);
}

// R5's check of asp's native output: the native image asp's records drew
// and the engine's copy of the same surface at the same place in the stream
// (the handoff that brings it to PPFX), read back in the batch and compared
// when it is done -- the pixels that differ and the largest difference of a
// channel (in units of the format: 8-bit steps, or half-float bit patterns).
std::atomic<uint64_t> s_aspc_n{0}, s_aspc_equal{0}, s_aspc_px{0}, s_aspc_px_total{0}, s_aspc_maxd{0};
void asp_compare(Batch& b, const Rec* r, Img* mine, Img* eng, uint32_t off)
{
    static int every = -1;
    if (every < 0) { const char* e = getenv("DOTHEROES_ASP_COMPARE"); every = e && *e ? atoi(e) : 1; }
    if (every <= 0 || r->frame % (uint32_t)every) return;
    const VkDeviceSize bytes = (VkDeviceSize)mine->w * mine->h * mine->bpp;
    if (!bytes || !mine->bpp) return;
    Buf a = rb_take(bytes), e = rb_take(bytes);
    if (!a.b || !e.b) return;
    VkBufferImageCopy c = {};
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { mine->w, mine->h, 1 };
    access(b.cb, mine, kCopySrc);
    F.vkCmdCopyImageToBuffer(b.cb, mine->img, layout_of(mine), a.b, 1, &c);
    access(b.cb, eng, kCopySrc);
    F.vkCmdCopyImageToBuffer(b.cb, eng->img, layout_of(eng), e.b, 1, &c);
    mark(b, kSectReadback);
    const uint32_t w = mine->w, h = mine->h, bpp = mine->bpp, frame = r->frame;
    const VkFormat f = mine->fmt;
    const uint64_t seq = r->seq;
    b.done.push_back([a, e, bytes, w, h, bpp, frame, f, seq, off] {
        const bool half = f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R16G16_SFLOAT;
        uint64_t px = 0, maxd = 0;
        for (VkDeviceSize i = 0; i < bytes; i += bpp) {
            if (!memcmp(a.p + i, e.p + i, bpp)) continue;
            px++;
            if (half) for (uint32_t k = 0; k + 2 <= bpp; k += 2) {
                const int x = *(const uint16_t*)(a.p + i + k), y = *(const uint16_t*)(e.p + i + k);
                const uint64_t d = (uint64_t)(x > y ? x - y : y - x);
                if (d > maxd) maxd = d;
            } else for (uint32_t k = 0; k < bpp; k++) {
                const int x = a.p[i + k], y = e.p[i + k];
                const uint64_t d = (uint64_t)(x > y ? x - y : y - x);
                if (d > maxd) maxd = d;
            }
        }
        s_aspc_n++;
        if (!px) s_aspc_equal++;
        s_aspc_px += px; s_aspc_px_total += (uint64_t)w * h;
        uint64_t m = s_aspc_maxd.load();
        while (maxd > m && !s_aspc_maxd.compare_exchange_weak(m, maxd)) {}
        static int said = 0;
        if (px && (said < 16 || (px > 1000 && said < 80))) {
            said++;
            char line[200];
            snprintf(line, sizeof line, "asp compare: frame %u target %08X %ux%u: %llu of %u pixels differ, largest %llu",
                     frame, off, w, h, (unsigned long long)px, w * h, (unsigned long long)maxd);
            report_add(line);
        }
        static long long from = -2;
        if (from == -2) { const char* e = getenv("DOTHEROES_ASP_DUMP_FROM"); from = e && *e ? atoll(e) : 0; }
        if (px > 1000 && (long long)frame >= from && s_dump[0] && s_dumped++ < 6) {
            dump("asp-native", frame, seq, off, a.p, (size_t)bytes, w, h, f);
            dump("asp-engine", frame, seq, off, e.p, (size_t)bytes, w, h, f);
        }
        std::lock_guard<std::mutex> g(s_cmp_lock);
        s_rb_pool.emplace(a.n, a); s_rb_pool.emplace(e.n, e);
    });
}

// ---- R5: asp's draws (DOTHEROES_ASP_NATIVE=1, development) -----------------------------------
// asp's records drawn natively: its colour and depth targets in the native
// registries, its vertices, indices and textures read from guest memory on
// this thread (docs/NATIVE_R5.md section 4). The engine draws them too until
// asp's writers are bypassed.

int asp_native()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_NATIVE"); on = e && *e == '1'; }
    return on;
}

// A fast 64-bit hash of guest bytes (the shape of xxHash64's lanes)
uint64_t fast_hash(const uint8_t* p, size_t n, uint64_t seed)
{
    const uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull;
    auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    uint64_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4]; memcpy(w, p + i, 32);
        a = rotl(a + w[0] * P2, 31) * P1; b = rotl(b + w[1] * P2, 31) * P1;
        c = rotl(c + w[2] * P2, 31) * P1; d = rotl(d + w[3] * P2, 31) * P1;
    }
    uint64_t h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18) + n;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h ^= rotl(w * P2, 31) * P1; h = rotl(h, 27) * P1 + P2; }
    for (; i < n; i++) { h ^= p[i] * P1; h = rotl(h, 11) * P2; }
    return h;
}

// Guest memory as the vertex fetch reads it: local (location 0) or main
const uint8_t* guest_ptr_live(void*, uint32_t location, uint32_t offset, uint32_t n)
{
    const uint32_t ea = cellGcmResolveLocated(location == 0, offset);
    if (!ea || (uint64_t)ea + n > 0xE0000000ull) return nullptr;
    return vm_base + ea;
}

std::unordered_map<uint64_t, Img*> s_depths;   // asp's depth surfaces by address
VkFormat s_d24s8 = VK_FORMAT_UNDEFINED;

// The engine's own choice (rsx_vulkan_backend.c): one host depth format for
// every guest depth surface, D32_SFLOAT_S8_UINT first -- so that asp's depth
// tests resolve as the engine's do (the comparisons are exact)
VkFormat depth_vkformat(uint32_t depth_format)
{
    (void)depth_format;
    if (s_d24s8 == VK_FORMAT_UNDEFINED) {
        const VkFormat cand[] = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };
        for (VkFormat c : cand) {
            VkFormatProperties fp;
            F.GetPhysicalDeviceFormatProperties(D.phys, c, &fp);
            if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) { s_d24s8 = c; break; }
        }
        if (s_d24s8 == VK_FORMAT_UNDEFINED) s_d24s8 = VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return s_d24s8;
}

Img* depth_image(uint32_t loc, uint32_t off, VkFormat fmt, uint32_t w, uint32_t h)
{
    Img*& o = s_depths[akey(loc, off)];
    if (o && (o->fmt != fmt || o->w < w || o->h < h)) { s_retired.push_back(o); stats().n_retired++; o = nullptr; }
    if (!o) o = make_depth_image(fmt, w, h);
    return o;
}

// A texture asp samples from guest memory (static, mostly: DXT and
// swizzled ones), cached by its description and checked against the guest
// bytes by a hash once a frame
struct GuestTex { Img* img; uint64_t hash; uint32_t frame; };
uint64_t range_hash(const uint8_t* p, uint32_t len, const Rec* r);
std::unordered_map<uint64_t, GuestTex> s_gtex;

Img* guest_texture_live(Batch& b, const Rec* r, const Texture& t)
{
    if (t.cubemap || t.dimension == 3) return nullptr;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const uint32_t nlv = rsx_texture_mip_chain(t.format, t.width, t.height, t.mipmap, t.pitch, lv);
    if (!nlv) return nullptr;
    const uint32_t bytes = lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes;
    const uint32_t ea = cellGcmResolveLocated(t.location == 0, t.offset);
    if (!ea || (uint64_t)ea + bytes > 0xE0000000ull || bytes > (64u << 20)) return nullptr;
    const uint32_t key[] = { t.location, t.offset, t.format, t.width, t.height, nlv, t.pitch };
    const uint64_t k = fnv(key, sizeof key);
    GuestTex& g = s_gtex[k];
    if (g.img && g.frame == r->frame) return g.img;
    const uint64_t h = range_hash(vm_base + ea, bytes, r) ^ k;
    if (g.img && g.hash == h) { g.frame = r->frame; return g.img; }
    uint32_t bpp;
    const VkFormat vf = be_vkformat(texfmt_be(lv[0].tl.fmt), &bpp);
    if (vf == VK_FORMAT_UNDEFINED) return nullptr;
    if (g.img && (g.img->fmt != vf || g.img->w != t.width || g.img->h != t.height || g.img->levels != nlv)) {
        s_retired.push_back(g.img); g.img = nullptr;
    }
    if (!g.img && !(g.img = make_image(vf, t.width, t.height, nlv, bpp, false))) return nullptr;
    access(b.cb, g.img, kCopyDst);
    std::vector<uint8_t> dec;
    for (uint32_t m = 0; m < nlv; m++) {
        const rsx_tex_layout* tl = &lv[m].tl;
        dec.assign((size_t)tl->dst_row_bytes * tl->rows, 0);
        rsx_texture_decode(dec.data(), tl->dst_row_bytes, vm_base + ea + lv[m].offset, lv[m].w, lv[m].h, tl,
                           rsx_texture_argb_is_rgba());
        const VkDeviceSize at = upload(b, dec.data(), dec.size(), 16);
        if (at == ~(VkDeviceSize)0) { g.frame = ~0u; return nullptr; }   // the batch is full: again next time
        VkBufferImageCopy c = {};
        c.bufferOffset = at; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
        c.imageExtent = { lv[m].w, lv[m].h, 1 };
        F.vkCmdCopyBufferToImage(b.cb, b.up.b, g.img->img, layout_of(g.img), 1, &c);
    }
    mark(b, kSectCopy);
    g.img->written = 1;
    g.hash = h; g.frame = r->frame;
    stats().n_static_textures++;
    return g.img;
}

// asp's clears before a draw, each on the surface it was made with
void asp_clears(Batch& b, const Rec* r)
{
    for (uint32_t i = 0; i < r->naclear; i++) {
        const Rec::AspClear& k = r->aclear[i];
        const Surface& sf = k.surface;
        if (!sf.width || !sf.height) continue;
        if ((k.mask & 0xF0u) && sf.color_target) {
            uint32_t bpp = 0;
            const VkFormat rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
            Img* t = target_image(sf.color_location[0] ? 1u : 0u, sf.color_offset[0], rt, bpp, sf.width, sf.height);
            if (t) {
                VkClearColorValue cv;
                const bool flt = rt == VK_FORMAT_R16G16B16A16_SFLOAT || rt == VK_FORMAT_R32G32B32A32_SFLOAT;
                if (flt) for (int j = 0; j < 4; j++) cv.float32[j] = k.rgba[j];
                else { cv.float32[0] = (k.argb >> 16 & 0xFF) / 255.0f; cv.float32[1] = (k.argb >> 8 & 0xFF) / 255.0f;
                       cv.float32[2] = (k.argb & 0xFF) / 255.0f; cv.float32[3] = (k.argb >> 24) / 255.0f; }
                access(b.cb, t, kCopyDst);
                VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                F.vkCmdClearColorImage(b.cb, t->img, layout_of(t), &cv, 1, &rg);
                t->written = 1; t->asp_frame = r->frame + 1;
            }
        }
        if ((k.mask & 0x03u) && sf.depth_format) {
            const VkFormat df = depth_vkformat(sf.depth_format);
            Img* d = depth_image(sf.depth_location ? 1u : 0u, sf.depth_offset, df, sf.width, sf.height);
            if (d) {
                VkClearDepthStencilValue dv = { k.depth / 16777215.0f, k.stencil };
                access(b.cb, d, kCopyDst);
                VkImageSubresourceRange rg = { (VkImageAspectFlags)((k.mask & 1u ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                               (k.mask & 2u && has_stencil(df) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1 };
                if (rg.aspectMask) F.vkCmdClearDepthStencilImage(b.cb, d->img, layout_of(d), &dv, 1, &rg);
                d->written = 1;
            }
        }
        mark(b, kSectOther);
        stats().n_asp_clears++;
    }
}

// DOTHEROES_ASP_DESCRIBE_FRAME=<frame> (a diagnostic): a line per asp draw
// of that frame -- what it draws with -- into the dump directory
void asp_describe_line(const Rec* r)
{
    static long long want = -2;
    static FILE* f = nullptr;
    if (want == -2) { const char* e = getenv("DOTHEROES_ASP_DESCRIBE_FRAME"); want = e && *e ? atoll(e) : -1; }
    if (want < 0 || (long long)r->frame != want || !s_dump[0]) { if (f && (long long)r->frame > want) { fclose(f); f = nullptr; want = -1; } return; }
    if (!f) {
        char path[600];
        snprintf(path, sizeof path, "%s/describe_f%u.txt", s_dump, r->frame);
        if (!(f = fopen(path, "w"))) { want = -1; return; }
    }
    const State& st = r->st;
    const Raster& ra = st.raster;
    fprintf(f, "%llu prim %u n %u v %u | tgt %08X fmt %u dz %08X | fp %016llx | blend %u %X/%X %X/%X eq %X/%X | alpha %u %X %u | "
               "depth %u/%u %X | stencil %u %X ref %u mask %X/%X ops %X/%X/%X | cull %u %X | cmask %08X | clears %u | tex",
            (unsigned long long)r->seq, r->prim, r->count, r->vcount, st.surface.color_offset[0], st.surface.color_format,
            st.surface.depth_offset, r->fp ? (unsigned long long)r->fp->hash : 0ull, ra.blend, ra.blend_sf_rgb, ra.blend_df_rgb,
            ra.blend_sf_a, ra.blend_df_a, ra.blend_eq_rgb, ra.blend_eq_a, ra.alpha_test, ra.alpha_func, ra.alpha_ref,
            ra.depth_test, ra.depth_write, ra.depth_func, ra.stencil_test, ra.stencil_func, ra.stencil_ref, ra.stencil_func_mask,
            ra.stencil_write_mask, ra.stencil_fail, ra.stencil_zfail, ra.stencil_zpass, ra.cull, ra.cull_face, ra.color_mask,
            r->naclear);
    for (uint32_t u = 0; u < 16; u++)
        if (st.samp[u].enable) fprintf(f, " %u:%02X@%08X", u, st.tex[u].format, st.tex[u].offset);
    fprintf(f, "\n");
    for (uint32_t i = 0; i < r->naclear; i++)
        fprintf(f, "  clear %u: mask %X depth %06X stencil %X argb %08X on %08X/%08X\n", i, r->aclear[i].mask, r->aclear[i].depth,
                r->aclear[i].stencil, r->aclear[i].argb, r->aclear[i].surface.color_offset[0], r->aclear[i].surface.depth_offset);
}

// R5: asp's converted vertices and rebuilt index lists, kept on the GPU
// across draws and frames (DOTHEROES_ASP_VCACHE=0 turns it off). An entry is
// found by what it is made from -- the streams' descriptions, the vertex
// count, the primitive -- and a hash of the guest bytes it is made from,
// computed at every use: a rewrite of the game's data is a different entry.
// Entries live in arenas of video memory the CPU writes; past the budget the
// oldest arena goes whole (its entries are gone with it), destroyed when the
// batch then being recorded has completed.
struct Arena { Buf b; VkDeviceSize used = 0; uint32_t gen = 0; };
std::deque<Arena> s_arenas;
uint32_t s_arena_gen = 0;
const VkDeviceSize kArena = 64u << 20;
const size_t kArenas = 8;   // 512 MB
struct CacheEnt { uint32_t gen; VkDeviceSize off; VkDeviceSize bytes; uint32_t count; };
std::unordered_map<uint64_t, CacheEnt> s_vcache;
// admission: by where the data comes from (without its content), the hash
// it had the last time -- an entry is made only when the same place holds
// the same bytes a second time, so data the game rewrites every frame
// (debris, effects) goes through the batch's upload instead of filling the
// arenas and pushing the static meshes out
std::unordered_map<uint64_t, uint64_t> s_vseen;
std::atomic<uint64_t> s_vc_uncached{0}, s_vc_direct{0}, s_vc_direct_bytes{0}, s_vc_direct_max{0};
std::atomic<uint64_t> s_vc_hits{0}, s_vc_misses{0}, s_ic_hits{0}, s_ic_misses{0}, s_vc_hashed{0}, s_arenas_retired{0};

int asp_vcache()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_VCACHE"); on = !(e && *e == '0'); }
    return on;
}

// a cached entry's buffer, or null when its arena is gone
const Arena* arena_of(const CacheEnt& e)
{
    for (const Arena& a : s_arenas) if (a.gen == e.gen) return &a;
    return nullptr;
}

// An arena: video memory only (the CPU's window into video memory is small
// without resizable BAR, and shared with the batches' uploads)
bool make_device_buffer(Buf& o, VkDeviceSize n, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = n; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (F.vkCreateBuffer(D.device, &ci, nullptr, &o.b) != VK_SUCCESS) return false;
    VkMemoryRequirements rq;
    F.vkGetBufferMemoryRequirements(D.device, o.b, &rq);
    const uint32_t t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size; ai.memoryTypeIndex = t;
    if (t == ~0u || F.vkAllocateMemory(D.device, &ai, nullptr, &o.m) != VK_SUCCESS) {
        F.vkDestroyBuffer(D.device, o.b, nullptr); o.b = VK_NULL_HANDLE; return false;
    }
    F.vkBindBufferMemory(D.device, o.b, o.m, 0);
    o.p = nullptr; o.n = n;
    return true;
}

// room for n bytes; the arena and its offset
Arena* arena_alloc(Batch& b, VkDeviceSize n, VkDeviceSize* off)
{
    if (n > kArena) return nullptr;
    if (s_arenas.empty() || ((s_arenas.back().used + 255) & ~(VkDeviceSize)255) + n > kArena) {
        Arena a;
        if (!make_device_buffer(a.b, kArena, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            static int said = 0;
            if (!said++) report_add("render: asp's vertex cache: an arena could not be made");
            return nullptr;
        }
        a.gen = ++s_arena_gen;
        s_arenas.push_back(a);
        static size_t keep = 0;
        if (!keep) { const char* e = getenv("DOTHEROES_R6_ARENAS"); keep = e && atoi(e) > 0 ? (size_t)atoi(e) : kArenas; }
        if (s_arenas.size() > keep) {   // R6: DOTHEROES_R6_ARENAS=<n> (a lifetime test) keeps fewer
            Buf old = s_arenas.front().b;
            s_arenas.pop_front();
            s_arenas_retired++;
            b.done.push_back([old] { F.vkDestroyBuffer(D.device, old.b, nullptr); F.vkFreeMemory(D.device, old.m, nullptr); });
        }
    }
    Arena& a = s_arenas.back();
    *off = (a.used + 255) & ~(VkDeviceSize)255;
    a.used = *off + n;
    return &a;
}

// The arena's bytes at off come from the batch's upload buffer at src: a
// copy, and a barrier before any later vertex or index read of them
void arena_fill(Batch& b, const Arena& a, VkDeviceSize off, VkDeviceSize src, VkDeviceSize n)
{
    VkBufferCopy c = { src, off, n };
    F.vkCmdCopyBuffer(b.cb, b.up.b, a.b.b, 1, &c);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    F.vkCmdPipelineBarrier(b.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    mark(b, kSectCopy);
}

// DOTHEROES_ASP_PROFILE=1: asp_draw's time by part, on the native thread
int asp_profile()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_PROFILE"); on = e && *e == '1'; }
    return on;
}
enum { kApClears, kApLayout, kApIndices, kApPlan, kApVcache, kApPipeline, kApTextures, kApDescriptors, kApCommands,
       kApHash, kApN };
const char* const kApName[kApN] = { "clears", "layout", "indices", "fetch plan", "vertex cache", "pipeline", "textures",
                                    "uploads and descriptors", "commands", "(of the cache: hashing)" };
uint64_t s_ap_ns[kApN], s_ap_draws;
struct AspProf {
    uint64_t t = 0;
    bool on;
    AspProf() : on(asp_profile() != 0) { if (on) { t = now_ns(); s_ap_draws++; } }
    void mark(int i) { if (on) { const uint64_t n = now_ns(); s_ap_ns[i] += n - t; t = n; } }
};

// Guest writes, from the OS's write watch on the guest space (port/main.cpp
// reserves it with MEM_WRITE_WATCH when asp is drawn natively): a poll
// collects the 4 KB pages written since the previous one and stamps each
// with the poll's number. A range hashed after poll E is unchanged while
// none of its pages carries a later stamp. A poll sees every write made
// before it, so it covers the records published before it began (their
// vertices were written before the game drew); a record published later
// waits for the next poll -- at most one a millisecond -- or is hashed.
extern "C" int vm_write_watch;
std::vector<uint32_t> s_pg_epoch;   // per 4 KB guest page: the poll that last found it written
std::vector<void*> s_ww_buf;
uint32_t s_ww_epoch = 0;            // the polls so far
uint64_t s_ww_cover = 0;            // records below this seq were published before the last poll
uint64_t s_ww_last_ns = 0, s_ww_polls = 0, s_ww_ns = 0, s_ww_pages = 0, s_ww_failed = 0, s_ww_scanned = 0;
// the guest pages relied on, as runs [first, end) -- runs closer than
// kWwGap pages merged (a poll's cost is per page scanned and per call)
std::map<uint32_t, uint32_t> s_ww_runs;
const uint32_t kWwGap = 32;

int ww_poll_us()
{
    static int us = -1;
    if (us < 0) { const char* e = getenv("DOTHEROES_ASP_WWATCH_US"); us = e && *e ? atoi(e) : 1000; }
    return us;
}

void ww_note(uint32_t ea, uint32_t len)
{
    uint32_t a = ea >> 12, b = (uint32_t)(((uint64_t)ea + len + 0xFFFu) >> 12);
    // merge every run within kWwGap of [a, b)
    auto it = s_ww_runs.upper_bound(a);
    if (it != s_ww_runs.begin()) {
        auto pv = std::prev(it);
        if (pv->second + kWwGap >= a) it = pv;
    }
    while (it != s_ww_runs.end() && it->first <= b + kWwGap) {
        if (it->second + kWwGap < a) { ++it; continue; }
        if (it->first < a) a = it->first;
        if (it->second > b) b = it->second;
        it = s_ww_runs.erase(it);
    }
    s_ww_runs[a] = b;
}

bool ww_poll()
{
    if (!vm_write_watch) return false;
    if (s_pg_epoch.empty()) { s_pg_epoch.assign(1u << 20, 0); s_ww_buf.resize(1u << 20); }
    const uint64_t t0 = now_ns();
    const uint64_t cover = queue().put.load(std::memory_order_acquire);   // before the poll
    s_ww_epoch++;
    for (const auto& run : s_ww_runs) {
        // the output array sized to the run: the call's cost follows the
        // array's size, not the span's (0.7 ms for a million entries)
        ULONG_PTR n = run.second - run.first;
        DWORD gran = 0;
        s_ww_scanned += run.second - run.first;
        if (n > s_ww_buf.size() ||
            GetWriteWatch(WRITE_WATCH_FLAG_RESET, vm_base + ((uint64_t)run.first << 12), (SIZE_T)(run.second - run.first) << 12,
                          s_ww_buf.data(), &n, &gran) != 0) {
            vm_write_watch = 0;   // the write watch failed: everything hashed from now on
            s_ww_failed++;
            return false;
        }
        for (ULONG_PTR i = 0; i < n; i++) {
            const uint64_t pg = (uint64_t)((uint8_t*)s_ww_buf[i] - vm_base) >> 12;
            if (pg < s_pg_epoch.size()) s_pg_epoch[pg] = s_ww_epoch;
        }
        s_ww_pages += n;
    }
    s_ww_cover = cover;
    s_ww_polls++;
    s_ww_last_ns = now_ns();
    s_ww_ns += s_ww_last_ns - t0;
    return true;
}

// the record's writes seen by a poll: it is covered, or a poll is due
bool ww_covers(uint64_t seq)
{
    if (!vm_write_watch) return false;
    if (seq < s_ww_cover) return true;
    if (now_ns() - s_ww_last_ns < (uint64_t)ww_poll_us() * 1000u) return false;
    return ww_poll() && seq < s_ww_cover;
}

bool ww_clean(uint32_t ea, uint32_t len, uint32_t epoch)
{
    for (uint32_t pg = ea >> 12, e = (uint32_t)(((uint64_t)ea + len - 1) >> 12); pg <= e; pg++)
        if (s_pg_epoch[pg] > epoch) return false;
    return true;
}

// The hash of a guest range's bytes. Unchanged since it was last hashed
// (the write watch above): the same hash, nothing read. Otherwise once a
// frame: asp draws a mesh in several passes a frame (the shadow, the
// reflection, the scene), and a later use in the same frame is checked by
// a sample of the bytes (eight words across the range); a range the game
// rewrote between two draws of a frame -- its sample changed -- is hashed
// again.
struct RangeHash { uint64_t h, sample; uint32_t frame, epoch; bool ww; };
std::unordered_map<uint64_t, RangeHash> s_rhash;
uint64_t s_rh_memo_bytes = 0, s_rh_ww_bytes = 0, s_rh_rehashed = 0;

uint64_t sample_hash(const uint8_t* p, uint32_t len)
{
    if (len < 64) return fast_hash(p, len, 0x5A3Dull);
    uint64_t h = len;
    for (uint32_t i = 0; i < 8; i++) {
        uint64_t w;
        memcpy(&w, p + (uint64_t)(len - 8) * i / 7, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

uint64_t range_hash(const uint8_t* p, uint32_t len, const Rec* r)
{
    const uint32_t ea = (uint32_t)(p - vm_base);
    const uint32_t kk[2] = { ea, len };
    const uint64_t key = fnv(kk, sizeof kk);
    if (s_rhash.size() > 300000) { s_rhash.clear(); s_ww_runs.clear(); }
    RangeHash& e = s_rhash[key];
    if (e.ww && ww_covers(r->seq) && ww_clean(ea, len, e.epoch)) { s_rh_ww_bytes += len; e.frame = r->frame + 1; return e.h; }
    const uint64_t sm = sample_hash(p, len);
    if (e.frame == r->frame + 1 && e.sample == sm) { s_rh_memo_bytes += len; return e.h; }
    if (e.frame == r->frame + 1) s_rh_rehashed++;
    e.h = fast_hash(p, len, key);
    e.sample = sm;
    e.frame = r->frame + 1;
    e.ww = vm_write_watch != 0;
    e.epoch = s_ww_epoch;
    if (e.ww) ww_note(ea, len);
    s_vc_hashed += len;
    return e.h;
}

// An index list's referenced vertices [vmin, vmax], by its bytes' hash: an
// unchanged list is not decoded again
struct IdxSum { uint32_t vmin, vmax; };
std::unordered_map<uint64_t, IdxSum> s_isum;

void asp_draw(const Rec* r)
{
    AspProf prof;
    Batch& b = batch(r->frame);
    wait_for(b, r);
    asp_describe_line(r);
    asp_clears(b, r);
    prof.mark(kApClears);
    if (r->why || !r->vp || !r->fp) { unsupported(r, "asp: no programs, or no draw object"); return; }
    const State& st = r->st;
    const Surface& sf = st.surface;
    if (sf.color_target > 1 || sf.antialias) { unsupported(r, "asp: MRT or antialiased target"); return; }
    const uint32_t W = sf.width, H = sf.height;
    if (!W || !H) { unsupported(r, "asp: no clip"); return; }
    // its targets
    Img* t = nullptr; VkFormat rt = VK_FORMAT_UNDEFINED; uint32_t bpp = 0;
    if (sf.color_target) {
        rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
        t = target_image(sf.color_location[0] ? 1u : 0u, sf.color_offset[0], rt, bpp, W, H);
        if (!t) { unsupported(r, "asp: target image"); return; }
        if (sf.color_location[0]) { t->main_dirty = 1; t->guest_pitch = sf.color_pitch[0]; }   // R6: written back
    }
    const Raster& ra = st.raster;
    Img* d = nullptr; VkFormat df = VK_FORMAT_UNDEFINED;
    if (sf.depth_format && (ra.depth_test || ra.stencil_test)) {
        df = depth_vkformat(sf.depth_format);
        d = depth_image(sf.depth_location ? 1u : 0u, sf.depth_offset, df, W, H);
        if (!d) { unsupported(r, "asp: depth image"); return; }
    }
    if (!t && !d) { unsupported(r, "asp: neither a colour nor a depth target"); return; }
    // the vertex layout, the vertices [0, vcount), the indices
    rsx_vertex_layout_plan layout;
    {
        const uint64_t vkey = r->vp->hash ^ ((uint64_t)r->vp->bytes.size() << 40);
        auto mt = s_vpmask.find(vkey);
        uint32_t mask = 0xFFFFu;
        if (mt != s_vpmask.end()) mask = mt->second;
        else {
            rsx_vp_input_analysis an = { 0xFFFFu, 0 };
            const uint32_t instrs = (uint32_t)r->vp->bytes.size() / 16;
            if (rsx_vp_analyze_inputs(r->vp->bytes.data(), instrs * 16u, &an) == (int)instrs && an.exact && an.input_mask)
                mask = an.input_mask;
            if (s_vpmask.size() > 8192) s_vpmask.clear();
            s_vpmask[vkey] = mask;
        }
        rsx_vertex_layout_plan_init(&layout, mask);
    }
    prof.mark(kApLayout);
    int indexed = 0;
    if (!rsx_vertex_topology_plan(r->prim, 1, &indexed)) { unsupported(r, "asp: a primitive the engine does not rebuild"); return; }
    const uint32_t nv = r->vcount, n = r->count;
    if (!nv || !n || nv > (1u << 20) || n > (1u << 22)) { unsupported(r, "asp: counts"); return; }
    const bool u16 = r->idx_type == 2;
    const uint32_t ibytes_guest = n * (u16 ? 2u : 4u);
    if (r->idx_ea) {
        const uint64_t ib = (uint64_t)r->idx_ea + ibytes_guest;
        if (r->idx_ea < 0x10000u || ib > 0xE0000000ull) { unsupported(r, "asp: index buffer"); return; }
    } else if (n > nv) { unsupported(r, "asp: more vertices drawn than the object has"); return; }
    // the vertices the draw references, [vmin, vmax]; the indices, rebased to
    // vmin, decoded only when they are needed (not when the list is cached)
    static std::vector<uint32_t> occ;   // the native thread's own
    uint32_t vmin = 0, vmax = n - 1;
    bool decoded = false, have_sum = false;
    auto decode = [&]() -> bool {
        occ.resize(n);
        if (r->idx_ea) {
            const uint8_t* ip = vm_base + r->idx_ea;
            uint32_t lo = ~0u, hi = 0;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t v = u16 ? (uint32_t)(ip[2 * i] << 8 | ip[2 * i + 1])
                                       : (uint32_t)ip[4 * i] << 24 | (uint32_t)ip[4 * i + 1] << 16 | (uint32_t)ip[4 * i + 2] << 8 | ip[4 * i + 3];
                if (v >= nv) return false;
                occ[i] = v;
                lo = v < lo ? v : lo; hi = v > hi ? v : hi;
            }
            if (have_sum && (lo != vmin || hi != vmax)) return false;   // rewritten since it was hashed
            vmin = lo; vmax = hi;
            if (vmin) for (uint32_t i = 0; i < n; i++) occ[i] -= vmin;
        } else {
            for (uint32_t i = 0; i < n; i++) occ[i] = i;
        }
        decoded = true;
        return true;
    };
    uint64_t ih = 0;   // the index bytes' hash (with the vertex cache)
    if (!asp_vcache() || !r->idx_ea) {
        if (!decode()) { unsupported(r, "asp: an index past the vertex count"); return; }
    } else {
        ih = range_hash(vm_base + r->idx_ea, ibytes_guest, r);
        const uint32_t sv[3] = { n, nv, (uint32_t)u16 };
        const uint64_t sk = fnv(sv, sizeof sv, ih);
        auto it = s_isum.find(sk);
        if (it != s_isum.end()) { vmin = it->second.vmin; vmax = it->second.vmax; have_sum = true; }
        else {
            if (!decode()) { unsupported(r, "asp: an index past the vertex count"); return; }
            if (s_isum.size() > 200000) s_isum.clear();
            s_isum[sk] = { vmin, vmax };
        }
    }
    const uint32_t nref = vmax - vmin + 1;
    prof.mark(kApIndices);
    if (r->src_hash) {   // DOTHEROES_ASP_RACE_CHECK: the same bytes as when the game drew it?
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < r->nvr; i++) {
            const uint8_t* p = guest_ptr_live(nullptr, r->vr[i].location, r->vr[i].offset, r->vr[i].bytes);
            if (p) h = fnv(p, r->vr[i].bytes, h);
        }
        if (r->idx_ea) h = fnv(vm_base + r->idx_ea, (size_t)n * (r->idx_type == 2 ? 2 : 4), h);
        stats().asp_race_checked++;
        if ((h | 1) != r->src_hash) {
            stats().asp_race_changed++;
            static int said = 0;
            if (said++ < 12) {
                char line[200];
                snprintf(line, sizeof line, "asp race: record %llu frame %u: its bytes changed before the native draw (%u streams, %u vertices)",
                         (unsigned long long)r->seq, r->frame, r->nvr, nv);
                report_add(line);
            }
        }
    }
    const uint32_t n_draw = rsx_draw_engine_topology_index_count(r->prim, n, nullptr, 0);
    rsx_vertex_fetch_plan plan;
    fetch_plan(&plan, r, layout);
    plan.guest_ptr = guest_ptr_live; plan.guest_user = nullptr;
    prof.mark(kApPlan);
    // the vertices: from the cache when the same streams hold the same bytes
    VkBuffer vbuf = VK_NULL_HANDLE, ibuf = VK_NULL_HANDLE;
    VkDeviceSize vofs = 0, iofs = 0;
    std::vector<uint8_t> verts;
    std::vector<uint32_t> idx;
    const VkDeviceSize vbytes = (VkDeviceSize)nref * layout.stride;
    if (asp_vcache()) {
        uint64_t k = fnv(&layout, sizeof layout, 0x5EEDull);
        k = fnv(&nv, 4, k);
        for (uint32_t a = 0; a < 16; a++) {
            if (!(layout.mask >> a & 1)) continue;
            const rsx_vertex_fetch_attr& f = plan.attr[a];
            k = fnv(&f.desc, sizeof f.desc, k);
            k = fnv(f.default_value, sizeof f.default_value, k);
        }
        k = fnv(&plan.base_offset, 4, k);
        k = fnv(&vmin, 4, fnv(&nref, 4, k));
        uint64_t place = k;   // the place: k before the bytes are hashed in
        // the guest bytes: each stream's referenced range, hashed (ranges of one interleaved buffer once)
        uint32_t lo[16], hi[16], loc[16], nr = 0;
        for (uint32_t a = 0; a < 16; a++) {
            const VertexInput& in = r->st.vin[a];
            if (!in.size) continue;
            const uint32_t base = (r->st.vdata.base_offset + (in.offset & 0x7FFFFFFFu)) & 0x0FFFFFFFu;
            const uint32_t es = rsx_vertex_attrib_size(in.type & 7u, in.size), stride = in.stride ? in.stride : es;
            const uint32_t vlo = base + vmin * stride, vhi = base + vmax * stride + es;
            bool merged = false;
            for (uint32_t j = 0; j < nr && !merged; j++)
                if (loc[j] == in.location && vlo <= hi[j] && vhi >= lo[j]) {
                    lo[j] = lo[j] < vlo ? lo[j] : vlo; hi[j] = hi[j] > vhi ? hi[j] : vhi;
                    merged = true;
                }
            if (!merged && nr < 16) { lo[nr] = vlo; hi[nr] = vhi; loc[nr] = in.location; nr++; }
        }
        const uint64_t th0 = prof.on ? now_ns() : 0;
        for (uint32_t j = 0; j < nr; j++) {
            const uint8_t* p = guest_ptr_live(nullptr, loc[j], lo[j], hi[j] - lo[j]);
            if (!p) { unsupported(r, "asp: vertex range"); return; }
            const uint64_t rh = range_hash(p, hi[j] - lo[j], r);
            k = fnv(&rh, 8, k);
            place = fnv(&lo[j], 4, fnv(&hi[j], 4, fnv(&loc[j], 4, place)));
        }
        if (prof.on) s_ap_ns[kApHash] += now_ns() - th0;
        auto it = s_vcache.find(k);
        const Arena* ar = it != s_vcache.end() ? arena_of(it->second) : nullptr;
        uint64_t& seen = s_vseen[place];
        const bool admit = seen == k;
        seen = k;
        if (ar) { vbuf = ar->b.b; vofs = it->second.off; s_vc_hits++; }
        else if (!admit) s_vc_uncached++;
        else {
            VkDeviceSize off = 0, src = ~(VkDeviceSize)0;
            if (b.used + vbytes + 512 < kUpload / 2) src = (b.used + 15) & ~(VkDeviceSize)15;   // the staging place, aligned
            Arena* na = src != ~(VkDeviceSize)0 ? arena_alloc(b, vbytes, &off) : nullptr;
            if (na) {
                std::vector<rsx_vertex_ref> refs(nref);
                for (uint32_t i = 0; i < nref; i++) refs[i] = { vmin + i, 0 };
                rsx_vertex_fetch_plan_prepare(&plan, refs.data(), nref);
                if (!rsx_vertex_fetch_many(&plan, refs.data(), nref, b.up.p + src)) { unsupported(r, "asp: vertex fetch"); return; }
                b.used = src + vbytes;
                arena_fill(b, *na, off, src, vbytes);
                s_vcache[k] = { na->gen, off, vbytes, nref };
                vbuf = na->b.b; vofs = off;
                s_vc_misses++;
                stats().n_asp_vertex_bytes += vbytes;
            }
        }
        // the index list, rebuilt as triangles: by the index bytes, the primitive and counts
        uint64_t ik = fnv(&r->prim, 4, 0x1D7ull);
        ik = fnv(&n, 4, ik); ik = fnv(&vmin, 4, fnv(&nref, 4, ik));
        ik = r->idx_ea ? fnv(&ih, 8, ik ^ r->idx_ea ^ ((uint64_t)r->idx_type << 40)) : fnv("arrays", 6, ik);
        auto jt = s_vcache.find(ik);
        const Arena* ia = jt != s_vcache.end() ? arena_of(jt->second) : nullptr;
        uint64_t iplace = fnv(&r->idx_ea, 4, fnv(&n, 4, fnv(&r->prim, 4, 0x1D8ull)));
        uint64_t& iseen = s_vseen[iplace];
        const bool iadmit = iseen == ik;
        iseen = ik;
        if (ia) { ibuf = ia->b.b; iofs = jt->second.off; s_ic_hits++; }
        else if (!iadmit) s_vc_uncached++;
        else {
            const VkDeviceSize ibytes = (VkDeviceSize)(n_draw ? n_draw : 1) * 4;
            VkDeviceSize off = 0, src = ~(VkDeviceSize)0;
            if (b.used + ibytes + 512 < kUpload / 2) src = (b.used + 15) & ~(VkDeviceSize)15;
            Arena* na = src != ~(VkDeviceSize)0 ? arena_alloc(b, ibytes, &off) : nullptr;
            if (na && !decoded && !decode()) { unsupported(r, "asp: an index past the vertex count"); return; }
            if (na) {
                rsx_draw_engine_write_topology_indices(r->prim, n, nullptr, 0, occ.data(), (uint32_t*)(b.up.p + src));
                b.used = src + ibytes;
                arena_fill(b, *na, off, src, ibytes);
                s_vcache[ik] = { na->gen, off, ibytes, n_draw };
                ibuf = na->b.b; iofs = off;
                s_ic_misses++;
            }
        }
        if (s_vcache.size() > 400000) s_vcache.clear();   // bounded: everything again from the guest
        if (s_vseen.size() > 400000) s_vseen.clear();
    }
    if (!vbuf) {
        s_vc_direct++; s_vc_direct_bytes += vbytes;
        if (vbytes > s_vc_direct_max) s_vc_direct_max = vbytes;
        std::vector<rsx_vertex_ref> refs(nref);
        for (uint32_t i = 0; i < nref; i++) refs[i] = { vmin + i, 0 };
        rsx_vertex_fetch_plan_prepare(&plan, refs.data(), nref);
        verts.resize((size_t)vbytes);
        if (!rsx_vertex_fetch_many(&plan, refs.data(), nref, verts.data())) { unsupported(r, "asp: vertex fetch"); return; }
        stats().n_asp_vertex_bytes += vbytes;
    }
    if (!ibuf) {
        if (!decoded && !decode()) { unsupported(r, "asp: an index past the vertex count"); return; }
        idx.resize(n_draw ? n_draw : 1);
        rsx_draw_engine_write_topology_indices(r->prim, n, nullptr, 0, occ.data(), idx.data());
    }
    prof.mark(kApVcache);
    // the pipeline
    rsx_fp_constant_block fc;
    const Pipe* P = pipeline_asp(r, layout, rt, df, &fc);
    if (!P) { unsupported(r, "asp: pipeline"); return; }
    prof.mark(kApPipeline);
    // constants: the vertex program's and the viewport epilogue; the fragment
    // program's buffered constants and the alpha reference and window slot
    const Viewport& vp = st.viewport;
    const float Wf = (float)W, Hf = (float)H;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.offset[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wf * 0.5f);
        xf[1] = -(vp.scale[1] / (Hf * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.offset[0] - Wf * 0.5f) / (Wf * 0.5f);
        xf[5] = -((vp.offset[1] - Hf * 0.5f) / (Hf * 0.5f));
        xf[6] = vp.offset[2];
    }
    uint8_t vcb[512 * 16 + 32];
    memcpy(vcb, s_vc, 512 * 16);
    memcpy(vcb + 512 * 16, xf, sizeof xf);
    const uint32_t nslots = fc.count ? fc.count : 1u;
    std::vector<uint8_t> pcb((nslots + 1) * 16, 0);
    if (fc.count) memcpy(pcb.data(), fc.values, fc.count * 16u);
    {
        float* alpha = (float*)(pcb.data() + nslots * 16);
        alpha[0] = ra.alpha_test ? ra.alpha_ref / 255.0f : 0.0f;
        alpha[1] = (float)(sf.height & 0xFFFu);
        alpha[2] = 0.0f; alpha[3] = 0.0f;
    }
    // textures: a native image of the address (a target asp or PPFX drew, a
    // depth surface), else guest memory
    VkDescriptorImageInfo ii[16], si[16];
    const uint32_t sampled = fp_info(r->fp.get()).sampled;
    Img* srcs[16] = {};
    VkImageView views[16] = {};
    for (uint32_t u = 0; u < 16; u++) {
        if (!(sampled >> u & 1) || !st.samp[u].enable) continue;
        const Texture& tx = st.tex[u];
        const uint64_t key = akey(tx.location ? 1u : 0u, tx.offset);
        Img* src = nullptr;
        auto dt = s_depths.find(key);
        auto ct = s_targets.find(key);
        if (dt != s_depths.end() && dt->second && dt->second->written) { src = dt->second; views[u] = depth_sample_view(src); }
        else if (ct != s_targets.end() && ct->second->written) {
            src = ct->second;
            if (src == t) { unsupported(r, "asp: a draw sampling its own target"); return; }
            views[u] = view_of(src, tx.remap & 0xFFFFu, tx.format);
        } else if ((src = static_texture(b, r, u, tx)) || (src = guest_texture_live(b, r, tx)))
            views[u] = view_of(src, tx.remap & 0xFFFFu, tx.format);
        if (!src) {
            char why[200];
            snprintf(why, sizeof why, "asp: unit %u (format %02X %ux%u mips %u dim %u cube %u) has no source at %u:%08X", u,
                     tx.format, tx.width, tx.height, tx.mipmap, tx.dimension, tx.cubemap, tx.location, tx.offset);
            unsupported(r, why);
            return;
        }
        srcs[u] = src;
    }
    prof.mark(kApTextures);
    for (uint32_t u = 0; u < 16; u++) if (srcs[u]) access(b.cb, srcs[u], kSampled);
    for (uint32_t u = 0; u < 16; u++) {
        ii[u] = { VK_NULL_HANDLE, s_dummy->view, sampled_layout() };
        si[u] = { s_dummy_smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        if (!srcs[u]) continue;
        ii[u].imageView = views[u];
        si[u].sampler = sampler(st.samp[u]);
    }
    const VkDeviceSize o_vc = upload(b, vcb, sizeof vcb), o_pc = upload(b, pcb.data(), pcb.size());
    const VkDeviceSize o_v = vbuf ? vofs : upload(b, verts.data(), verts.size(), 16);
    const VkDeviceSize o_i = ibuf ? iofs : upload(b, idx.data(), idx.size() * 4, 16);
    if (o_vc == ~(VkDeviceSize)0 || o_pc == ~(VkDeviceSize)0 || o_v == ~(VkDeviceSize)0 || o_i == ~(VkDeviceSize)0 ||
        b.sets >= kSets) {
        unsupported(r, "asp: the batch is full");
        return;
    }
    if (!vbuf) vbuf = b.up.b;
    if (!ibuf) ibuf = b.up.b;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = b.dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dsl;
    VkDescriptorSet set;
    if (F.vkAllocateDescriptorSets(D.device, &dai, &set) != VK_SUCCESS) { unsupported(r, "asp: descriptor set"); return; }
    b.sets++;
    VkDescriptorBufferInfo bi[2] = { { b.up.b, o_vc, sizeof vcb }, { b.up.b, o_pc, pcb.size() } };
    VkWriteDescriptorSet wr[4] = {};
    for (int k = 0; k < 4; k++) { wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = set; }
    wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[0].pBufferInfo = &bi[0];
    wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[1].pBufferInfo = &bi[1];
    wr[2].dstBinding = 16; wr[2].descriptorCount = 16; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[2].pImageInfo = ii;
    wr[3].dstBinding = 48; wr[3].descriptorCount = 16; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; wr[3].pImageInfo = si;
    F.vkUpdateDescriptorSets(D.device, 4, wr, 0, nullptr);
    prof.mark(kApDescriptors);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = render_pass2(rt, df); rbi.framebuffer = framebuffer2(t, d);
    const uint32_t fw = t && d ? (t->w < d->w ? t->w : d->w) : t ? t->w : d->w;
    const uint32_t fh = t && d ? (t->h < d->h ? t->h : d->h) : t ? t->h : d->h;
    rbi.renderArea = { { 0, 0 }, { fw, fh } };
    // the previous draw's pass, still open on the same targets (nothing
    // recorded since that had to end it): this draw joins it -- draws into
    // the same attachments within one pass are ordered without barriers
    if (pass_merge() && s_open.cb == b.cb && s_open.rp == rbi.renderPass && s_open.fb == rbi.framebuffer && s_open.w == fw &&
        s_open.h == fh)
        s_pass_merged++;
    else {
        if (t) access(b.cb, t, kAttach);
        if (d) access(b.cb, d, kDepthAttach);
        F.vkCmdBeginRenderPass(b.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);   // ends an open one first
        if (pass_merge()) s_open = { b.cb, rbi.renderPass, rbi.framebuffer, fw, fh };
    }
    F.vkCmdBindPipeline(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->p);
    VkViewport v = { 0.0f, 0.0f, Wf, Hf, 0.0f, 1.0f };
    F.vkCmdSetViewport(b.cb, 0, 1, &v);
    uint32_t sc[4];
    scissor_rect(st.scissor, fw, fh, sc);
    VkRect2D s2 = { { (int32_t)sc[0], (int32_t)sc[1] }, { sc[2], sc[3] } };
    F.vkCmdSetScissor(b.cb, 0, 1, &s2);
    F.vkCmdSetStencilReference(b.cb, VK_STENCIL_FACE_FRONT_AND_BACK, ra.stencil_ref);
    // polygon offset: the guest's units are the depth format's resolution steps, as Vulkan's
    // the engine's scaling (vk_eng_set_depth_bias): units in 2^-24 steps, a Z16 surface's in 2^-16
    if (ra.poly_offset_fill) F.vkCmdSetDepthBias(b.cb, ra.poly_offset_units * (sf.depth_format == 1 ? 256.0f : 1.0f), 0.0f,
                                                 ra.poly_offset_factor);
    else F.vkCmdSetDepthBias(b.cb, 0.0f, 0.0f, 0.0f);
    const float bc[4] = { 0, 0, 0, 0 };   // the blend colour: not described yet (asp's constant-colour blends)
    F.vkCmdSetBlendConstants(b.cb, bc);
    F.vkCmdBindDescriptorSets(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl, 0, 1, &set, 0, nullptr);
    F.vkCmdBindVertexBuffers(b.cb, 0, 1, &vbuf, &o_v);
    F.vkCmdBindIndexBuffer(b.cb, ibuf, o_i, VK_INDEX_TYPE_UINT32);
    occ_begin(b, r);
    F.vkCmdDrawIndexed(b.cb, n_draw, 1, 0, 0, 0);
    occ_end(b);
    if (!pass_merge()) F.vkCmdEndRenderPass(b.cb);   // else left open for the next draw
    mark(b, kSectDraw);
    prof.mark(kApCommands);
    if (t) { t->written = 1; t->asp_frame = r->frame + 1; }
    if (d) d->written = 1;
    b.records++;
    stats().n_asp_draws++;
    if (t) compare_after(b, r, t, W, H);   // DOTHEROES_ASP_COMPARE_DRAWS
}

void clear(const Rec* r)
{
    Batch& b = batch(r->frame);
    wait_for(b, r);
    if (r->naclear) asp_clears(b, r);   // R6: the clears made before it (the game's, asp's)
    const Surface& sf = r->st.surface;
    uint32_t bpp = 0;
    const VkFormat rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
    if (!sf.width || !sf.height || sf.color_target != 1) { unsupported(r, "clear target"); return; }
    apply_handoffs(b, r);
    Img* t = target_image(sf.color_location[0] ? 1u : 0u, sf.color_offset[0], rt, bpp, sf.width, sf.height);
    if (!t) { unsupported(r, "clear target image"); return; }
    const uint32_t c = r->st.clear_color;   // cellGcmSetClearColor's ARGB
    VkClearColorValue cv;
    cv.float32[0] = (float)(c >> 16 & 0xFF) / 255.0f; cv.float32[1] = (float)(c >> 8 & 0xFF) / 255.0f;
    cv.float32[2] = (float)(c & 0xFF) / 255.0f; cv.float32[3] = (float)(c >> 24 & 0xFF) / 255.0f;
    // the engine's eng_clear_rect
    const uint32_t cw = sf.width < t->w ? sf.width : t->w, ch = sf.height < t->h ? sf.height : t->h;
    uint32_t rc[4];
    scissor_rect(r->st.scissor, cw, ch, rc);
    if (!rc[2] || !rc[3]) return;
    if (rc[0] == 0 && rc[1] == 0 && rc[2] >= cw && rc[3] >= ch) {
        access(b.cb, t, kCopyDst);
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        F.vkCmdClearColorImage(b.cb, t->img, layout_of(t), &cv, 1, &rg);
    } else {
        access(b.cb, t, kAttach);
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rbi.renderPass = render_pass(rt); rbi.framebuffer = framebuffer(t);
        rbi.renderArea = { { 0, 0 }, { t->w, t->h } };
        F.vkCmdBeginRenderPass(b.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, {} };
        ca.clearValue.color = cv;
        VkClearRect cr = { { { (int32_t)rc[0], (int32_t)rc[1] }, { rc[2], rc[3] } }, 0, 1 };
        F.vkCmdClearAttachments(b.cb, 1, &ca, 1, &cr);
        F.vkCmdEndRenderPass(b.cb);
    }
    mark(b, kSectDraw);
    t->written = 1;
    stats().n_clears++;
}

// ---- the thread ------------------------------------------------------------------------------

bool init()
{
    while (rsx_vk_bridge_device(&D)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
#define X(f) F.f = (PFN_##f)D.get_device_proc(D.device, #f); if (!F.f) { fail(#f, VK_ERROR_INITIALIZATION_FAILED); return false; }
    PPFX_VK_DEVICE(X)
#undef X
    pass_wrap_install();
    F.GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)D.get_instance_proc(D.instance, "vkGetPhysicalDeviceMemoryProperties");
    F.GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)D.get_instance_proc(D.instance, "vkGetPhysicalDeviceProperties");
    F.GetPhysicalDeviceFormatProperties =
        (PFN_vkGetPhysicalDeviceFormatProperties)D.get_instance_proc(D.instance, "vkGetPhysicalDeviceFormatProperties");
    F.GetPhysicalDeviceMemoryProperties(D.phys, &s_mem);
    VkPhysicalDeviceProperties props;
    F.GetPhysicalDeviceProperties(D.phys, &props);
    s_ts_period = props.limits.timestampPeriod;
    {
        const char* e = getenv("DOTHEROES_PPFX_BARRIERS");
        s_full = e && !strcmp(e, "full");
        char line[240];
        snprintf(line, sizeof line, "render: on %s queue of family %u (%s); barriers %s", D.shared_queue ? "the backend's" : "a second",
                 D.queue_family, props.deviceName, s_full ? "full (every image in GENERAL)" : "per image (tracked hazards)");
        report_add(line);
    }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = D.queue_family;
    if (F.vkCreateCommandPool(D.device, &pci, nullptr, &s_pool) != VK_SUCCESS) return false;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding lb[4] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr },
        { 16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 48, VK_DESCRIPTOR_TYPE_SAMPLER, 16, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 4; lci.pBindings = lb;
    F.vkCreateDescriptorSetLayout(D.device, &lci, nullptr, &s_dsl);
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_dsl;
    F.vkCreatePipelineLayout(D.device, &plci, nullptr, &s_pl);
    for (Batch& b : s_b) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = s_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        F.vkAllocateCommandBuffers(D.device, &ai, &b.cb);
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        F.vkCreateFence(D.device, &fi, nullptr, &b.fence);
        VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSets },
                                       { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * kSets },
                                       { VK_DESCRIPTOR_TYPE_SAMPLER, 16 * kSets } };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = kSets; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
        F.vkCreateDescriptorPool(D.device, &dpi, nullptr, &b.dp);
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kQueries;
        F.vkCreateQueryPool(D.device, &qi, nullptr, &b.qp);
        {   // R6: occlusion
            VkQueryPoolCreateInfo oi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            oi.queryType = VK_QUERY_TYPE_OCCLUSION; oi.queryCount = kOcc;
            F.vkCreateQueryPool(D.device, &oi, nullptr, &b.occ);
        }
        static int dev_up = -1;
        if (dev_up < 0) { const char* e = getenv("DOTHEROES_PPFX_UPLOAD"); dev_up = !(e && !strcmp(e, "host")); }
        if (!make_buffer(b.up, kUpload, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         false, dev_up != 0))
            return false;
    }
    // the dummy every unbound unit samples (as the engine's: 1x1 magenta)
    {
        Batch& b = batch(0);
        s_dummy = make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 4, false);
        access(b.cb, s_dummy, kCopyDst);
        const uint8_t magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
        const VkDeviceSize at = upload(b, magenta, 4, 16);
        VkBufferImageCopy c = {};
        c.bufferOffset = at; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; c.imageExtent = { 1, 1, 1 };
        F.vkCmdCopyBufferToImage(b.cb, b.up.b, s_dummy->img, layout_of(s_dummy), 1, &c);
        access(b.cb, s_dummy, kSampled);
        submit();
        Sampler ds = {};
        ds.min_filter = 2; ds.mag_filter = 2; ds.wrap_s = ds.wrap_t = ds.wrap_r = 3;
        s_dummy_smp = sampler(ds);
    }
    for (VkSemaphore& sem : s_present_sem) {   // R6's native present
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        F.vkCreateSemaphore(D.device, &sci, nullptr, &sem);
    }
    const char* d = getenv("DOTHEROES_PPFX_DUMP");
    snprintf(s_dump, sizeof s_dump, "%s", d && *d ? d : "");
#ifdef _WIN32
    if (s_dump[0]) CreateDirectoryA(s_dump, NULL);
#endif
    s_ready = 1;
    return true;
}

std::atomic<int> s_idle{0};   // the thread has submitted everything walked and waits

// ---- R6: occlusion reports (docs/NATIVE_R6.md section 5) -------------------------------------
// The RSX's zpass counter, as the records drive it: cellGcmSetClearReport
// starts a new count, every draw made with zpass counting on adds the samples
// that pass its depth and stencil tests (one precise occlusion query each),
// cellGcmSetReport writes the count so far into the report and
// cellGcmSetTransferReportData copies reports to the game's buffer. Written
// into guest memory when the batch with them completes (the GPU has the
// counts), in record order: a transfer after the reports before it, the pad
// word last; main-memory colour targets the records drew into written back
// first (PPFX's probe reads its 1x1 target when its query is done).

}  // namespace
}  // namespace ppfx
extern "C" void cellGcm_report_write_pub(uint32_t index, uint32_t location, uint32_t value);
extern "C" int cellGcm_report_copy_pub(uint32_t index, uint32_t location, uint32_t io_out, uint32_t count);
extern "C" uint32_t cellGcm_io2ea_pub(uint32_t io);
extern "C" int rsx_texture_argb_is_rgba(void);
namespace ppfx {
namespace {

std::vector<uint64_t> s_occ_set;   // the queries counted since the last clear
uint64_t s_occ_gid = 0;
int occ_on()
{
    if (s_occ_on < 0) { const char* e = getenv("DOTHEROES_R6_OCCLUSION"); s_occ_on = e && *e == '1'; }
    return s_occ_on;
}
void occ_begin(Batch& b, const Rec* r)
{
    if (!r->st.raster.zpass_count || !occ_on() || !b.occ || b.nocc >= kOcc) { b.occ_gid.reserve(0); return; }
    F.vkCmdBeginQuery(b.cb, b.occ, b.nocc, VK_QUERY_CONTROL_PRECISE_BIT);
    const uint64_t gid = s_occ_gid++;
    b.occ_gid.push_back(gid);
    s_occ_set.push_back(gid);
    b.nocc++;
    s_occ_open = true;
    stats().occ_draws++;
}
void occ_end(Batch& b)
{
    if (!s_occ_open) return;
    F.vkCmdEndQuery(b.cb, b.occ, b.nocc - 1);
    s_occ_open = false;
}

// main-memory colour targets the records drew into since their last write-back
struct WriteBack { uint32_t ea, pitch, w, h, bpp; VkFormat fmt; Buf buf; };
void report_op(const Rec* r)
{
    Batch& b = batch(r->frame);
    wait_for(b, r);
    if (r->rep_op == kRepClear) { s_occ_set.clear(); return; }
    if (r->rep_op == kRepReport) {
        if (r->rep_type != 1) stats().occ_other_types++;   // ZCULL statistics: never seen; written as 0
        std::vector<uint64_t> gids = r->rep_type == 1 ? s_occ_set : std::vector<uint64_t>();
        const uint32_t index = r->rep_index;
        b.done.push_back([gids, index] {
            uint64_t sum = 0;
            for (uint64_t g : gids) sum += s_occ_val[g & 0xFFFFu];
            const uint32_t v = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sum;
            cellGcm_report_write_pub(index, 0 /* LOCAL */, v);
            stats().occ_reports++;
            stats().occ_sum_last = v;
        });
        return;
    }
    // a transfer: the main-memory targets first
    std::vector<WriteBack> wb;
    for (auto& kv : s_targets) {
        Img* t = kv.second;
        if ((kv.first >> 32) != 1 || !t || !t->main_dirty) continue;
        t->main_dirty = 0;
        WriteBack w;
        w.ea = cellGcm_io2ea_pub((uint32_t)kv.first); w.pitch = t->guest_pitch ? t->guest_pitch : t->w * t->bpp;
        w.w = t->w; w.h = t->h; w.bpp = t->bpp; w.fmt = t->fmt;
        if (!w.ea || !make_buffer(w.buf, (VkDeviceSize)t->w * t->h * t->bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) continue;
        access(b.cb, t, kCopySrc);
        VkBufferImageCopy c = {};
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageExtent = { t->w, t->h, 1 };
        F.vkCmdCopyImageToBuffer(b.cb, t->img, layout_of(t), w.buf.b, 1, &c);
        mark(b, kSectReadback);
        wb.push_back(w);
    }
    const uint32_t index = r->rep_index, io = r->rep_io, count = r->rep_count ? r->rep_count : 1;
    b.done.push_back([wb, index, io, count] {
        const bool as_is = rsx_texture_argb_is_rgba() != 0;
        for (const WriteBack& w : wb) {
            if (w.bpp == 4 && (w.fmt == VK_FORMAT_R8G8B8A8_UNORM || w.fmt == VK_FORMAT_B8G8R8A8_UNORM)) {
                const bool bgra = w.fmt == VK_FORMAT_B8G8R8A8_UNORM;
                for (uint32_t y = 0; y < w.h; y++) {
                    if ((uint64_t)w.ea + (uint64_t)y * w.pitch + w.w * 4 > 0xE0000000ull) break;
                    uint8_t* d = vm_base + w.ea + y * w.pitch;
                    const uint8_t* sp = w.buf.p + (size_t)y * w.w * 4;
                    for (uint32_t x = 0; x < w.w; x++, d += 4, sp += 4) {
                        if (as_is) { memcpy(d, sp, 4); continue; }
                        d[0] = sp[3]; d[1] = bgra ? sp[2] : sp[0]; d[2] = sp[1]; d[3] = bgra ? sp[0] : sp[2];
                    }
                }
                stats().occ_writebacks++;
            }
            F.vkDestroyBuffer(D.device, w.buf.b, nullptr);
            F.vkFreeMemory(D.device, w.buf.m, nullptr);
        }
        std::atomic_thread_fence(std::memory_order_release);
        if (cellGcm_report_copy_pub(index, 0 /* LOCAL */, io, count)) stats().occ_transfers++;
    });
}

// ---- R6: the native present (docs/NATIVE_R6.md section 2) -----------------------------------

}  // namespace
}  // namespace ppfx
extern "C" void dotheroes_present_finish_async(void);   // main.cpp: the window present on the present thread
extern "C" int cellGcm_flip_immediate(void);
extern "C" void cellGcmTickFlip(void);
namespace ppfx {
namespace {


// A kFlip: the display buffer the flip names, as the native renderer drew
// it, blitted into the backend's present target on this queue, and the
// present's own submission (on the backend's queue) made to wait for that.
// The previous present is complete first (rsx_vk_bridge_present_prepare: the
// backend's queue is done reading the target). Then the flip counts as
// presented: the game's flip status follows (the flip gate).
void present_native(const Rec* r)
{
    const uint64_t t0 = now_ns();
    // the clears made after the frame's last draw first (a frame that draws
    // nothing after the game's frame-start clear shows the cleared buffer)
    if (r->naclear) {
        Batch& b = batch(r->frame);
        wait_for(b, r);
        asp_clears(b, r);
        stats().r6_flip_clears += r->naclear;
    }
    auto it = s_targets.find((uint64_t)r->flip_offset);   // a display buffer: local memory (location 0)
    Img* t = it == s_targets.end() ? nullptr : it->second;
    VkImage target = VK_NULL_HANDLE;
    uint32_t tw = 0, th = 0;
    // the frame's work first, without the present's lock (its submission may
    // wait for the engine's: nothing the engine does then may need the lock)
    if (s_b[s_cur].open) submit();
    if (t && t->written && !rsx_vk_bridge_present_prepare(&target, &tw, &th)) {
        Batch& b = batch(r->frame);
        wait_for(b, r);
        access(b.cb, t, kCopySrc);
        VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        ib.srcAccessMask = 0; ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = target; ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        F.vkCmdPipelineBarrier(b.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        VkImageBlit bl = {};
        bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bl.srcOffsets[1] = { (int32_t)(r->flip_w < t->w ? r->flip_w : t->w), (int32_t)(r->flip_h < t->h ? r->flip_h : t->h), 1 };
        bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bl.dstOffsets[1] = { (int32_t)tw, (int32_t)th, 1 };
        const bool same = bl.srcOffsets[1].x == (int32_t)tw && bl.srcOffsets[1].y == (int32_t)th;
        F.vkCmdBlitImage(b.cb, t->img, layout_of(t), target, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                         same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = 0;
        F.vkCmdPipelineBarrier(b.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        mark(b, kSectOther);
        // DOTHEROES_R6_HAZARD=present (a negative control for the validation
        // layer): the present's submission does not wait for the blit
        static int hz = -1;
        if (hz < 0) { const char* e = getenv("DOTHEROES_R6_HAZARD"); hz = e && !strcmp(e, "present"); }
        VkSemaphore sem = s_present_sem[s_present_n++ & 1];
        if (!hz) b.signals.push_back(sem);
        submit();
        rsx_vk_bridge_present_submit(hz ? VK_NULL_HANDLE : sem, 0);
        dotheroes_present_finish_async();
        stats().r6_presents++;
    } else {
        stats().r6_present_missing++;
        static int said = 0;
        if (said++ < 8) {
            char line[160];
            snprintf(line, sizeof line, "render: flip %u of frame %u: no native image of display buffer 0:%08X -- not presented",
                     r->flip_id, r->frame, r->flip_offset);
            report_add(line);
        }
    }
    queue().flips_presented.fetch_add(1, std::memory_order_release);
    if (cellGcm_flip_immediate()) cellGcmTickFlip();   // benchmarks: the flip completes now
    stats().r6_present_ns += now_ns() - t0;
}

// ---- R4: handbacks ----------------------------------------------------------------------------

struct HbReq {
    uint64_t key;
    bool check;
    int state;        // 0 asked, 1 submitted, < 0 failed
    HandOut out;
};
std::mutex s_hb_lock;
std::condition_variable s_hb_cv;
HbReq* s_hb;             // the walker's request, while it waits
bool s_hb_serving;       // this thread is working on it (the walker does not leave before)
std::vector<Buf> s_hb_bufs;   // the checks' readbacks, by slot
std::vector<bool> s_hb_busy;

// This thread, with every record walked recorded: the image ready for the
// engine's copy, the batch submitted with the request's semaphore.
void hb_service(uint32_t frame)
{
    HbReq* q;
    {
        std::lock_guard<std::mutex> g(s_hb_lock);
        q = s_hb;
        if (!q || q->state) return;
        s_hb_serving = true;
    }
    int rc = 0;
    auto it = s_targets.find(q->key);
    Img* t = it == s_targets.end() ? nullptr : it->second;
    uint64_t sem = 0;
    const int slot = t && t->written ? slot_semaphore(&sem) : -1;
    if (!t || !t->written) rc = -1;
    else if (slot < 0) rc = -2;
    else {
        Batch& b = batch(frame);
        access(b.cb, t, kCopySrc);
        int cs = -1;
        if (q->check) {
            const VkDeviceSize bytes = (VkDeviceSize)t->w * t->h * t->bpp;
            std::lock_guard<std::mutex> g(s_hb_lock);
            for (size_t i = 0; i < s_hb_busy.size(); i++)
                if (!s_hb_busy[i] && s_hb_bufs[i].n >= bytes) { cs = (int)i; break; }
            if (cs < 0 && s_hb_bufs.size() < 8) {
                Buf nb;
                if (make_buffer(nb, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
                    s_hb_bufs.push_back(nb); s_hb_busy.push_back(false); cs = (int)s_hb_bufs.size() - 1;
                }
            }
            if (cs >= 0) {
                s_hb_busy[cs] = true;
                VkBufferImageCopy c = {};
                c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                c.imageExtent = { t->w, t->h, 1 };
                F.vkCmdCopyImageToBuffer(b.cb, t->img, layout_of(t), s_hb_bufs[cs].b, 1, &c);
                mark(b, kSectReadback);
            }
        }
        b.signals.push_back((VkSemaphore)sem);
        submit();
        HandOut& o = q->out;
        o.image = (uint64_t)t->img; o.layout = (uint32_t)layout_of(t); o.w = t->w; o.h = t->h;
        o.vkformat = (uint32_t)t->fmt; o.bpp = t->bpp; o.sem_slot = slot; o.sem = sem;
        o.check_slot = cs; o.check_rows = cs >= 0 ? s_hb_bufs[cs].p : nullptr;
    }
    {
        std::lock_guard<std::mutex> g(s_hb_lock);
        q->state = rc ? rc : 1;
        s_hb_serving = false;
    }
    s_hb_cv.notify_all();
}

// ---- R6: a native image read back (the snapshot, guest reads, the present comparison) ---------

struct RbReq {
    uint64_t key, upto;      // the image; served once every record before `upto` is recorded
    std::vector<uint8_t>* rows;
    uint32_t w, h, vkformat, bpp;
    int state;               // 0 asked, 1 done, < 0 failed
};
RbReq* s_rb;                 // under s_hb_lock
std::atomic<uint64_t> s_rb_upto{~0ull};   // the request's upto, for this thread's check without the lock
bool s_rb_serving;
Buf s_rb_buf;                // host-visible, grown as needed

// This thread, with every record before the request's `upto` recorded: the
// image copied into host memory in a batch of its own, waited for here.
void rb_service(uint32_t frame)
{
    RbReq* q;
    {
        std::lock_guard<std::mutex> g(s_hb_lock);
        q = s_rb;
        if (!q || q->state) return;
        s_rb_serving = true;
    }
    int rc = 0;
    auto it = s_targets.find(q->key);
    Img* t = it == s_targets.end() ? nullptr : it->second;
    if (!t || !t->written) rc = -1;
    else {
        const VkDeviceSize bytes = (VkDeviceSize)t->w * t->h * t->bpp;
        if (s_rb_buf.n < bytes) {
            // the old one: its readbacks' batches were waited for (below), nothing uses it
            if (s_rb_buf.b) { F.vkDestroyBuffer(D.device, s_rb_buf.b, nullptr); F.vkFreeMemory(D.device, s_rb_buf.m, nullptr); s_rb_buf = Buf(); }
            if (!make_buffer(s_rb_buf, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) rc = -2;
        }
        if (!rc) {
            if (s_b[s_cur].open) submit();
            const int bi = s_cur;
            Batch& b = batch(frame);
            access(b.cb, t, kCopySrc);
            VkBufferImageCopy c = {};
            c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.imageExtent = { t->w, t->h, 1 };
            F.vkCmdCopyImageToBuffer(b.cb, t->img, layout_of(t), s_rb_buf.b, 1, &c);
            mark(b, kSectReadback);
            submit();
            retire(s_b[bi]);   // its fence: the copy is in host memory
            q->rows->assign(s_rb_buf.p, s_rb_buf.p + bytes);
            q->w = t->w; q->h = t->h; q->vkformat = (uint32_t)t->fmt; q->bpp = t->bpp;
        }
    }
    {
        std::lock_guard<std::mutex> g(s_hb_lock);
        q->state = rc ? rc : 1;
        s_rb_serving = false;
    }
    s_hb_cv.notify_all();
}

void run()
{
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"ppfx native");
#endif
    if (!init()) { report_add("render: the native renderer did not start"); return; }
    Queue& q = queue();
    uint64_t next = 0;
    uint32_t frame = 0;
    for (;;) {
        if (s_stop.load()) {   // R6: the exit drain is done: everything waited for, nothing more
            submit();
            for (Batch& b : s_b) retire(b);
            s_stopped = 1;
            return;
        }
        for (Batch& b : s_b) if (retire_ready(b)) retire(b);
        if (!s_static_swept.empty()) {   // R6: into the pool once unused, as retired images are destroyed
            Batch& cur = s_b[s_cur];
            std::vector<Img*> v;
            v.swap(s_static_swept);
            if (cur.open) cur.done.push_back([v] { for (Img* o : v) static_pool_put(o); });
            else {
                bool idle = true;
                for (Batch& b : s_b) idle &= !b.pending;
                if (idle) for (Img* o : v) static_pool_put(o);
                else s_static_swept.swap(v);   // later
            }
        }
        if (!s_retired.empty()) {
            // retired images: destroyed once every batch that may use them has
            // completed -- the batch being recorded too (R5: an image retired
            // while it was open, used by its earlier records, was destroyed
            // before it was submitted, and the device was lost)
            Batch& cur = s_b[s_cur];
            if (cur.open) {
                std::vector<Img*> v;
                v.swap(s_retired);
                cur.done.push_back([v] { for (Img* o : v) destroy_image(o); });
            } else {
                bool idle = true;
                for (Batch& b : s_b) idle &= !b.pending;
                if (idle) { for (Img* o : s_retired) destroy_image(o); s_retired.clear(); }
            }
        }
        if (next >= s_rb_upto.load(std::memory_order_acquire)) rb_service(frame);   // R6: a readback asked for
        if (next >= q.walked.load(std::memory_order_acquire)) {
            // R4: a handback asked for (everything walked is recorded now)
            hb_service(frame);
            // nothing walked yet: submit what is open, so its work and
            // comparisons go ahead, and wait for the walker
            const uint64_t t0 = now_ns();
            submit();
            s_where = 2;
            if (s_draining) { for (Batch& b : s_b) retire(b); s_idle = 1; }
            std::unique_lock<std::mutex> lk(q.m);
            q.cv.wait_for(lk, std::chrono::milliseconds(2),
                          [&] { return next < q.walked.load(std::memory_order_acquire) || s_hb != nullptr || s_stop.load() ||
                                       next >= s_rb_upto.load(std::memory_order_acquire); });
            stats().wait_walker_us += (now_ns() - t0) / 1000;
            continue;
        }
        s_idle = 0;
        Rec* r = q.at(next);
        apply_vconst(r);   // R6: every record's constants, in order, whatever is drawn of it
        if (r->kind == kAspDraw && !asp_native()) {   // R5: checked, not rendered
            next++;
            q.done.store(next, std::memory_order_release);
            q.cv.notify_all();
            continue;
        }
        if (r->frame != frame && s_b[s_cur].open) submit();   // a frame's work in a batch of its own
        if (r->frame != frame) {
            stats().n_frames++;
            frames_close(r->frame);
            // DOTHEROES_PPFX_REALLOC_EVERY=<n> (a test of image lifetime):
            // every n frames every native image is retired -- destroyed once
            // the batches in flight are done -- and made again at its next use
            static int every = -1;
            if (every < 0) { const char* e = getenv("DOTHEROES_PPFX_REALLOC_EVERY"); every = e && *e ? atoi(e) : 0; }
            if (every > 0 && r->frame % (uint32_t)every == 0 && !s_targets.empty()) {
                if (s_b[s_cur].open) submit();
                for (auto& kv : s_targets) { s_retired.push_back(kv.second); stats().n_retired++; }
                s_targets.clear();
                for (auto& kv : s_snap) if (kv.second) { s_retired.push_back(kv.second); stats().n_retired++; }
                s_snap.clear();
                // R6: the depth images, the guest textures and the static ones too
                for (auto& kv : s_depths) if (kv.second) { s_retired.push_back(kv.second); stats().n_retired++; }
                s_depths.clear();
                for (auto& kv : s_gtex) if (kv.second.img) { s_retired.push_back(kv.second.img); stats().n_retired++; }
                s_gtex.clear();
                for (auto& kv : s_static) if (kv.second.img) { s_static_swept.push_back(kv.second.img); stats().n_retired++; }
                s_static.clear();
            }
        }
        frame = r->frame;
        // DOTHEROES_PPFX_DELAY_MS=<n> (a test): a slow consumer, n ms a record
        static int delay = -1;
        if (delay < 0) { const char* e = getenv("DOTHEROES_PPFX_DELAY_MS"); delay = e && *e ? atoi(e) : 0; }
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        // R6: DOTHEROES_PPFX_DELAY_US=<n> (the backpressure test): n us a record, spun
        static int delay_us = -1;
        if (delay_us < 0) { const char* e = getenv("DOTHEROES_PPFX_DELAY_US"); delay_us = e && *e ? atoi(e) : 0; }
        if (delay_us) { const uint64_t until = now_ns() + 1000ull * delay_us; while (now_ns() < until) {} }
        const uint64_t t0 = now_ns();
        if (r->kind == kAspDraw) asp_draw(r);   // R5 (DOTHEROES_ASP_NATIVE=1)
        else if (r->kind == kDraw) draw(r);
        else if (r->kind == kClear) clear(r);
        else if (r->kind == kFlip) present_native(r);   // R6 (DOTHEROES_R6_PRESENT=1)
        else if (r->kind == kReport) report_op(r);      // R6 (DOTHEROES_R6_OCCLUSION=1)
        stats().cpu_record_ns += now_ns() - t0;
        if (s_b[s_cur].open && (s_b[s_cur].used > kUpload / 2 || s_b[s_cur].sets > kSets / 2 ||
                                s_b[s_cur].nq > kQueries - 64 || s_b[s_cur].nocc > kOcc - 64)) submit();
        next++;
        s_next = next;
        s_where = 1;
        q.done.store(next, std::memory_order_release);
        q.cv.notify_all();
    }
}

}  // namespace

uint32_t surface_vkformat(uint32_t color_format, uint32_t* bpp)
{
    rsx_be_format f;   // the engine's eng_surface_format
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: f = RSX_BE_FMT_R16G16B16A16F; break;
    case 0x0C: f = RSX_BE_FMT_R32G32B32A32F; break;
    case 0x0D: f = RSX_BE_FMT_R32F; break;
    default: f = RSX_BE_FMT_R8G8B8A8; break;
    }
    return (uint32_t)be_vkformat(f, bpp);
}

uint32_t be_vkformat_pub(uint32_t be_format, uint32_t* bpp) { return (uint32_t)be_vkformat((rsx_be_format)be_format, bpp); }

bool render_ready() { return s_ready.load() != 0; }

uint32_t surface_beformat(uint32_t color_format)
{
    switch (color_format & 0x1Fu) {   // the engine's eng_surface_format
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default: return RSX_BE_FMT_R8G8B8A8;
    }
}

int handback_take(uint32_t location, uint32_t offset, bool check, uint32_t timeout_ms, HandOut* out)
{
    if (!render_ready()) return -10;
    HbReq q;
    q.key = akey(location, offset); q.check = check; q.state = 0;
    memset(&q.out, 0, sizeof q.out);
    q.out.check_slot = -1;
    std::unique_lock<std::mutex> lk(s_hb_lock);
    s_hb = &q;
    lk.unlock();
    { std::lock_guard<std::mutex> g(queue().m); }
    queue().cv.notify_all();
    lk.lock();
    const bool ok = s_hb_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return q.state != 0; });
    // not before the native thread has left the request alone (it lives on this stack)
    s_hb_cv.wait(lk, [&] { return !s_hb_serving; });
    s_hb = nullptr;
    if (!ok && q.state == 0) return -11;
    if (q.state < 0) return q.state;
    *out = q.out;
    return 0;
}

int readback_take(uint32_t location, uint32_t offset, uint64_t upto, uint32_t timeout_ms, std::vector<uint8_t>& rows,
                  uint32_t* w, uint32_t* h, uint32_t* vkformat, uint32_t* bpp)
{
    if (!render_ready()) return -10;
    static std::mutex one;   // one request at a time (the walker's comparison, the game's snapshot)
    std::lock_guard<std::mutex> only(one);
    RbReq q;
    q.key = akey(location, offset); q.upto = upto; q.rows = &rows; q.w = q.h = q.vkformat = q.bpp = 0; q.state = 0;
    std::unique_lock<std::mutex> lk(s_hb_lock);
    s_rb = &q;
    s_rb_upto.store(upto, std::memory_order_release);
    lk.unlock();
    { std::lock_guard<std::mutex> g(queue().m); }
    queue().cv.notify_all();
    lk.lock();
    const bool ok = s_hb_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return q.state != 0; });
    s_hb_cv.wait(lk, [&] { return !s_rb_serving; });
    s_rb = nullptr;
    s_rb_upto.store(~0ull, std::memory_order_release);
    if (!ok && q.state == 0) {
        char line[200];
        snprintf(line, sizeof line, "render: readback of %u:%08X timed out: the native thread at record %llu (wanted %llu, "
                 "walked %llu), where %d", location, offset, (unsigned long long)s_next.load(), (unsigned long long)upto,
                 (unsigned long long)queue().walked.load(), s_where.load());
        report_add(line);
        return -11;
    }
    if (q.state < 0) return q.state;
    *w = q.w; *h = q.h; *vkformat = q.vkformat; *bpp = q.bpp;
    return 0;
}

void handback_check_done(int slot)
{
    std::lock_guard<std::mutex> g(s_hb_lock);
    if (slot >= 0 && (size_t)slot < s_hb_busy.size()) s_hb_busy[slot] = false;
}

void frame_flag_inherited(uint32_t frame)
{
    std::lock_guard<std::mutex> g(s_inh_lock);
    s_inherited.insert(frame);
    while (s_inherited.size() > 4096) s_inherited.erase(s_inherited.begin());
}

// At exit: the native thread submits everything walked and the comparisons
// queued run, within the time given; what is still in flight then is
// counted as unfinished (wanted = equal + differ + skipped + unfinished).
void render_drain(uint32_t timeout_ms)
{
    if (!render_ready()) return;
    const uint64_t t0 = now_ns(), limit = (uint64_t)timeout_ms * 1000000ull;
    s_draining = 1;
    Queue& q = queue();
    while (now_ns() - t0 < limit) {
        const bool walked_done = q.done.load() >= q.walked.load() && s_idle.load();
        bool cmp_done;
        { std::lock_guard<std::mutex> g(s_cmp_lock); cmp_done = s_cmp_q.empty() && !s_cmp_running; }
        if (walked_done && cmp_done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // R6: the native thread stops here -- every batch it submitted completed
    // (waited for), nothing more submitted -- so no native work is in flight
    // when the process ends, whatever the walker still passes it
    s_stop = 1;
    { std::lock_guard<std::mutex> g(queue().m); }
    queue().cv.notify_all();
    for (int i = 0; i < 400 && !s_stopped.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        char line[200];
        snprintf(line, sizeof line, "render: exit drain %.1f ms, the native thread %s; batches cancelled (an engine signal "
                 "that never came) %llu", (now_ns() - t0) / 1e6, s_stopped.load() ? "stopped, its batches complete" : "NOT stopped",
                 (unsigned long long)s_cancelled);
        report_add(line);
    }
    Stats& s = stats();
    const uint64_t ended = s.cmp_equal + s.cmp_differ + s.cmp_skip_no_ref + s.cmp_skip_no_readback +
                           s.cmp_skip_extent + s.cmp_skip_not_drawn;
    s.cmp_unfinished = s.cmp_wanted > ended ? s.cmp_wanted - ended : 0;
}

void render_report(std::vector<std::string>& out)
{
    {   // R6: where the native thread is (a diagnostic for a stall: the record it takes next)
        Queue& q = queue();
        const uint64_t n = s_next.load();
        char line[220];
        const Rec* r = n < q.put.load() && q.at(n) ? q.at(n) : nullptr;
        snprintf(line, sizeof line, "render: the native thread at record %llu (kind %u frame %u bypassed %u sem %d ord %lld), "
                 "where %d; put %llu walked %llu done %llu", (unsigned long long)n, r ? r->kind : 0u, r ? r->frame : 0u,
                 r ? r->bypassed : 0u, r ? r->walk.sem : -1, r ? (long long)r->walk.ordinal : -1LL, s_where.load(),
                 (unsigned long long)q.put.load(), (unsigned long long)q.walked.load(), (unsigned long long)q.done.load());
        out.push_back(line);
    }
    if (s_ap_draws) {
        std::string line = "asp draw profile (us a draw, " + std::to_string(s_ap_draws) + " draws):";
        uint64_t tot = 0;
        for (int i = 0; i < kApHash; i++) tot += s_ap_ns[i];
        char x[96];
        for (int i = 0; i < kApN; i++) {
            snprintf(x, sizeof x, " %s %.2f", kApName[i], s_ap_ns[i] / 1e3 / (double)s_ap_draws);
            line += x;
        }
        snprintf(x, sizeof x, " | total %.2f us, %.1f s | %llu draws joined the previous one's pass", tot / 1e3 / (double)s_ap_draws,
                 tot / 1e9, (unsigned long long)s_pass_merged);
        line += x;
        out.push_back(line);
    }
    if (s_rh_memo_bytes || s_rh_rehashed || s_rh_ww_bytes) {
        char line[400];
        snprintf(line, sizeof line, "asp range hashes: %.1f MB unwritten since hashed (the write watch), %.1f MB taken from earlier in "
                 "the frame, %llu ranges rewritten within a frame | write watch %s: %llu polls, %.1f ms, %llu pages scanned, %llu found "
                 "written, %zu runs now%s",
                 s_rh_ww_bytes / 1048576.0, s_rh_memo_bytes / 1048576.0, (unsigned long long)s_rh_rehashed,
                 vm_write_watch ? "on" : "off", (unsigned long long)s_ww_polls, s_ww_ns / 1e6, (unsigned long long)s_ww_scanned,
                 (unsigned long long)s_ww_pages, s_ww_runs.size(), s_ww_failed ? " (failed: turned off)" : "");
        out.push_back(line);
    }
    if (s_vc_hits || s_vc_misses) {
        char line[300];
        snprintf(line, sizeof line, "asp vertex cache: vertices %llu hits %llu misses, indices %llu hits %llu misses, %llu not cached "
                 "(changed since their last use), %.1f MB hashed, %zu arenas, %llu retired", (unsigned long long)s_vc_hits.load(),
                 (unsigned long long)s_vc_misses.load(), (unsigned long long)s_ic_hits.load(), (unsigned long long)s_ic_misses.load(),
                 (unsigned long long)s_vc_uncached.load(), s_vc_hashed / 1048576.0, s_arenas.size(), (unsigned long long)s_arenas_retired.load());
        out.push_back(line);
    }
    {
        char line[200];
        snprintf(line, sizeof line, "static textures: %zu live, %llu images reused from the pool (%zu pooled, %llu destroyed past it)",
                 s_static.size(), (unsigned long long)s_static_reused, s_static_pool.size(),
                 (unsigned long long)s_static_pool_destroyed);
        out.push_back(line);
    }
    if (s_aspc_n) {
        char line[300];
        snprintf(line, sizeof line, "asp compare: %llu targets compared, %llu equal; %.4f%% of pixels differ, the largest difference %llu",
                 (unsigned long long)s_aspc_n.load(), (unsigned long long)s_aspc_equal.load(),
                 s_aspc_px_total ? 100.0 * s_aspc_px / s_aspc_px_total : 0.0, (unsigned long long)s_aspc_maxd.load());
        out.push_back(line);
        snprintf(line, sizeof line, "asp vertex cache: converted without the cache %llu draws, %.1f MB, the largest %.1f MB",
                 (unsigned long long)s_vc_direct.load(), s_vc_direct_bytes / 1048576.0, s_vc_direct_max / 1048576.0);
        out.push_back(line);
    }
    {
        std::lock_guard<std::mutex> g(s_why_lock);
        for (auto& kv : s_why) {
            char line[300];
            snprintf(line, sizeof line, "not rendered: %llu %s", (unsigned long long)kv.second, kv.first.c_str());
            out.push_back(line);
        }
    }
    std::lock_guard<std::mutex> g(s_var_lock);
    char line[512];
    for (auto& kv : s_vars) {
        const VarAcc& v = kv.second;
        snprintf(line, sizeof line, "variant %016llx: %llu frames (%zu program variants), %llu with every draw compared; "
                 "draws equal %llu differ %llu; finals equal %llu of %llu | %s", (unsigned long long)kv.first,
                 (unsigned long long)v.frames, v.progs.size(), (unsigned long long)v.cmp_frames,
                 (unsigned long long)v.eq, (unsigned long long)v.diff, (unsigned long long)v.fin_eq,
                 (unsigned long long)v.fin, v.desc.c_str());
        out.push_back(line);
    }
    // frames not closed yet (their comparisons in flight, or the run's last)
    for (auto& kv : s_frames) {
        const FrameAcc& f = kv.second;
        if (!f.eq && !f.diff && !f.pending) continue;
        snprintf(line, sizeof line, "open frame %u: %u draws, compared equal %u differ %u, pending %d",
                 kv.first, f.draws, f.eq, f.diff, f.pending);
        out.push_back(line);
    }
}

int slot_image(uint32_t vkformat, uint32_t w, uint32_t h, uint64_t* image)
{
    if (!render_ready()) return -1;
    std::lock_guard<std::mutex> g(s_slot_lock);
    for (size_t i = 0; i < s_simg.size(); i++) {
        SlotImg& s = s_simg[i];
        if (!s.busy && s.im->fmt == (VkFormat)vkformat && s.im->w == w && s.im->h == h) {
            s.busy = true; *image = (uint64_t)s.im->img; return (int)i;
        }
    }
    if (s_simg.size() >= kMaxSlotImages) return -1;
    uint32_t bpp = 0;
    switch ((VkFormat)vkformat) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: bpp = 16; break;
    default: bpp = 4; break;
    }
    // created here (the walker's thread): vkCreateImage is free-threaded; the
    // engine's copy takes it to GENERAL with its own barrier, and it stays
    // there (the native side reads it in GENERAL, ordered by the semaphore)
    Img* o = make_image((VkFormat)vkformat, w, h, 1, bpp, false);
    if (!o) return -1;
    o->external = true;
    o->layout = VK_IMAGE_LAYOUT_GENERAL;
    s_simg.push_back({ o, true });
    *image = (uint64_t)o->img;
    return (int)s_simg.size() - 1;
}

int slot_buffer(uint64_t bytes, uint64_t* buffer, int prio)
{
    if (!render_ready()) return -1;
    std::lock_guard<std::mutex> g(s_slot_lock);
    size_t busy = 0;
    for (const SlotBuf& b : s_sbuf) busy += b.busy;
    // the reserves: a sampled comparison leaves room for the change frames'
    // and the finals', a change frame's for the finals'
    const size_t keep = prio >= 2 ? 0 : prio == 1 ? kFinalReserve : kFinalReserve + kChangeReserve;
    if (busy + keep >= kMaxSlotBuffers) return -1;
    int best = -1;
    for (size_t i = 0; i < s_sbuf.size(); i++)
        if (!s_sbuf[i].busy && s_sbuf[i].b.n >= bytes && (best < 0 || s_sbuf[i].b.n < s_sbuf[best].b.n)) best = (int)i;
    if (best >= 0) { s_sbuf[best].busy = true; *buffer = (uint64_t)s_sbuf[best].b.b; return best; }
    if (s_sbuf.size() >= kMaxSlotBuffers) return -1;
    // bounded in bytes too, with the same reserves in bytes
    VkDeviceSize total = 0;
    for (const SlotBuf& b : s_sbuf) total += b.b.n;
    const VkDeviceSize cap = prio >= 2 ? kSlotBufferBytes : prio == 1 ? kSlotBufferBytes * 7 / 8 : kSlotBufferBytes * 5 / 8;
    if (total + bytes > cap) return -1;
    SlotBuf s;
    if (!make_buffer(s.b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return -1;
    s.busy = true; s.prio = prio;
    s_sbuf.push_back(s);
    *buffer = (uint64_t)s.b.b;
    return (int)s_sbuf.size() - 1;
}

int slot_semaphore(uint64_t* semaphore)
{
    if (!render_ready()) return -1;
    std::lock_guard<std::mutex> g(s_slot_lock);
    for (size_t i = 0; i < s_ssem.size(); i++)
        if (!s_ssem[i].busy) { s_ssem[i].busy = true; *semaphore = (uint64_t)s_ssem[i].s; return (int)i; }
    if (s_ssem.size() >= kMaxSlotSems) return -1;
    VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore s = VK_NULL_HANDLE;
    if (F.vkCreateSemaphore(D.device, &ci, nullptr, &s) != VK_SUCCESS) return -1;
    s_ssem.push_back({ s, true });
    *semaphore = (uint64_t)s;
    return (int)s_ssem.size() - 1;
}

void slot_release_image(int slot)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (slot >= 0 && (size_t)slot < s_simg.size()) s_simg[slot].busy = false;
}

void slot_release_buffer(int slot)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (slot >= 0 && (size_t)slot < s_sbuf.size()) s_sbuf[slot].busy = false;
}

void slot_release_semaphore(int slot)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (slot >= 0 && (size_t)slot < s_ssem.size()) s_ssem[slot].busy = false;
}

// Which records' outputs are compared: the final composite of every
// DOTHEROES_PPFX_COMPARE_FINALS-th frame (default 1: every frame's), every
// draw of every DOTHEROES_PPFX_COMPARE_EVERY-th frame (default 30), and (the
// bridge decides) every draw after a structure change. A final's comparison
// waits for a readback buffer when all are busy, which at the display's pace
// costs frames (the owner's boss session, docs/NATIVE_R4.md section 6): play
// sessions sample the finals instead.
int compare_wanted(const Rec* r)
{
    static int every = -1, finals = -1, off = -1;
    if (every < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE_EVERY"); every = e && *e ? atoi(e) : 30; }
    if (finals < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE_FINALS"); finals = e && *e ? atoi(e) : 1; }
    // DOTHEROES_PPFX_COMPARE=0: no comparison at all (the native renderer's own cost)
    if (off < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE"); off = e && *e == '0'; }
    if (off) return kCmpNone;
    if (r->kind == kAspDraw) {   // R5: DOTHEROES_ASP_COMPARE_DRAWS=<n>: every asp draw of every n-th frame
        static int ad = -1;
        if (ad < 0) { const char* e = getenv("DOTHEROES_ASP_COMPARE_DRAWS"); ad = e && *e ? atoi(e) : 0; }
        return ad > 0 && asp_native() && r->frame % (uint32_t)ad == 0 && r->nneed && r->need[0].unit == 0xFF &&
               r->st.raster.color_mask ? kCmpEvery : kCmpNone;
    }
    if (r->kind != kDraw) return kCmpNone;
    const bool every_frame = every > 0 && r->frame % (uint32_t)every == 0;
    if (r->need[0].format == 0x08 && finals > 0 && r->frame % (uint32_t)finals == 0)
        return kCmpFinal;   // the final composite: the A8R8G8B8 display buffer
    return every_frame ? kCmpEvery : kCmpNone;
}

void render_start()
{
    std::thread(run).detach();
    std::thread(cmp_thread).detach();
}

}  // namespace ppfx
