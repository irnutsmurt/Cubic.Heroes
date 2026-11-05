/* R1's executor (docs/NATIVE_PORT.md section 5): renders one captured pass
 * description with its own Vulkan instance, device, images, buffers and
 * pipeline, and reads its output back.
 *
 *   r1_replay <capture dir> [--validate] [--poison] [--reps N] [--warmup N] [--batches N]
 *   r1_replay --chain <capture dir> ...   (R2: a whole chain, frame after frame; below)
 *
 * <capture dir> holds desc.json and the files it names (written by
 * rsx_vulkan_backend.c's capture). The steps:
 *  1. The programs: the microcode and the conversion parameters made into
 *     HLSL again by the toolkit's decompilers, with the engine's recipe
 *     (rsx_draw_engine.c, eng_programs_text), and into SPIR-V by glslang;
 *     each compared with the capture's, byte for byte. The pipeline is made
 *     from this SPIR-V, not the captured one.
 *  2. The resources, the pipeline and the draw, from the description alone,
 *     in proper image layouts (the existing renderer keeps every image in
 *     GENERAL).
 *  3. The output read back into native_output.bin. With --poison the target
 *     starts as 0xA5 bytes instead of its captured content, so a pixel the
 *     draw does not produce shows (native_output_poisoned.bin).
 *  4. With --reps, the pass's GPU time: a timestamp before each repetition's
 *     render pass begins and one after it ends, each repetition ordered after
 *     the last (as a chain of passes is), after --warmup repetitions; uploads
 *     and readback are outside the timed command buffers.
 * Results go to replay.json; with --validate, the Khronos validation layer
 * runs with synchronisation validation, its messages in validation.txt.
 *
 * It links the program translators (asset conversion) and glslang, and none
 * of the runtime: no FIFO walker, RSX state or GCM HLE (tools/r1_check_deps.py
 * checks its symbols). */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "r1_json.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include "rsx_shader_spirv.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#endif

#define VKF(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkCreateDebugUtilsMessengerEXT) \
    X(vkDestroyDebugUtilsMessengerEXT)
#define VKD(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) \
    X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilReference) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkFreeCommandBuffers)
#define DECL(f) static PFN_##f p##f;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
static PFN_vkCreateInstance pvkCreateInstance;
VKF(DECL) VKD(DECL)

static const char* s_dir;
static int s_poison;   /* --poison: the target starts as 0xA5 bytes */
static int s_hazard;   /* --hazard-selftest: a missing dependency, on purpose */
static int s_attach_inputs;   /* --attachment-inputs: textures usable as attachments too (timing experiment) */
static int s_general;         /* --general-layouts: every image in GENERAL, as the game's backend keeps them
                               * (timing experiment) */
#define L_ATT (s_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
#define L_TEX (s_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
static FILE* s_vlog;
static int s_verr, s_vwarn;

static void die(const char* fmt, const char* a)
{
    fprintf(stderr, "r1_replay: ");
    fprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    exit(1);
}

/* A file of the capture, whole; NULL (and *n = 0) when absent. */
static void* load(const char* name, size_t* n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    FILE* f = fopen(path, "rb");
    *n = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* p = malloc(sz > 0 ? (size_t)sz + 1 : 1);
    if (sz > 0 && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    ((char*)p)[sz > 0 ? sz : 0] = 0;
    fclose(f);
    *n = (size_t)sz;
    return p;
}

static void save(const char* name, const void* p, size_t n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n) die("cannot write %s", path);
    fclose(f);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                 VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* d, void* user)
{
    (void)type; (void)user;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) s_verr++;
    else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) s_vwarn++;
    else return VK_FALSE;
    if (s_vlog) fprintf(s_vlog, "%s: %s\n", (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARNING",
                        d->pMessage);
    return VK_FALSE;
}

/* ---- the programs: the engine's conversion, run again -------------------- */

typedef struct {
    int vs_hlsl_equal, ps_hlsl_equal, vs_spirv_equal, fs_spirv_equal, vi, fi;
    u32 nconst;
    u32* vs; u32 vs_words;
    u32* fs; u32 fs_words;
} programs;

static void make_programs(const rj* d, programs* out)
{
    memset(out, 0, sizeof *out);
    const rj* c = rj_get(d, "conversion");
    if (!c) die("%s: no conversion parameters", "desc.json");
    size_t vp_n, fp_n, sh_n, vh_n, ph_n;
    u8* vp = (u8*)load("vp.ucode", &vp_n);
    u8* fp = (u8*)load("fp.ucode", &fp_n);
    u8* sh = (u8*)load("shadow.bin", &sh_n);
    char* vh = (char*)load("vs.hlsl", &vh_n);
    char* ph = (char*)load("ps.hlsl", &ph_n);
    if (!vp || !fp || !vh || !ph) die("%s: programs missing", s_dir);
    rsx_fp_shadow_desc shadow[16];
    memset(shadow, 0, sizeof shadow);
    if (sh && sh_n == sizeof shadow) memcpy(shadow, sh, sizeof shadow);
    u8 gamma[16] = { 0 }, expand[16] = { 0 };
    u32 unnorm[16][3] = { { 0 } };
    for (int u = 0; u < 16; u++) {
        const rj* g = rj_at(rj_get(c, "gamma_masks"), u);
        const rj* e = rj_at(rj_get(c, "expand_masks"), u);
        const rj* un = rj_at(rj_get(c, "unnorm_dim"), u);
        gamma[u] = g ? (u8)g->num : 0;
        expand[u] = e ? (u8)e->num : 0;
        for (int k = 0; k < 3; k++) { const rj* x = rj_at(un, k); unnorm[u][k] = x ? (u32)x->num : 0; }
    }
    const rj* wp = rj_get(c, "wpos");
    const u32 wpos[2] = { rj_at(wp, 0) ? (u32)rj_at(wp, 0)->num : 1u, rj_at(wp, 1) ? (u32)rj_at(wp, 1)->num : 1u };
    static char vs[16384], ps[65536];
    /* rsx_draw_engine.c eng_programs_text, step for step. */
    rsx_vp_set_branch_bits((u32)rj_num(c, "vp_branch_bits", 0));
    int vi = rsx_vp_decompile_compact_ex(vp, (u32)vp_n, (u32)rj_num(c, "vtex_mask", 0),
                                         (u32)rj_num(c, "layout_mask", 0), vs, sizeof vs);
    const u32 ucp = (u32)rj_num(c, "ucp", 0);
    if (vi > 0 && ucp && rsx_vp_apply_user_clip(vs, sizeof vs, ucp) < 0) vi = -1;
    const u32 cube = (u32)rj_num(c, "cube_mask", 0), vol = (u32)rj_num(c, "vol_mask", 0);
    u32 nconst = 0;
    int fi = rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp, (u32)fp_n, (u32)rj_num(c, "fp_ctrl", 0),
            cube, vol, rj_num(c, "shadow_mask", 0) ? shadow : NULL, gamma,
            rj_num(c, "expand_units", 0) ? expand : NULL, ps, sizeof ps, &nconst);
    if (fi > 0 && (wpos[0] > 1u || wpos[1] > 1u) && rsx_fp_apply_wpos_grid(ps, sizeof ps, wpos[0], wpos[1]) < 0) fi = -1;
    if (fi > 0 && rsx_fp_apply_unnorm_scale(ps, sizeof ps, (u32)rj_num(c, "unnorm_mask", 0),
                                            (const u32 (*)[3])unnorm, cube, vol) < 0) fi = -1;
    if (fi > 0 && rj_num(c, "srgb_output", 0) && rsx_fp_apply_srgb_output(ps, sizeof ps) < 0) fi = -1;
    if (fi > 0 && rj_num(c, "alpha_test", 0) &&
        rsx_fp_apply_alpha_test_buffered(ps, sizeof ps, (u32)rj_num(c, "alpha_func", 0)) < 0) fi = -1;
    out->vi = vi; out->fi = fi; out->nconst = nconst;
    if (vi <= 0 || fi <= 0) die("%s: the programs did not convert", s_dir);
    save("replay_vs.hlsl", vs, strlen(vs));
    save("replay_ps.hlsl", ps, strlen(ps));
    out->vs_hlsl_equal = strlen(vs) == vh_n && !memcmp(vs, vh, vh_n);
    out->ps_hlsl_equal = strlen(ps) == ph_n && !memcmp(ps, ph, ph_n);
    /* SPIR-V, by the translator the engine uses (rsx_shader_spirv.cpp). */
    static u32 words[2][262144];
    char log[4096];
    for (int st = 0; st < 2; st++) {
        u32 n = 0;
        if (rsx_hlsl_to_spirv(st ? ps : vs, st ? RSX_SHADER_STAGE_FRAGMENT : RSX_SHADER_STAGE_VERTEX,
                              words[st], 262144, &n, log, sizeof log) || !n)
            die("glslang: %s", log);
        size_t cn;
        void* cap = load(st ? "fs.spv" : "vs.spv", &cn);
        const int eq = cap && cn == (size_t)n * 4u && !memcmp(cap, words[st], cn);
        free(cap);
        if (st) { out->fs = words[1]; out->fs_words = n; out->fs_spirv_equal = eq; }
        else    { out->vs = words[0]; out->vs_words = n; out->vs_spirv_equal = eq; }
    }
    save("replay_vs.spv", out->vs, (size_t)out->vs_words * 4u);
    save("replay_fs.spv", out->fs, (size_t)out->fs_words * 4u);
    free(vp); free(fp); free(sh); free(vh); free(ph);
}

/* ---- Vulkan ------------------------------------------------------------- */

static VkInstance s_inst;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_q;
static u32 s_qfam;
static VkCommandPool s_pool;
static VkPhysicalDeviceMemoryProperties s_mem;
static VkPhysicalDeviceProperties s_props;
static VkDebugUtilsMessengerEXT s_msgr;

#define CHECK(x, what) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "r1_replay: %s failed (%d)\n", what, (int)_r); exit(1); } } while (0)

static void vk_start(int validate)
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) die("%s", "no vulkan-1.dll");
    pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) die("%s", "no libvulkan.so.1");
    pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
    pvkCreateInstance = (PFN_vkCreateInstance)pvkGetInstanceProcAddr(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "r1_replay",
                              .apiVersion = VK_API_VERSION_1_1 };
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const char* exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkValidationFeatureEnableEXT en[] = { VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT };
    VkValidationFeaturesEXT vf = { .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                   .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = en };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .pNext = validate ? &vf : NULL,
                                .enabledLayerCount = validate ? 1u : 0u, .ppEnabledLayerNames = layers,
                                .enabledExtensionCount = validate ? 1u : 0u, .ppEnabledExtensionNames = exts };
    CHECK(pvkCreateInstance(&ci, NULL, &s_inst), validate ? "vkCreateInstance (validation layer)" : "vkCreateInstance");
#define LOADI(f) p##f = (PFN_##f)pvkGetInstanceProcAddr(s_inst, #f);
    VKF(LOADI)
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = on_message };
        CHECK(pvkCreateDebugUtilsMessengerEXT(s_inst, &mi, NULL, &s_msgr), "vkCreateDebugUtilsMessengerEXT");
    }
    u32 n = 8;
    VkPhysicalDevice pd[8];
    CHECK(pvkEnumeratePhysicalDevices(s_inst, &n, pd), "vkEnumeratePhysicalDevices");
    s_phys = VK_NULL_HANDLE;
    for (u32 i = 0; i < n && !s_phys; i++) {     /* a discrete GPU first, as the game's backend picks */
        pvkGetPhysicalDeviceProperties(pd[i], &s_props);
        if (s_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) s_phys = pd[i];
    }
    if (!s_phys) s_phys = pd[0];
    pvkGetPhysicalDeviceProperties(s_phys, &s_props);
    pvkGetPhysicalDeviceMemoryProperties(s_phys, &s_mem);
    VkQueueFamilyProperties qf[16];
    u32 nq = 16;
    pvkGetPhysicalDeviceQueueFamilyProperties(s_phys, &nq, qf);
    s_qfam = 0;
    while (s_qfam < nq && !(qf[s_qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) s_qfam++;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = s_qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi };
    CHECK(pvkCreateDevice(s_phys, &di, NULL, &s_dev), "vkCreateDevice");
#define LOADD(f) p##f = (PFN_##f)pvkGetDeviceProcAddr(s_dev, #f);
    VKD(LOADD)
    pvkGetDeviceQueue(s_dev, s_qfam, 0, &s_q);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s_qfam };
    CHECK(pvkCreateCommandPool(s_dev, &pi, NULL, &s_pool), "vkCreateCommandPool");
}

static u32 mem_type(u32 bits, VkMemoryPropertyFlags want)
{
    for (u32 i = 0; i < s_mem.memoryTypeCount; i++)
        if ((bits >> i & 1u) && (s_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    die("%s", "no memory type");
    return 0;
}

typedef struct { VkBuffer b; VkDeviceMemory m; void* p; VkDeviceSize n; } buf;

static buf make_buffer(VkDeviceSize n, VkBufferUsageFlags usage, const void* init)
{
    buf o = { 0 };
    o.n = n ? n : 16;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = o.n, .usage = usage };
    CHECK(pvkCreateBuffer(s_dev, &bi, NULL, &o.b), "vkCreateBuffer");
    VkMemoryRequirements r;
    pvkGetBufferMemoryRequirements(s_dev, o.b, &r);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
        .memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(pvkAllocateMemory(s_dev, &ai, NULL, &o.m), "vkAllocateMemory");
    CHECK(pvkBindBufferMemory(s_dev, o.b, o.m, 0), "vkBindBufferMemory");
    CHECK(pvkMapMemory(s_dev, o.m, 0, VK_WHOLE_SIZE, 0, &o.p), "vkMapMemory");
    if (init && n) memcpy(o.p, init, (size_t)n);
    return o;
}

static void free_buffer(buf* o)
{
    if (o->b) pvkDestroyBuffer(s_dev, o->b, NULL);
    if (o->m) pvkFreeMemory(s_dev, o->m, NULL);
    memset(o, 0, sizeof *o);
}

static VkCommandBuffer cmd_begin(void)
{
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s_pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK(pvkAllocateCommandBuffers(s_dev, &ai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(pvkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    return cb;
}

static void cmd_submit(VkCommandBuffer cb)
{
    CHECK(pvkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(pvkQueueSubmit(s_q, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    CHECK(pvkQueueWaitIdle(s_q), "vkQueueWaitIdle");
    pvkFreeCommandBuffers(s_dev, s_pool, 1, &cb);
}

static void image_barrier(VkCommandBuffer cb, VkImage img, u32 levels, u32 layers,
                          VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                          VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access,
        .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers } };
    pvkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

typedef struct {
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkFormat fmt; u32 w, h, levels, layers, bpp, block;
    VkDeviceSize bytes;
} image;

/* An image from a description's "image"/"target" entry, its pixels uploaded
 * from the file it names (the layout rsx_vulkan_backend.c writes: each level
 * and layer, 16-byte aligned). Left in `final`. */
static image make_image(const rj* e, VkImageUsageFlags usage, VkImageLayout final,
                        VkPipelineStageFlags use_stage, VkAccessFlags use_access)
{
    image o = { 0 };
    o.w = (u32)rj_num(e, "width", 0); o.h = (u32)rj_num(e, "height", 0);
    o.fmt = (VkFormat)rj_num(e, "format", 0);
    o.levels = (u32)rj_num(e, "levels", 1); o.layers = (u32)rj_num(e, "layers", 1);
    o.bpp = (u32)rj_num(e, "bytes_per_texel", 0); o.block = (u32)rj_num(e, "bytes_per_block", 0);
    const VkFormat vfmt = (VkFormat)rj_num(e, "view_format", o.fmt);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = o.layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
        .format = o.fmt, .extent = { o.w, o.h, 1 }, .mipLevels = o.levels, .arrayLayers = o.layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(pvkCreateImage(s_dev, &ci, NULL, &o.img), "vkCreateImage");
    VkMemoryRequirements r;
    pvkGetImageMemoryRequirements(s_dev, o.img, &r);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                                .memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK(pvkAllocateMemory(s_dev, &ai, NULL, &o.mem), "vkAllocateMemory(image)");
    CHECK(pvkBindImageMemory(s_dev, o.img, o.mem, 0), "vkBindImageMemory");
    const rj* sw = rj_get(e, "swizzle");
    VkComponentMapping map = { rj_at(sw, 0) ? (VkComponentSwizzle)rj_at(sw, 0)->num : VK_COMPONENT_SWIZZLE_IDENTITY,
                               rj_at(sw, 1) ? (VkComponentSwizzle)rj_at(sw, 1)->num : VK_COMPONENT_SWIZZLE_IDENTITY,
                               rj_at(sw, 2) ? (VkComponentSwizzle)rj_at(sw, 2)->num : VK_COMPONENT_SWIZZLE_IDENTITY,
                               rj_at(sw, 3) ? (VkComponentSwizzle)rj_at(sw, 3)->num : VK_COMPONENT_SWIZZLE_IDENTITY };
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o.img,
        .viewType = o.layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, .format = vfmt,
        .components = map, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, o.levels, 0, o.layers } };
    CHECK(pvkCreateImageView(s_dev, &vi, NULL, &o.view), "vkCreateImageView");
    /* the pixels */
    size_t n;
    void* px = load(rj_str(e, "file"), &n);
    if (!px) die("missing %s", rj_str(e, "file"));
    if (s_poison && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) memset(px, 0xA5, n);
    buf st = make_buffer(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, px);
    free(px);
    VkBufferImageCopy rg[96];
    u32 nr = 0;
    VkDeviceSize off = 0;
    for (u32 l = 0; l < o.layers; l++)
        for (u32 m = 0; m < o.levels && nr < 96; m++) {
            const u32 w = o.w >> m ? o.w >> m : 1u, h = o.h >> m ? o.h >> m : 1u;
            const VkDeviceSize b = o.block ? (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * o.block : (VkDeviceSize)w * h * o.bpp;
            rg[nr++] = (VkBufferImageCopy){ .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1 },
                                            .imageExtent = { w, h, 1 } };
            off += (b + 15u) & ~(VkDeviceSize)15u;
        }
    o.bytes = off;
    if (off != n) die("%s: size differs from the description", rj_str(e, "file"));
    VkCommandBuffer cb = cmd_begin();
    image_barrier(cb, o.img, o.levels, o.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    pvkCmdCopyBufferToImage(cb, st.b, o.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nr, rg);
    image_barrier(cb, o.img, o.levels, o.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, use_stage, use_access);
    cmd_submit(cb);
    free_buffer(&st);
    return o;
}

static void free_image(image* o)
{
    if (o->view) pvkDestroyImageView(s_dev, o->view, NULL);
    if (o->img) pvkDestroyImage(s_dev, o->img, NULL);
    if (o->mem) pvkFreeMemory(s_dev, o->mem, NULL);
    memset(o, 0, sizeof *o);
}

static VkShaderModule make_module(const u32* w, u32 n)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n * 4u, .pCode = w };
    VkShaderModule m;
    CHECK(pvkCreateShaderModule(s_dev, &ci, NULL, &m), "vkCreateShaderModule");
    return m;
}

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* ---- R2: a chain of passes, frame after frame ----------------------------
 *
 *   r1_replay --chain <capture dir> [--frames A-B] [--validate] [--poison-each-frame] [--hazard-selftest]
 *                     [--reps N --warmup N --time-frame F]
 *
 * Runs <capture dir>/chain.json (tools/r2_chain.py): every frame's copies
 * and draws in order, on images of the executor's own -- one per captured
 * resource. External inputs are seeded from each frame's capture, where
 * they arrive: at the frame's start, or mid-frame where work outside the
 * chain re-supplied them (asp's scene, rendered between PPFX's first pass
 * and the rest); history only at the first frame, and from then on carried
 * from the executor's own results. Everything else starts as 0xA5 bytes
 * (and, with --poison-each-frame, again at each frame's start, outputs
 * *_pe.bin), so whatever a pass reads, an earlier pass of the executor
 * produced, and a pixel no pass writes shows. Each image's state is
 * tracked (its layout, its last write, the reads since), and each access
 * gets exactly the barrier its hazard needs: read after write, write after
 * read, write after write, a layout change. Correctness runs read each
 * pass's output back right after the pass, before any other pass can
 * overwrite it (<draw dir>/native_after.bin), and each feedback copy right
 * after it is made (<frame dir>/native_copy_r<id>.bin); a resource carried
 * into the next frame is read back at the frame's end
 * (<next frame dir>/native_carried_r<id>.bin). Timing runs (--reps) record
 * no readbacks: the chain of --time-frame, each segment between seeds (pass
 * 1; passes 3-48) between a timestamp after a full barrier and one at its
 * end, copies and barriers included, the seeding outside them; after them,
 * one more correctness pass checks the repeated execution left the results
 * exact (native_after_repeated.bin). --hazard-selftest drops one dependency on
 * purpose (outputs *_hazard.bin): the check that "no errors" means the
 * validation layer looked. */

enum { R2_MAX_RID = 1024 };

typedef struct {
    int exists, role;               /* 0 external, 1 history, 2 snapshot, 3 intermediate */
    image im;
    VkImageLayout layout;
    VkPipelineStageFlags wstage, rstages;
    VkAccessFlags waccess;
    VkFramebuffer fb;
    VkImageView views[8]; VkFormat view_fmt[8]; VkComponentMapping view_map[8]; int nviews;
} r2_res;

typedef struct {
    int kind;                       /* 0 draw, 1 copy, 2 seed, 3 outside (nothing to do) */
    int src, dst;                   /* copy; seed: src, the resource */
    buf st;                         /* seed: its content */
    int target, tex[16], used[16];  /* draw */
    VkFormat view_fmt[16]; VkComponentMapping view_map[16];
    char dir[256];
    VkPipeline pipe; VkDescriptorSet set; VkSampler smp[16];
    buf ubo_v, ubo_p, vb, ib;
    VkViewport vp; VkRect2D sc; float blend_c[4]; u32 sref; float bias_c, bias_s;
    u32 vcount, icount;
    VkDeviceSize rb_off;
    int programs_equal;
} r2_op;

static r2_res s_rs[R2_MAX_RID];
static VkRenderPass s_rp_by_fmt[256]; static VkFormat s_rp_fmt[256]; static int s_nrp;
static VkDescriptorSetLayout s_r2_sl;
static VkPipelineLayout s_r2_pl;
static VkDescriptorPool s_r2_dp;
static image s_r2_dummy;
static VkSampler s_r2_dsmp;
static int s_r2_prog_total, s_r2_prog_equal;
static const char* s_r2_sfx = "";   /* "_pe": the --poison-each-frame run's outputs */

static VkRenderPass r2_render_pass(VkFormat f)
{
    for (int i = 0; i < s_nrp; i++) if (s_rp_fmt[i] == f) return s_rp_by_fmt[i];
    VkAttachmentDescription att = { .format = f, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                .pColorAttachments = &ar };
    /* Ordered by the explicit barriers recorded before each pass begins. */
    VkSubpassDependency dep[2] = {
        { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 } };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
    CHECK(pvkCreateRenderPass(s_dev, &ci, NULL, &s_rp_by_fmt[s_nrp]), "vkCreateRenderPass");
    s_rp_fmt[s_nrp] = f;
    return s_rp_by_fmt[s_nrp++];
}

/* One access to a resource, with the barrier its hazard needs. */
static void r2_access(VkCommandBuffer cb, r2_res* r, VkImageLayout want, VkPipelineStageFlags stage,
                      VkAccessFlags access, int write)
{
    VkPipelineStageFlags src = 0;
    VkAccessFlags srca = 0;
    int need = 0;
    if (r->layout != want) {          /* a transition writes: after every earlier access */
        src |= r->wstage | r->rstages; srca |= r->waccess; need = 1;
    } else if (write) {               /* WAW, WAR */
        if (r->wstage || r->rstages) { src |= r->wstage | r->rstages; srca |= r->waccess; need = 1; }
    } else if (r->wstage && !(r->rstages & stage)) {   /* RAW, once per reading stage */
        src |= r->wstage; srca |= r->waccess; need = 1;
    }
    /* --hazard-selftest: the first read of a pass's output after the colour
     * writes keeps its layout transition but loses its dependency on them --
     * a hazard synchronisation validation must report */
    if (s_hazard == 1 && need && !write && r->wstage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) {
        src = 0; srca = 0; s_hazard = 2;
    }
    if (need) image_barrier(cb, r->im.img, r->im.levels, r->im.layers, r->layout, want,
                            src ? src : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, srca, stage, access);
    if (write) { r->wstage = stage; r->waccess = access; r->rstages = 0; }
    else if (need && r->layout != want) { r->rstages = stage; }
    else r->rstages |= stage;
    r->layout = want;
}

static image r2_make_image(const rj* spec)
{
    image o = { 0 };
    o.w = (u32)rj_num(spec, "width", 0); o.h = (u32)rj_num(spec, "height", 0);
    o.fmt = (VkFormat)rj_num(spec, "format", 0);
    o.levels = (u32)rj_num(spec, "levels", 1); o.layers = (u32)rj_num(spec, "layers", 1);
    o.bpp = (u32)rj_num(spec, "bytes_per_texel", 0); o.block = (u32)rj_num(spec, "bytes_per_block", 0);
    const int attach = rj_num(spec, "drawn", 0) != 0;
    if (attach && (o.block || o.layers != 1 || o.levels != 1)) die("%s", "a drawn resource that is not a plain 2D image");
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = o.layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
        .format = o.fmt, .extent = { o.w, o.h, 1 }, .mipLevels = o.levels, .arrayLayers = o.layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 (attach ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(pvkCreateImage(s_dev, &ci, NULL, &o.img), "vkCreateImage");
    VkMemoryRequirements r;
    pvkGetImageMemoryRequirements(s_dev, o.img, &r);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                                .memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK(pvkAllocateMemory(s_dev, &ai, NULL, &o.mem), "vkAllocateMemory(image)");
    CHECK(pvkBindImageMemory(s_dev, o.img, o.mem, 0), "vkBindImageMemory");
    VkDeviceSize off = 0;
    for (u32 l = 0; l < o.layers; l++)
        for (u32 m = 0; m < o.levels; m++) {
            const u32 w = o.w >> m ? o.w >> m : 1u, h = o.h >> m ? o.h >> m : 1u;
            const VkDeviceSize b = o.block ? (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * o.block : (VkDeviceSize)w * h * o.bpp;
            off += (b + 15u) & ~(VkDeviceSize)15u;
        }
    o.bytes = off;
    return o;
}

/* Buffer <-> image regions in the capture's layout (every level and layer,
 * 16-byte aligned), at base. */
static u32 r2_regions(const image* o, VkDeviceSize base, VkBufferImageCopy* rg)
{
    u32 n = 0; VkDeviceSize off = base;
    for (u32 l = 0; l < o->layers; l++)
        for (u32 m = 0; m < o->levels && n < 96; m++) {
            const u32 w = o->w >> m ? o->w >> m : 1u, h = o->h >> m ? o->h >> m : 1u;
            const VkDeviceSize b = o->block ? (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * o->block : (VkDeviceSize)w * h * o->bpp;
            rg[n++] = (VkBufferImageCopy){ .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1 },
                                           .imageExtent = { w, h, 1 } };
            off += (b + 15u) & ~(VkDeviceSize)15u;
        }
    return n;
}

static VkImageView r2_view(r2_res* r, VkFormat f, VkComponentMapping map)
{
    for (int i = 0; i < r->nviews; i++)
        if (r->view_fmt[i] == f && !memcmp(&r->view_map[i], &map, sizeof map)) return r->views[i];
    if (r->nviews == 8) die("%s", "too many views of one resource");
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r->im.img,
        .viewType = r->im.layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, .format = f,
        .components = map, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, r->im.levels, 0, r->im.layers } };
    CHECK(pvkCreateImageView(s_dev, &vi, NULL, &r->views[r->nviews]), "vkCreateImageView");
    r->view_fmt[r->nviews] = f; r->view_map[r->nviews] = map;
    return r->views[r->nviews++];
}

/* A draw of the chain, from its description: programs remade and checked,
 * pipeline, buffers, samplers, descriptor set. */
static void r2_load_draw(const char* cap, r2_op* op)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", cap, op->dir);
    const char* keep = s_dir;
    s_dir = dir;
    size_t dn;
    char* text = (char*)load("desc.json", &dn);
    if (!text) die("no %s/desc.json", dir);
    char err[128];
    rj* d = rj_parse(text, err, sizeof err);
    if (!d) die("desc.json: %s", err);
    programs pr;
    make_programs(d, &pr);
    op->programs_equal = pr.vs_hlsl_equal && pr.ps_hlsl_equal && pr.vs_spirv_equal && pr.fs_spirv_equal;
    s_r2_prog_total++; s_r2_prog_equal += op->programs_equal;
    const rj* ds = rj_get(d, "depth_stencil");
    if (rj_num(ds, "depth_test", 0) || rj_num(ds, "depth_write", 0) || rj_num(ds, "stencil_test", 0) ||
        rj_num(ds, "depth_bounds_test", 0))
        die("%s: depth or stencil in use, not in the chains captured", dir);
    const rj* cst = rj_get(d, "constants");
    if (rj_num(rj_get(cst, "vertex"), "binding", 0) != 0 || rj_num(rj_get(cst, "fragment"), "binding", 1) != 1)
        die("%s: constants at other bindings than the chain's layout", dir);
    const rj* tg = rj_get(d, "target");
    const VkFormat tfmt = (VkFormat)rj_num(tg, "format", 0);
    /* textures: views and samplers */
    const rj* texs = rj_get(d, "textures");
    for (int i = 0; texs && i < texs->n; i++) {
        const rj* t = rj_at(texs, i);
        const int u = (int)rj_num(t, "unit", -1);
        if (u < 0 || u >= 16) continue;
        if (rj_num(t, "cube", 0)) die("%s: a cube texture, not in the chains captured", dir);
        if (rj_num(t, "binding", 16) != 16 || rj_num(t, "sampler_binding", 48) != 48)
            die("%s: textures at other bindings than the chain's layout", dir);
        const rj* im = rj_get(t, "image");
        const rj* sw = rj_get(im, "swizzle");
        op->view_fmt[u] = (VkFormat)rj_num(im, "view_format", rj_num(im, "format", 0));
        op->view_map[u] = (VkComponentMapping){
            rj_at(sw, 0) ? (VkComponentSwizzle)rj_at(sw, 0)->num : VK_COMPONENT_SWIZZLE_IDENTITY,
            rj_at(sw, 1) ? (VkComponentSwizzle)rj_at(sw, 1)->num : VK_COMPONENT_SWIZZLE_IDENTITY,
            rj_at(sw, 2) ? (VkComponentSwizzle)rj_at(sw, 2)->num : VK_COMPONENT_SWIZZLE_IDENTITY,
            rj_at(sw, 3) ? (VkComponentSwizzle)rj_at(sw, 3)->num : VK_COMPONENT_SWIZZLE_IDENTITY };
        const rj* s = rj_get(t, "sampler");
        const rj* a = rj_get(s, "address");
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = (VkFilter)rj_num(s, "mag_filter", 0), .minFilter = (VkFilter)rj_num(s, "min_filter", 0),
            .mipmapMode = (VkSamplerMipmapMode)rj_num(s, "mipmap_mode", 0),
            .addressModeU = (VkSamplerAddressMode)(rj_at(a, 0) ? rj_at(a, 0)->num : 0),
            .addressModeV = (VkSamplerAddressMode)(rj_at(a, 1) ? rj_at(a, 1)->num : 0),
            .addressModeW = (VkSamplerAddressMode)(rj_at(a, 2) ? rj_at(a, 2)->num : 0),
            .minLod = (float)rj_num(s, "min_lod", 0), .maxLod = (float)rj_num(s, "max_lod", 0),
            .borderColor = (VkBorderColor)rj_num(s, "border_color", 0) };
        CHECK(pvkCreateSampler(s_dev, &sci, NULL, &op->smp[u]), "vkCreateSampler");
    }
    /* constants, vertices */
    size_t vcn, pcn;
    void* vc = load(rj_str(rj_get(cst, "vertex"), "file"), &vcn);
    void* pc = load(rj_str(rj_get(cst, "fragment"), "file"), &pcn);
    op->ubo_v = make_buffer(vcn, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vc);
    op->ubo_p = make_buffer(pcn, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, pc);
    free(vc); free(pc);
    const rj* dr = rj_get(d, "draw");
    size_t vn, in_ = 0;
    void* vtx = load(rj_str(dr, "vertices"), &vn);
    void* idx = rj_str(dr, "indices") ? load(rj_str(dr, "indices"), &in_) : NULL;
    op->vb = make_buffer(vn, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vtx);
    op->ib = make_buffer(idx ? in_ : 16, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, idx);
    free(vtx); free(idx);
    op->vcount = (u32)rj_num(dr, "vertex_count", 0); op->icount = (u32)rj_num(dr, "index_count", 0);
    /* the pipeline */
    VkShaderModule vsm = make_module(pr.vs, pr.vs_words), fsm = make_module(pr.fs, pr.fs_words);
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
    const rj* vin = rj_get(d, "vertex_input");
    const rj* attrs = rj_get(vin, "attributes");
    VkVertexInputBindingDescription vbd = { 0, (u32)rj_num(vin, "stride", 0), (VkVertexInputRate)rj_num(vin, "rate", 0) };
    VkVertexInputAttributeDescription vad[16];
    const int na = attrs ? (attrs->n < 16 ? attrs->n : 16) : 0;
    for (int a = 0; a < na; a++) {
        const rj* x = rj_at(attrs, a);
        vad[a] = (VkVertexInputAttributeDescription){ (u32)rj_num(x, "location", 0), 0, (VkFormat)rj_num(x, "format", 0),
                                                      (u32)rj_num(x, "offset", 0) };
    }
    VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
        .vertexAttributeDescriptionCount = (u32)na, .pVertexAttributeDescriptions = vad };
    VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (VkPrimitiveTopology)rj_num(dr, "topology", 3) };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    const rj* ra = rj_get(d, "rasterization");
    VkPipelineRasterizationStateCreateInfo ras = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = (VkPolygonMode)rj_num(ra, "polygon_mode", 0), .cullMode = (VkCullModeFlags)rj_num(ra, "cull_mode", 0),
        .frontFace = (VkFrontFace)rj_num(ra, "front_face", 0), .depthBiasEnable = (VkBool32)rj_num(ra, "depth_bias_enable", 0),
        .lineWidth = (float)rj_num(ra, "line_width", 1) };
    VkPipelineMultisampleStateCreateInfo mss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    const rj* bl = rj_get(d, "blend");
    const rj* ba = rj_at(rj_get(bl, "attachments"), 0);
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = (VkBool32)rj_num(ba, "enable", 0),
        .srcColorBlendFactor = (VkBlendFactor)rj_num(ba, "src_color", 0), .dstColorBlendFactor = (VkBlendFactor)rj_num(ba, "dst_color", 0),
        .colorBlendOp = (VkBlendOp)rj_num(ba, "color_op", 0),
        .srcAlphaBlendFactor = (VkBlendFactor)rj_num(ba, "src_alpha", 0), .dstAlphaBlendFactor = (VkBlendFactor)rj_num(ba, "dst_alpha", 0),
        .alphaBlendOp = (VkBlendOp)rj_num(ba, "alpha_op", 0), .colorWriteMask = (VkColorComponentFlags)rj_num(ba, "write_mask", 15) };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    const VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dys = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 5, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps, .pRasterizationState = &ras,
        .pMultisampleState = &mss, .pColorBlendState = &cbs, .pDynamicState = &dys, .layout = s_r2_pl,
        .renderPass = r2_render_pass(tfmt) };
    CHECK(pvkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gci, NULL, &op->pipe), "vkCreateGraphicsPipelines");
    pvkDestroyShaderModule(s_dev, vsm, NULL); pvkDestroyShaderModule(s_dev, fsm, NULL);
    const rj* vpj = rj_get(d, "viewport");
    const rj* scj = rj_get(d, "scissor");
    op->vp = (VkViewport){ (float)rj_at(vpj, 0)->num, (float)rj_at(vpj, 1)->num, (float)rj_at(vpj, 2)->num,
                           (float)rj_at(vpj, 3)->num, (float)rj_at(vpj, 4)->num, (float)rj_at(vpj, 5)->num };
    op->sc = (VkRect2D){ { (int32_t)rj_at(scj, 0)->num, (int32_t)rj_at(scj, 1)->num },
                         { (u32)rj_at(scj, 2)->num, (u32)rj_at(scj, 3)->num } };
    const rj* bc = rj_get(bl, "constants");
    for (int i = 0; i < 4; i++) op->blend_c[i] = (float)rj_at(bc, i)->num;
    op->sref = (u32)rj_num(ds, "stencil_reference", 0);
    op->bias_c = (float)rj_num(ra, "depth_bias_constant", 0); op->bias_s = (float)rj_num(ra, "depth_bias_slope", 0);
    rj_free(d);
    free(text);
    s_dir = keep;
}

/* The draw's descriptor set: its resources' views (the images exist by now). */
static void r2_bind_draw(r2_op* op)
{
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s_r2_dp,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_r2_sl };
    CHECK(pvkAllocateDescriptorSets(s_dev, &dai, &op->set), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo bi[2] = { { op->ubo_v.b, 0, op->ubo_v.n }, { op->ubo_p.b, 0, op->ubo_p.n } };
    VkDescriptorImageInfo ii[16], si[16];
    for (int u = 0; u < 16; u++) {
        if (op->used[u] && op->view_fmt[u] != s_rs[op->tex[u]].im.fmt)
            die("%s: a view in another format, not in the chains captured", op->dir);
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
            op->used[u] ? r2_view(&s_rs[op->tex[u]], op->view_fmt[u], op->view_map[u]) : s_r2_dummy.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        si[u] = (VkDescriptorImageInfo){ op->used[u] ? op->smp[u] : s_r2_dsmp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = op->set, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = op->set, .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = op->set, .dstBinding = 16, .descriptorCount = 16,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = op->set, .dstBinding = 48, .descriptorCount = 16,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si } };
    pvkUpdateDescriptorSets(s_dev, 4, w, 0, NULL);
}

static void r2_record_draw(VkCommandBuffer cb, r2_op* op)
{
    for (int u = 0; u < 16; u++)
        if (op->used[u])
            r2_access(cb, &s_rs[op->tex[u]], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0);
    r2_res* t = &s_rs[op->target];
    r2_access(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 1);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r2_render_pass(t->im.fmt),
                                  .framebuffer = t->fb, .renderArea = { { 0, 0 }, { t->im.w, t->im.h } } };
    pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, op->pipe);
    pvkCmdSetViewport(cb, 0, 1, &op->vp); pvkCmdSetScissor(cb, 0, 1, &op->sc);
    pvkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, op->sref);
    pvkCmdSetDepthBias(cb, op->bias_c, 0.0f, op->bias_s); pvkCmdSetBlendConstants(cb, op->blend_c);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_r2_pl, 0, 1, &op->set, 0, NULL);
    VkDeviceSize zero = 0;
    pvkCmdBindVertexBuffers(cb, 0, 1, &op->vb.b, &zero);
    if (op->icount) { pvkCmdBindIndexBuffer(cb, op->ib.b, 0, VK_INDEX_TYPE_UINT32); pvkCmdDrawIndexed(cb, op->icount, 1, 0, 0, 0); }
    else pvkCmdDraw(cb, op->vcount, 1, 0, 0);
    pvkCmdEndRenderPass(cb);
}

static void r2_record_copy(VkCommandBuffer cb, r2_op* op)
{
    r2_res* s = &s_rs[op->src];
    r2_res* d = &s_rs[op->dst];
    r2_access(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0);
    r2_access(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    VkImageCopy c = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .extent = { s->im.w, s->im.h, 1 } };
    pvkCmdCopyImage(cb, s->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

/* A resource's whole content into buf at off (a diagnostic readback). */
static void r2_readback(VkCommandBuffer cb, r2_res* r, buf* rb, VkDeviceSize off)
{
    r2_access(cb, r, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0);
    VkBufferImageCopy rg[96];
    const u32 n = r2_regions(&r->im, off, rg);
    pvkCmdCopyImageToBuffer(cb, r->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb->b, n, rg);
}

/* Contents from a staging buffer into a resource (a seed, or poison). */
static void r2_fill(VkCommandBuffer cb, r2_res* r, buf* st, VkDeviceSize off)
{
    r2_access(cb, r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    VkBufferImageCopy rg[96];
    const u32 n = r2_regions(&r->im, off, rg);
    pvkCmdCopyBufferToImage(cb, st->b, r->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, rg);
}

static void r2_full_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* A frame's operations into cb. With rb, each output read back right after
 * its pass, before anything can overwrite it. With qp, each segment -- a run
 * of copies and draws between seeds, the content work outside the chain
 * supplies -- between a timestamp after a full barrier and one at its end,
 * so the seeds stay outside what is timed. Returns the number of segments;
 * with no cb, only counts them. */
static int r2_record_ops(VkCommandBuffer cb, r2_op* op, int nops, buf* rb, VkQueryPool qp)
{
    int seg = 0, open = 0;
    for (int k = 0; k < nops; k++) {
        if (op[k].kind == 3) continue;
        if (op[k].kind == 2) {
            if (open) {
                if (cb && qp) pvkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2u * (u32)seg + 1u);
                seg++; open = 0;
            }
            if (cb) r2_fill(cb, &s_rs[op[k].src], &op[k].st, 0);
            continue;
        }
        if (!open) {
            if (cb && qp) {
                r2_full_barrier(cb);
                pvkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 2u * (u32)seg);
            }
            open = 1;
        }
        if (!cb) continue;
        if (op[k].kind == 1) {
            r2_record_copy(cb, &op[k]);
            if (rb) r2_readback(cb, &s_rs[op[k].dst], rb, op[k].rb_off);
        } else {
            r2_record_draw(cb, &op[k]);
            if (rb) r2_readback(cb, &s_rs[op[k].target], rb, op[k].rb_off);
        }
    }
    if (open) {
        if (cb && qp) pvkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2u * (u32)seg + 1u);
        seg++;
    }
    return seg;
}

static int chain_main(int argc, char** argv)
{
    const char* cap = argv[2];
    int validate = 0, poison_each = 0, reps = 0, warmup = 50, tframe = 0, fa = 0, fb = 1 << 30;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--validate")) validate = 1;
        else if (!strcmp(argv[i], "--poison-each-frame")) { poison_each = 1; s_r2_sfx = "_pe"; }
        else if (!strcmp(argv[i], "--hazard-selftest")) { s_hazard = 1; s_r2_sfx = "_hazard"; }
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--time-frame") && i + 1 < argc) tframe = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { sscanf(argv[++i], "%d-%d", &fa, &fb); }
    }
    s_dir = cap;
    size_t cn;
    char* text = (char*)load("chain.json", &cn);
    if (!text) die("no %s/chain.json (tools/r2_chain.py)", cap);
    char err[128];
    rj* ch = rj_parse(text, err, sizeof err);
    if (!ch) die("chain.json: %s", err);
    if (validate) {
        char path[1024];
        snprintf(path, sizeof path, "%s/validation_chain%s%s.txt", cap, s_r2_sfx, reps ? "_timing" : "");
        s_vlog = fopen(path, "w");
    }
    vk_start(validate);

    /* the resources */
    const rj* res = rj_get(ch, "resources");
    VkDeviceSize biggest = 16;
    for (int i = 0; res && i < res->n; i++) {
        const int rid = atoi(res->keys[i]);
        if (rid < 0 || rid >= R2_MAX_RID) die("%s", "resource id out of range");
        const rj* spec = res->items[i];
        r2_res* r = &s_rs[rid];
        r->exists = 1;
        const char* role = rj_str(spec, "role");
        r->role = !strcmp(role, "external") ? 0 : !strcmp(role, "history") ? 1 : !strcmp(role, "snapshot") ? 2 : 3;
        r->im = r2_make_image(spec);
        r->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (r->im.bytes > biggest) biggest = r->im.bytes;
        if (rj_num(spec, "drawn", 0)) {
            VkImageView v = r2_view(r, r->im.fmt, (VkComponentMapping){ 0 });
            VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = r2_render_pass(r->im.fmt), .attachmentCount = 1, .pAttachments = &v,
                .width = r->im.w, .height = r->im.h, .layers = 1 };
            CHECK(pvkCreateFramebuffer(s_dev, &fbi, NULL, &r->fb), "vkCreateFramebuffer");
        }
    }
    /* the descriptor layout every draw's programs were translated for */
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding lb[4] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL }, { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { 16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 48, VK_DESCRIPTOR_TYPE_SAMPLER, 16, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = lb };
    CHECK(pvkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_r2_sl), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &s_r2_sl };
    CHECK(pvkCreatePipelineLayout(s_dev, &plci, NULL, &s_r2_pl), "vkCreatePipelineLayout");
    const rj* frames = rj_get(ch, "frames");
    const int nf = frames ? frames->n : 0;
    if (fb >= nf) fb = nf - 1;
    int nops_total = 0;
    for (int f = fa; f <= fb; f++) nops_total += rj_get(rj_at(frames, f), "ops")->n;
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2u * (u32)nops_total + 2 },
                                   { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16u * (u32)nops_total + 16 },
                                   { VK_DESCRIPTOR_TYPE_SAMPLER, 16u * (u32)nops_total + 16 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = (u32)nops_total + 1,
                                       .poolSizeCount = 3, .pPoolSizes = ps };
    CHECK(pvkCreateDescriptorPool(s_dev, &dpi, NULL, &s_r2_dp), "vkCreateDescriptorPool");
    static const char dummy_json[] = "{\"width\": 1, \"height\": 1, \"format\": 37, \"levels\": 1, \"layers\": 1, "
                                     "\"bytes_per_texel\": 4, \"file\": \"replay_dummy.bin\"}";
    const uint8_t dummy_px[16] = { 255, 0, 255, 255 };
    save("replay_dummy.bin", dummy_px, sizeof dummy_px);
    rj* dj = rj_parse(dummy_json, err, sizeof err);
    s_r2_dummy = make_image(dj, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkSamplerCreateInfo dsci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    CHECK(pvkCreateSampler(s_dev, &dsci, NULL, &s_r2_dsmp), "vkCreateSampler(dummy)");
    buf poison = make_buffer(biggest, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, NULL);
    memset(poison.p, 0xA5, (size_t)biggest);

    char rpath[1024];
    snprintf(rpath, sizeof rpath, "%s/replay_chain%s%s.json", cap, s_r2_sfx, reps ? "_timing" : "");
    FILE* out = fopen(rpath, "w");
    if (!out) die("cannot write %s", rpath);
    fprintf(out, "{\n  \"device\": \"%s\", \"frames\": [%d, %d], \"poison_each_frame\": %s,\n  \"frame_results\": [\n",
            s_props.deviceName, fa, fb, poison_each ? "true" : "false");
    int first_frame = 1;
    for (int f = fa; f <= fb; f++) {
        const rj* fr = rj_at(frames, f);
        const rj* ops = rj_get(fr, "ops");
        const int nops = ops->n;
        r2_op* op = (r2_op*)calloc((size_t)nops, sizeof(r2_op));
        VkDeviceSize rb_total = 0;
        for (int k = 0; k < nops; k++) {
            const rj* o = rj_at(ops, k);
            const char* kind = rj_str(o, "op");
            if (!strcmp(kind, "outside")) { op[k].kind = 3; continue; }
            if (!strcmp(kind, "seed")) {   /* rewritten outside the chain (asp's scene), read next */
                op[k].kind = 2; op[k].src = (int)rj_num(o, "resource", 0);
                size_t n;
                char* keep = (char*)s_dir; s_dir = cap;
                void* px = load(rj_str(o, "file"), &n);
                s_dir = keep;
                if (!px || n != s_rs[op[k].src].im.bytes) die("seed %s missing or of the wrong size", rj_str(o, "file"));
                op[k].st = make_buffer(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, px);
                free(px);
                continue;
            }
            if (!strcmp(kind, "copy")) {
                op[k].kind = 1; op[k].src = (int)rj_num(o, "src", 0); op[k].dst = (int)rj_num(o, "dst", 0);
                op[k].rb_off = rb_total; rb_total += s_rs[op[k].dst].im.bytes;
                continue;
            }
            snprintf(op[k].dir, sizeof op[k].dir, "%s", rj_str(o, "dir"));
            op[k].target = (int)rj_num(o, "target", 0);
            const rj* tx = rj_get(o, "textures");
            for (int i = 0; tx && i < tx->n; i++) {
                const int u = atoi(tx->keys[i]);
                if (u >= 0 && u < 16) { op[k].tex[u] = (int)tx->items[i]->num; op[k].used[u] = 1; }
            }
            r2_load_draw(cap, &op[k]);
            r2_bind_draw(&op[k]);
            op[k].rb_off = rb_total; rb_total += s_rs[op[k].target].im.bytes;
        }
        /* what this frame seeds; history only at the run's first frame */
        const rj* seed = rj_get(fr, "seed");
        buf seedbuf[64]; int seedrid[64]; int ns = 0;
        for (int i = 0; seed && i < seed->n && ns < 64; i++) {
            const int rid = atoi(seed->keys[i]);
            if (s_rs[rid].role == 1 && !first_frame) continue;   /* carried, not refreshed */
            size_t n;
            char* keep = (char*)s_dir; s_dir = cap;
            void* px = load(seed->items[i]->str, &n);
            s_dir = keep;
            if (!px || n != s_rs[rid].im.bytes) die("seed %s missing or of the wrong size", seed->items[i]->str);
            seedbuf[ns] = make_buffer(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, px);
            seedrid[ns++] = rid;
            free(px);
        }
        const rj* carried = rj_get(fr, "carried");
        buf rb = make_buffer(rb_total + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL);
        /* carried resources are read back at the end of the frame before (here: at this frame's start) */
        VkDeviceSize carry_off[64]; int ncarry = carried ? carried->n : 0;
        buf carrybuf = { 0 };
        if (ncarry && !first_frame) {
            VkDeviceSize tot = 0;
            for (int i = 0; i < ncarry && i < 64; i++) { carry_off[i] = tot; tot += s_rs[(int)rj_at(carried, i)->num].im.bytes; }
            carrybuf = make_buffer(tot, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL);
        }
        VkCommandBuffer cb = cmd_begin();
        if (ncarry && !first_frame)
            for (int i = 0; i < ncarry && i < 64; i++)
                r2_readback(cb, &s_rs[(int)rj_at(carried, i)->num], &carrybuf, carry_off[i]);
        /* poison: everything but carried history -- what is seeded at the
         * frame's start is then filled over it; an external input seeded
         * later in the frame (the scene target, which PPFX's first pass draws
         * into before asp renders over it) starts as poison too */
        if (first_frame || poison_each)
            for (int rid = 0; rid < R2_MAX_RID; rid++)
                if (s_rs[rid].exists && !(s_rs[rid].role == 1 && !first_frame))
                    r2_fill(cb, &s_rs[rid], &poison, 0);
        for (int i = 0; i < ns; i++) r2_fill(cb, &s_rs[seedrid[i]], &seedbuf[i], 0);
        r2_record_ops(cb, op, nops, &rb, VK_NULL_HANDLE);
        cmd_submit(cb);
        /* the outputs, each as it was right after its pass */
        int ndraws = 0, ncopies = 0;
        for (int k = 0; k < nops; k++) {
            char name[600];
            if (op[k].kind >= 2) continue;
            if (op[k].kind == 1)
            { snprintf(name, sizeof name, "%s/native_copy_r%d%s.bin", rj_str(fr, "dir"), op[k].dst, s_r2_sfx); ncopies++; }
            else { snprintf(name, sizeof name, "%s/native_after%s.bin", op[k].dir, s_r2_sfx); ndraws++; }
            char* keep = (char*)s_dir; s_dir = cap;
            save(name, (u8*)rb.p + op[k].rb_off, (size_t)(op[k].kind ? s_rs[op[k].dst].im.bytes : s_rs[op[k].target].im.bytes));
            s_dir = keep;
        }
        if (ncarry && !first_frame)
            for (int i = 0; i < ncarry && i < 64; i++) {
                const int rid = (int)rj_at(carried, i)->num;
                char name[600];
                snprintf(name, sizeof name, "%s/native_carried_r%d%s.bin", rj_str(fr, "dir"), rid, s_r2_sfx);
                char* keep = (char*)s_dir; s_dir = cap;
                save(name, (u8*)carrybuf.p + carry_off[i], (size_t)s_rs[rid].im.bytes);
                s_dir = keep;
            }
        fprintf(out, "%s   {\"frame\": %d, \"draws\": %d, \"copies\": %d, \"seeded\": %d, \"carried\": %d}",
                first_frame ? "" : ",\n", f, ndraws, ncopies, ns, first_frame ? 0 : ncarry);
        /* --reps: this frame's chain timed, then checked once more */
        if (reps > 0 && f == tframe) {
            const int nseg = r2_record_ops(VK_NULL_HANDLE, op, nops, NULL, VK_NULL_HANDLE);
            VkQueryPoolCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                          .queryCount = 2u * (u32)nseg };
            VkQueryPool qp;
            CHECK(pvkCreateQueryPool(s_dev, &qci, NULL, &qp), "vkCreateQueryPool");
            double* t = (double*)malloc(sizeof(double) * (size_t)reps * (size_t)(nseg + 1));   /* [seg][rep], then the sum */
            uint64_t ts[64];
            if (nseg > 32) die("%s", "too many segments to time");
            for (int r = -warmup; r < reps; r++) {
                VkCommandBuffer tb = cmd_begin();
                pvkCmdResetQueryPool(tb, qp, 0, 2u * (u32)nseg);
                for (int i = 0; i < ns; i++) r2_fill(tb, &s_rs[seedrid[i]], &seedbuf[i], 0);
                r2_record_ops(tb, op, nops, NULL, qp);
                cmd_submit(tb);
                CHECK(pvkGetQueryPoolResults(s_dev, qp, 0, 2u * (u32)nseg, sizeof(uint64_t) * 2u * (size_t)nseg, ts,
                                             sizeof ts[0], VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                      "vkGetQueryPoolResults");
                if (r < 0) continue;
                double total = 0;
                for (int s = 0; s < nseg; s++) {
                    const double us = (double)(ts[2 * s + 1] - ts[2 * s]) * s_props.limits.timestampPeriod * 1e-3;
                    t[(size_t)s * reps + r] = us;
                    total += us;
                }
                t[(size_t)nseg * reps + r] = total;
            }
            fprintf(out, ",\n   {\"timing_frame\": %d, \"repetitions\": %d, \"warmup\": %d, \"segments\": %d, \"chain_gpu_us\": [",
                    f, reps, warmup, nseg);
            for (int s = 0; s <= nseg; s++) {
                double* v = t + (size_t)s * reps;
                qsort(v, (size_t)reps, sizeof(double), cmp_double);
                double sum = 0;
                for (int i = 0; i < reps; i++) sum += v[i];
                fprintf(out, "%s\n     {\"%s\": %d, \"min\": %.3f, \"median\": %.3f, \"mean\": %.3f, \"p90\": %.3f, \"max\": %.3f}",
                        s ? "," : "", s < nseg ? "segment" : "total_of_segments", s < nseg ? s : nseg,
                        v[0], v[reps / 2], sum / reps, v[(int)(reps * 0.9)], v[reps - 1]);
            }
            /* after the repetitions: the chain once more, every pass read back */
            VkCommandBuffer cb2 = cmd_begin();
            for (int i = 0; i < ns; i++) r2_fill(cb2, &s_rs[seedrid[i]], &seedbuf[i], 0);
            r2_record_ops(cb2, op, nops, &rb, VK_NULL_HANDLE);
            cmd_submit(cb2);
            for (int k = 0; k < nops; k++) {
                if (op[k].kind != 0) continue;
                char name[600];
                snprintf(name, sizeof name, "%s/native_after_repeated.bin", op[k].dir);
                char* keep = (char*)s_dir; s_dir = cap;
                save(name, (u8*)rb.p + op[k].rb_off, (size_t)s_rs[op[k].target].im.bytes);
                s_dir = keep;
            }
            fprintf(out, "\n    ], \"repeated_outputs\": \"native_after_repeated.bin\"}");
            free(t);
            pvkDestroyQueryPool(s_dev, qp, NULL);
        }
        for (int i = 0; i < ns; i++) free_buffer(&seedbuf[i]);
        free_buffer(&rb);
        if (carrybuf.b) free_buffer(&carrybuf);
        for (int k = 0; k < nops; k++) {
            if (op[k].kind == 2) free_buffer(&op[k].st);
            if (op[k].kind != 0) continue;
            pvkDestroyPipeline(s_dev, op[k].pipe, NULL);
            for (int u = 0; u < 16; u++) if (op[k].smp[u]) pvkDestroySampler(s_dev, op[k].smp[u], NULL);
            free_buffer(&op[k].ubo_v); free_buffer(&op[k].ubo_p); free_buffer(&op[k].vb); free_buffer(&op[k].ib);
        }
        free(op);
        first_frame = 0;
        printf("r1_replay --chain: frame %d done\n", f);
    }
    fprintf(out, "\n  ],\n  \"programs\": {\"remade\": %d, \"identical\": %d},\n", s_r2_prog_total, s_r2_prog_equal);
    if (validate)
        fprintf(out, "  \"validation\": {\"layer\": \"VK_LAYER_KHRONOS_validation\", \"synchronization\": true, "
                     "\"errors\": %d, \"warnings\": %d},\n", s_verr, s_vwarn);
    fprintf(out, "  \"done\": true\n}\n");
    fclose(out);
    pvkDeviceWaitIdle(s_dev);
    for (int rid = 0; rid < R2_MAX_RID; rid++) {
        r2_res* r = &s_rs[rid];
        if (!r->exists) continue;
        if (r->fb) pvkDestroyFramebuffer(s_dev, r->fb, NULL);
        for (int i = 0; i < r->nviews; i++) pvkDestroyImageView(s_dev, r->views[i], NULL);
        pvkDestroyImage(s_dev, r->im.img, NULL);
        pvkFreeMemory(s_dev, r->im.mem, NULL);
    }
    for (int i = 0; i < s_nrp; i++) pvkDestroyRenderPass(s_dev, s_rp_by_fmt[i], NULL);
    free_image(&s_r2_dummy); pvkDestroySampler(s_dev, s_r2_dsmp, NULL);
    free_buffer(&poison);
    pvkDestroyDescriptorPool(s_dev, s_r2_dp, NULL);
    pvkDestroyPipelineLayout(s_dev, s_r2_pl, NULL);
    pvkDestroyDescriptorSetLayout(s_dev, s_r2_sl, NULL);
    pvkDestroyCommandPool(s_dev, s_pool, NULL);
    pvkDestroyDevice(s_dev, NULL);
    if (s_msgr) pvkDestroyDebugUtilsMessengerEXT(s_inst, s_msgr, NULL);
    pvkDestroyInstance(s_inst, NULL);
    if (s_vlog) fclose(s_vlog);
    rj_free(dj); rj_free(ch); free(text);
    printf("r1_replay --chain: %s: programs %d/%d identical%s\n", cap, s_r2_prog_equal, s_r2_prog_total,
           validate ? (s_verr || s_vwarn ? ", VALIDATION MESSAGES" : ", validation clean") : "");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--chain")) return chain_main(argc, argv);
    if (argc < 2) die("%s", "usage: r1_replay <capture dir> [--validate] [--reps N] [--warmup N] [--batches N]");
    s_dir = argv[1];
    int validate = 0, reps = 0, warmup = 50, batches = 10;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--validate")) validate = 1;
        else if (!strcmp(argv[i], "--poison")) s_poison = 1;
        else if (!strcmp(argv[i], "--hazard-selftest")) s_hazard = 1;
        else if (!strcmp(argv[i], "--attachment-inputs")) s_attach_inputs = 1;
        else if (!strcmp(argv[i], "--general-layouts")) s_general = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--batches") && i + 1 < argc) batches = atoi(argv[++i]);
    }
    size_t dn;
    char* text = (char*)load("desc.json", &dn);
    if (!text) die("no %s/desc.json", s_dir);
    char err[128];
    rj* d = rj_parse(text, err, sizeof err);
    if (!d) die("desc.json: %s", err);
    if (rj_get(d, "error")) die("the capture says: %s", rj_str(d, "error"));

    programs pr;
    make_programs(d, &pr);

    if (validate) {
        char path[1024];
        snprintf(path, sizeof path, "%s/validation.txt", s_dir);
        s_vlog = fopen(path, "w");
    }
    vk_start(validate);

    /* The depth attachment: R1's passes test and write no depth and no
     * stencil, so their pass has none. Anything else is outside R1. */
    const rj* ds = rj_get(d, "depth_stencil");
    if (rj_num(ds, "depth_test", 0) || rj_num(ds, "depth_write", 0) || rj_num(ds, "stencil_test", 0) ||
        rj_num(ds, "depth_bounds_test", 0))
        die("%s: depth or stencil in use, outside R1", s_dir);

    /* Resources. */
    const rj* tgt = rj_get(d, "target");
    image target = make_image(tgt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, L_ATT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    const rj* texs = rj_get(d, "textures");
    image tex[16];
    VkSampler smp[16];
    memset(tex, 0, sizeof tex); memset(smp, 0, sizeof smp);
    int used[16] = { 0 };
    u32 tex_binding = 16, smp_binding = 48;
    for (int i = 0; texs && i < texs->n; i++) {
        const rj* t = rj_at(texs, i);
        const int u = (int)rj_num(t, "unit", -1);
        if (u < 0 || u >= 16) continue;
        if (rj_num(t, "cube", 0)) die("%s: a cube texture, outside R1", s_dir);
        tex_binding = (u32)rj_num(t, "binding", 16); smp_binding = (u32)rj_num(t, "sampler_binding", 48);
        tex[u] = make_image(rj_get(t, "image"), VK_IMAGE_USAGE_SAMPLED_BIT | (s_attach_inputs ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
                            L_TEX,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        const rj* s = rj_get(t, "sampler");
        const rj* a = rj_get(s, "address");
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = (VkFilter)rj_num(s, "mag_filter", 0), .minFilter = (VkFilter)rj_num(s, "min_filter", 0),
            .mipmapMode = (VkSamplerMipmapMode)rj_num(s, "mipmap_mode", 0),
            .addressModeU = (VkSamplerAddressMode)(rj_at(a, 0) ? rj_at(a, 0)->num : 0),
            .addressModeV = (VkSamplerAddressMode)(rj_at(a, 1) ? rj_at(a, 1)->num : 0),
            .addressModeW = (VkSamplerAddressMode)(rj_at(a, 2) ? rj_at(a, 2)->num : 0),
            .minLod = (float)rj_num(s, "min_lod", 0), .maxLod = (float)rj_num(s, "max_lod", 0),
            .borderColor = (VkBorderColor)rj_num(s, "border_color", 0) };
        CHECK(pvkCreateSampler(s_dev, &sci, NULL, &smp[u]), "vkCreateSampler");
        used[u] = 1;
    }
    /* Units the draw binds nothing to get a 1x1 image and a nearest sampler
     * (the descriptor array has sixteen); a program that samples one reads
     * no data of the pass. */
    static const char dummy_json[] = "{\"width\": 1, \"height\": 1, \"format\": 37, \"levels\": 1, \"layers\": 1, "
                                     "\"bytes_per_texel\": 4, \"file\": \"replay_dummy.bin\"}";
    const uint8_t dummy_px[16] = { 255, 0, 255, 255 };
    save("replay_dummy.bin", dummy_px, sizeof dummy_px);
    rj* dj = rj_parse(dummy_json, err, sizeof err);
    image dummy = make_image(dj, VK_IMAGE_USAGE_SAMPLED_BIT, L_TEX,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkSamplerCreateInfo dsci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler dsmp;
    CHECK(pvkCreateSampler(s_dev, &dsci, NULL, &dsmp), "vkCreateSampler(dummy)");

    const rj* cst = rj_get(d, "constants");
    size_t vcn, pcn;
    void* vc = load(rj_str(rj_get(cst, "vertex"), "file"), &vcn);
    void* pc = load(rj_str(rj_get(cst, "fragment"), "file"), &pcn);
    buf ubo_v = make_buffer(vcn, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vc);
    buf ubo_p = make_buffer(pcn, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, pc);
    free(vc); free(pc);
    const rj* dr = rj_get(d, "draw");
    size_t vn, in_;
    void* vtx = load(rj_str(dr, "vertices"), &vn);
    void* idx = rj_str(dr, "indices") ? load(rj_str(dr, "indices"), &in_) : NULL;
    buf vb = make_buffer(vn, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vtx);
    buf ib = make_buffer(idx ? in_ : 16, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, idx);
    free(vtx); free(idx);

    /* The descriptor set: the layout the shaders were translated for. */
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding lb[4] = {
        { (u32)rj_num(rj_get(cst, "vertex"), "binding", 0), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { (u32)rj_num(rj_get(cst, "fragment"), "binding", 1), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { tex_binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { smp_binding, VK_DESCRIPTOR_TYPE_SAMPLER, 16, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = lb };
    VkDescriptorSetLayout sl;
    CHECK(pvkCreateDescriptorSetLayout(s_dev, &lci, NULL, &sl), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 },
                                   { VK_DESCRIPTOR_TYPE_SAMPLER, 16 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VkDescriptorPool dp;
    CHECK(pvkCreateDescriptorPool(s_dev, &dpi, NULL, &dp), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                        .descriptorSetCount = 1, .pSetLayouts = &sl };
    VkDescriptorSet set;
    CHECK(pvkAllocateDescriptorSets(s_dev, &dai, &set), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo bi[2] = { { ubo_v.b, 0, ubo_v.n }, { ubo_p.b, 0, ubo_p.n } };
    VkDescriptorImageInfo ii[16], si[16];
    for (int u = 0; u < 16; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, used[u] ? tex[u].view : dummy.view, L_TEX };
        si[u] = (VkDescriptorImageInfo){ used[u] ? smp[u] : dsmp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = lb[0].binding, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = lb[1].binding, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = tex_binding, .descriptorCount = 16,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = smp_binding, .descriptorCount = 16,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si } };
    pvkUpdateDescriptorSets(s_dev, 4, w, 0, NULL);
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl };
    VkPipelineLayout pl;
    CHECK(pvkCreatePipelineLayout(s_dev, &plci, NULL, &pl), "vkCreatePipelineLayout");

    /* The pass: one colour attachment, its content kept (LOAD), as the
     * guest's target is drawn over. */
    VkAttachmentDescription att = { .format = target.fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = L_ATT, .finalLayout = L_ATT };
    VkAttachmentReference ar = { 0, L_ATT };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkSubpassDependency dep[2] = {
        { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 } };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
                                   .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
    VkRenderPass rp;
    CHECK(pvkCreateRenderPass(s_dev, &rpi, NULL, &rp), "vkCreateRenderPass");
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                    .pAttachments = &target.view, .width = target.w, .height = target.h, .layers = 1 };
    VkFramebuffer fb;
    CHECK(pvkCreateFramebuffer(s_dev, &fbi, NULL, &fb), "vkCreateFramebuffer");

    /* The pipeline, from the description. */
    VkShaderModule vsm = make_module(pr.vs, pr.vs_words), fsm = make_module(pr.fs, pr.fs_words);
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
    const rj* vin = rj_get(d, "vertex_input");
    const rj* attrs = rj_get(vin, "attributes");
    VkVertexInputBindingDescription vbd = { 0, (u32)rj_num(vin, "stride", 0), (VkVertexInputRate)rj_num(vin, "rate", 0) };
    VkVertexInputAttributeDescription vad[16];
    const int na = attrs ? (attrs->n < 16 ? attrs->n : 16) : 0;
    for (int a = 0; a < na; a++) {
        const rj* x = rj_at(attrs, a);
        vad[a] = (VkVertexInputAttributeDescription){ (u32)rj_num(x, "location", 0), 0, (VkFormat)rj_num(x, "format", 0),
                                                      (u32)rj_num(x, "offset", 0) };
    }
    VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
        .vertexAttributeDescriptionCount = (u32)na, .pVertexAttributeDescriptions = vad };
    VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (VkPrimitiveTopology)rj_num(dr, "topology", 3) };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    const rj* ra = rj_get(d, "rasterization");
    VkPipelineRasterizationStateCreateInfo ras = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = (VkPolygonMode)rj_num(ra, "polygon_mode", 0), .cullMode = (VkCullModeFlags)rj_num(ra, "cull_mode", 0),
        .frontFace = (VkFrontFace)rj_num(ra, "front_face", 0), .depthBiasEnable = (VkBool32)rj_num(ra, "depth_bias_enable", 0),
        .lineWidth = (float)rj_num(ra, "line_width", 1) };
    VkPipelineMultisampleStateCreateInfo mss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    const rj* bl = rj_get(d, "blend");
    const rj* ba = rj_at(rj_get(bl, "attachments"), 0);
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = (VkBool32)rj_num(ba, "enable", 0),
        .srcColorBlendFactor = (VkBlendFactor)rj_num(ba, "src_color", 0), .dstColorBlendFactor = (VkBlendFactor)rj_num(ba, "dst_color", 0),
        .colorBlendOp = (VkBlendOp)rj_num(ba, "color_op", 0),
        .srcAlphaBlendFactor = (VkBlendFactor)rj_num(ba, "src_alpha", 0), .dstAlphaBlendFactor = (VkBlendFactor)rj_num(ba, "dst_alpha", 0),
        .alphaBlendOp = (VkBlendOp)rj_num(ba, "alpha_op", 0), .colorWriteMask = (VkColorComponentFlags)rj_num(ba, "write_mask", 15) };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    const VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dys = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 5, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps, .pRasterizationState = &ras,
        .pMultisampleState = &mss, .pColorBlendState = &cbs, .pDynamicState = &dys, .layout = pl, .renderPass = rp };
    VkPipeline pipe;
    CHECK(pvkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe), "vkCreateGraphicsPipelines");

    const rj* vpj = rj_get(d, "viewport");
    const rj* scj = rj_get(d, "scissor");
    const VkViewport vp = { (float)rj_at(vpj, 0)->num, (float)rj_at(vpj, 1)->num, (float)rj_at(vpj, 2)->num,
                            (float)rj_at(vpj, 3)->num, (float)rj_at(vpj, 4)->num, (float)rj_at(vpj, 5)->num };
    const VkRect2D sc = { { (int32_t)rj_at(scj, 0)->num, (int32_t)rj_at(scj, 1)->num },
                          { (u32)rj_at(scj, 2)->num, (u32)rj_at(scj, 3)->num } };
    const rj* bc = rj_get(bl, "constants");
    const float blend_c[4] = { (float)rj_at(bc, 0)->num, (float)rj_at(bc, 1)->num, (float)rj_at(bc, 2)->num, (float)rj_at(bc, 3)->num };
    const u32 vcount = (u32)rj_num(dr, "vertex_count", 0), icount = (u32)rj_num(dr, "index_count", 0);
    const u32 sref = (u32)rj_num(ds, "stencil_reference", 0);
    const float bias_c = (float)rj_num(ra, "depth_bias_constant", 0), bias_s = (float)rj_num(ra, "depth_bias_slope", 0);

#define RECORD_PASS(cb) do { \
        VkClearValue cv; memset(&cv, 0, sizeof cv); \
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, \
                                      .renderArea = { { 0, 0 }, { target.w, target.h } }, .clearValueCount = 0, .pClearValues = &cv }; \
        pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); \
        pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); \
        pvkCmdSetViewport(cb, 0, 1, &vp); pvkCmdSetScissor(cb, 0, 1, &sc); \
        pvkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sref); \
        pvkCmdSetDepthBias(cb, bias_c, 0.0f, bias_s); pvkCmdSetBlendConstants(cb, blend_c); \
        pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL); \
        VkDeviceSize zero = 0; pvkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &zero); \
        if (icount) { pvkCmdBindIndexBuffer(cb, ib.b, 0, VK_INDEX_TYPE_UINT32); pvkCmdDrawIndexed(cb, icount, 1, 0, 0, 0); } \
        else pvkCmdDraw(cb, vcount, 1, 0, 0); \
        pvkCmdEndRenderPass(cb); } while (0)

    /* The pass, once, and its output. */
    buf rb = make_buffer(target.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL);
    VkCommandBuffer cb = cmd_begin();
    RECORD_PASS(cb);
    /* --hazard-selftest: the same transition with no dependency on the
     * draw's writes, a read-after-write hazard synchronisation validation
     * must report -- the check that "no errors" means the layer looked. */
    image_barrier(cb, target.img, 1, 1, L_ATT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  s_hazard ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  s_hazard ? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy rr = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { target.w, target.h, 1 } };
    pvkCmdCopyImageToBuffer(cb, target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &rr);
    image_barrier(cb, target.img, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, L_ATT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    cmd_submit(cb);
    save(s_poison ? "native_output_poisoned.bin" : "native_output.bin", rb.p, (size_t)target.bytes);

    /* GPU time: --batches command buffers of --warmup untimed and --reps
     * timed repetitions; each ordered after the one before by a barrier on
     * the target (the dependency a chain of passes has). */
    double* t_us = NULL;
    int nt = 0;
    if (reps > 0) {
        VkQueryPoolCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                      .queryCount = (u32)reps * 2u };
        VkQueryPool qp;
        CHECK(pvkCreateQueryPool(s_dev, &qci, NULL, &qp), "vkCreateQueryPool");
        t_us = (double*)malloc(sizeof(double) * (size_t)reps * (size_t)batches);
        uint64_t* ts = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)reps * 2u);
        for (int bt = 0; bt < batches; bt++) {
            VkCommandBuffer tb = cmd_begin();
            pvkCmdResetQueryPool(tb, qp, 0, (u32)reps * 2u);
            for (int r = -warmup; r < reps; r++) {
                /* Into ALL_COMMANDS, so the TOP_OF_PIPE timestamp below waits
                 * for the repetition before to finish: each interval is one
                 * pass, measured as VK_GPU_PASSES measures the game's (a
                 * timestamp after a full barrier, before the pass begins). */
                image_barrier(tb, target.img, 1, 1, L_ATT, L_ATT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                if (r >= 0) pvkCmdWriteTimestamp(tb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, (u32)r * 2u);
                RECORD_PASS(tb);
                if (r >= 0) pvkCmdWriteTimestamp(tb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, (u32)r * 2u + 1u);
            }
            cmd_submit(tb);
            CHECK(pvkGetQueryPoolResults(s_dev, qp, 0, (u32)reps * 2u, sizeof(uint64_t) * (size_t)reps * 2u, ts,
                                         sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                  "vkGetQueryPoolResults");
            for (int r = 0; r < reps; r++)
                t_us[nt++] = (double)(ts[2 * r + 1] - ts[2 * r]) * s_props.limits.timestampPeriod * 1e-3;
        }
        free(ts);
        pvkDestroyQueryPool(s_dev, qp, NULL);
    }

    /* Results. */
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", s_dir, s_poison ? "replay_poisoned.json" : "replay.json");
    FILE* f = fopen(path, "w");
    if (!f) die("cannot write %s", path);
    fprintf(f, "{\n  \"device\": \"%s\", \"driver_version\": %u, \"api_version\": %u,\n", s_props.deviceName,
            s_props.driverVersion, s_props.apiVersion);
    fprintf(f, "  \"programs\": {\"vs_hlsl_equal\": %s, \"ps_hlsl_equal\": %s, \"vs_spirv_equal\": %s, "
               "\"fs_spirv_equal\": %s, \"vs_words\": %u, \"fs_words\": %u, \"nconst\": %u},\n",
            pr.vs_hlsl_equal ? "true" : "false", pr.ps_hlsl_equal ? "true" : "false",
            pr.vs_spirv_equal ? "true" : "false", pr.fs_spirv_equal ? "true" : "false",
            pr.vs_words, pr.fs_words, pr.nconst);
    fprintf(f, "  \"output\": \"%s\", \"output_bytes\": %llu, \"target_start\": \"%s\",\n",
            s_poison ? "native_output_poisoned.bin" : "native_output.bin", (unsigned long long)target.bytes,
            s_poison ? "0xA5 bytes" : "the captured content");
    if (validate)
        fprintf(f, "  \"validation\": {\"layer\": \"VK_LAYER_KHRONOS_validation\", \"synchronization\": true, "
                   "\"errors\": %d, \"warnings\": %d, \"log\": \"validation.txt\"},\n", s_verr, s_vwarn);
    if (nt) {
        qsort(t_us, (size_t)nt, sizeof(double), cmp_double);
        double sum = 0;
        for (int i = 0; i < nt; i++) sum += t_us[i];
        fprintf(f, "  \"gpu_time_us\": {\"repetitions\": %d, \"warmup_per_batch\": %d, \"batches\": %d, "
                   "\"min\": %.3f, \"median\": %.3f, \"mean\": %.3f, \"p90\": %.3f, \"max\": %.3f, "
                   "\"timestamp_period_ns\": %.3f},\n",
                nt, warmup, batches, t_us[0], t_us[nt / 2], sum / nt, t_us[(int)(nt * 0.9)], t_us[nt - 1],
                s_props.limits.timestampPeriod);
    }
    fprintf(f, "  \"done\": true\n}\n");
    fclose(f);

    /* Torn down in full, so the validation layer sees every object end. */
    pvkDeviceWaitIdle(s_dev);
    pvkDestroyPipeline(s_dev, pipe, NULL);
    pvkDestroyShaderModule(s_dev, vsm, NULL); pvkDestroyShaderModule(s_dev, fsm, NULL);
    pvkDestroyFramebuffer(s_dev, fb, NULL); pvkDestroyRenderPass(s_dev, rp, NULL);
    pvkDestroyPipelineLayout(s_dev, pl, NULL); pvkDestroyDescriptorPool(s_dev, dp, NULL);
    pvkDestroyDescriptorSetLayout(s_dev, sl, NULL);
    for (int u = 0; u < 16; u++) { if (used[u]) { free_image(&tex[u]); pvkDestroySampler(s_dev, smp[u], NULL); } }
    free_image(&dummy); pvkDestroySampler(s_dev, dsmp, NULL);
    free_image(&target);
    free_buffer(&rb); free_buffer(&ubo_v); free_buffer(&ubo_p); free_buffer(&vb); free_buffer(&ib);
    pvkDestroyCommandPool(s_dev, s_pool, NULL);
    pvkDestroyDevice(s_dev, NULL);
    if (s_msgr) pvkDestroyDebugUtilsMessengerEXT(s_inst, s_msgr, NULL);
    pvkDestroyInstance(s_inst, NULL);
    if (s_vlog) fclose(s_vlog);
    rj_free(dj);
    rj_free(d);
    free(text);
    free(t_us);
    printf("r1_replay: %s: programs %s/%s/%s/%s, output written%s\n", s_dir,
           pr.vs_hlsl_equal ? "vs-hlsl=" : "vs-hlsl!=", pr.ps_hlsl_equal ? "ps-hlsl=" : "ps-hlsl!=",
           pr.vs_spirv_equal ? "vs-spv=" : "vs-spv!=", pr.fs_spirv_equal ? "fs-spv=" : "fs-spv!=",
           validate ? (s_verr ? ", VALIDATION ERRORS" : ", validation clean of errors") : "");
    return 0;
}
