/* Copyright (C) 2018 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_VK_H_
#define MPV_CLIENT_API_RENDER_VK_H_

#include <vulkan/vulkan.h>

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Vulkan backend
 * --------------
 *
 * This backend requires the caller to have created its own VkInstance and
 * VkDevice, and to hand them to mpv via MPV_RENDER_PARAM_VULKAN_INIT_PARAMS.
 * Unlike the OpenGL backend, mpv does not own or manage the device.
 *
 * Use mpv_render_context_create() with MPV_RENDER_PARAM_API_TYPE set to
 * MPV_RENDER_API_TYPE_VULKAN, and MPV_RENDER_PARAM_VULKAN_INIT_PARAMS
 * provided.
 *
 * Call mpv_render_context_render() with MPV_RENDER_PARAM_VULKAN_IMAGE set to
 * render the video frame into a caller-owned VkImage. The caller is
 * responsible for all synchronization around that image via the semaphores
 * in mpv_vulkan_image -- mpv will wait on wait_semaphore before touching the
 * image, and signal signal_semaphore once rendering commands have been
 * submitted, but does not implicitly serialize anything else.
 *
 * mpv submits its rendering commands to queues from the graphics queue
 * family named in mpv_vulkan_init_params, with no locking around
 * vkQueueSubmit. Those queues must be for mpv's exclusive use; if the caller
 * submits to the same VkQueue objects from another thread, the behavior is
 * undefined.
 *
 * If mpv_render_context_render() returns an error, the state of
 * wait_semaphore/signal_semaphore is undefined: mpv may have waited on the
 * former without signaling the latter, or touched neither. Do not submit
 * work that waits on signal_semaphore before checking the return value, and
 * recreate both semaphores (and re-transition the image) before reusing
 * them after a failure.
 *
 * This backend additionally requires that
 * MPV_LIBMPV_RENDER_BACKEND=gpu-next is set in the environment before
 * mpv_render_context_create() is called, same as the OpenGL gpu-next
 * backend -- it is opt-in and does not affect any other libmpv embedder.
 */

/**
 * For initializing the mpv Vulkan state via MPV_RENDER_PARAM_VULKAN_INIT_PARAMS.
 *
 * mpv does not create or own any of these objects. They must outlive the
 * render context, and the caller remains responsible for destroying them
 * after mpv_render_context_free() returns.
 */
typedef struct mpv_vulkan_init_params {
    /**
     * Caller-owned Vulkan instance. Must have been created with at least
     * API version 1.3.
     */
    VkInstance instance;
    /**
     * Caller-owned physical device, must belong to `instance`.
     */
    VkPhysicalDevice phys_device;
    /**
     * Caller-owned logical device, must have been created from
     * `phys_device`.
     */
    VkDevice device;
    /**
     * Optional. If NULL, mpv will use the version of vkGetInstanceProcAddr
     * it was directly linked against.
     */
    PFN_vkGetInstanceProcAddr get_proc_address;
    /**
     * Queue family index providing VK_QUEUE_GRAPHICS_BIT, and the number of
     * queues in that family that mpv is allowed to use.
     */
    uint32_t queue_graphics_index;
    uint32_t queue_graphics_count;
    /**
     * Names of the device-level extensions the caller enabled on `device`.
     * mpv cannot otherwise discover this, since it did not create the
     * device itself, and some hwdec interop paths (e.g. CUDA<->Vulkan
     * external memory/semaphore import) are silently unusable without it.
     * Optional -- if NULL/0, hwdec paths that depend on external memory
     * interop will not work.
     */
    const char *const *enabled_extensions;
    int num_enabled_extensions;
} mpv_vulkan_init_params;

/**
 * For MPV_RENDER_PARAM_VULKAN_IMAGE.
 */
typedef struct mpv_vulkan_image {
    /**
     * Caller-owned target image. Must be usable as a color attachment
     * (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) and must have been created on
     * the same VkDevice passed via mpv_vulkan_init_params.
     */
    VkImage image;
    VkFormat format;
    int w, h;
    VkImageUsageFlags usage;
    /**
     * In: the image's layout at the time mpv_render_context_render() is
     * called. Out: updated in place to the layout mpv left the image in
     * once the call returns.
     */
    VkImageLayout layout;
    /**
     * Signaled by the caller once `image` is safe for mpv to access. May be
     * VK_NULL_HANDLE if no wait is needed (e.g. the image was just created).
     */
    VkSemaphore wait_semaphore;
    /**
     * Signaled by mpv once it has submitted all rendering commands that
     * touch `image`. Must not be VK_NULL_HANDLE.
     */
    VkSemaphore signal_semaphore;
} mpv_vulkan_image;

#ifdef __cplusplus
}
#endif

#endif
