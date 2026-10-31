/* OpenCE's S5a and S5b probes (docs/ps5_port_plan.md, "M0. Feasibility
probes on the console"): OpenGL ES through Zink on RADV in a PS5 title.

  1. The guest's addresses below 4 GB are reserved first, as the game's
     title will reserve them (probe.c's S1), so that Mesa's allocations are
     seen to keep out of them.
  2. EGL's surfaceless platform with Zink (MESA_LOADER_DRIVER_OVERRIDE=zink)
     makes an OpenGL ES 3.0 context on a 3840x2160 surface; its strings must
     name Zink and RADV.
  3. A shader draws a gradient whose every pixel is known; glReadPixels
     reads it back and it is compared.
  4. S5a (the surface a pbuffer): for FRAMES frames, the same drawing (its
     blue moving) is read back into memory a Vulkan device of RADV's own
     copies into the images of a VK_KHR_display swapchain on VideoOut. The
     readback and the present are timed.
     S5b (built with PROBE_S5B; the surface a window on the display, which
     Zink's kopper presents to: port/ps5/patches/mesa/0005): for
     WINDOW_FRAMES frames the drawing is presented with eglSwapBuffers, and
     the frames' intervals are timed.

Every line goes to standard output (the console's klog) and to
opence_s5a.txt or opence_s5b.txt in the title's folder; Mesa's own messages
reach the klog through standard error. The last line counts the failures. */

#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
/* the commands are taken from RADV's entry points (vk_start) */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include "probe_abi.h"

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceSystemServiceHideSplashScreen(void);
/* RADV's archive (linked whole): the ICD entry a Vulkan loader would use */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

#ifdef PROBE_S5B
#define PROBE_NAME "S5b"
#define LOG_PATH "/app0/opence_s5b.txt"
#else
#define PROBE_NAME "S5a"
#define LOG_PATH "/app0/opence_s5a.txt"
#endif
#define WIDTH 3840
#define HEIGHT 2160
#define FRAMES 300
#define WINDOW_FRAMES 1200
#define MIB 0x100000ull
#define PAGE 0x4000ull

static FILE *log_file;
static int failures;

static void say(const char *format, ...)
{
	char line[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	printf("opence %s: %s\n", PROBE_NAME, line);
	fflush(stdout);
	if (log_file)
	{
		fprintf(log_file, "%s\n", line);
		fflush(log_file);
	}
}

static void verdict(int passed, const char *what)
{
	say("%s %s: %s", PROBE_NAME, passed ? "PASS" : "FAIL", what);
	if (!passed)
		failures++;
}

static uint64_t ticks(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* ---------- the guest's addresses (probe.c's S1, without backing them) */

static int range_free(uint64_t address, uint64_t size)
{
	uint64_t page;

	for (page = address & ~(PAGE - 1); page < address + size; page += PAGE)
	{
		void *start = NULL, *end = NULL;
		uint32_t protection = 0;

		if (sceKernelQueryMemoryProtection((void *)(uintptr_t)page, &start, &end, &protection) == 0
			&& (uint64_t)(uintptr_t)start <= page && (uint64_t)(uintptr_t)end > page)
			return 0;
	}
	return 1;
}

static int reserve(uint64_t address, uint64_t size)
{
	void *at = (void *)(uintptr_t)address;
	int result;

	if (!range_free(address, size))
		return -2;
	result = sceKernelReserveVirtualRange(&at, size, PS5_KERNEL_MAP_FIXED, 0);
	return result ? result : at == (void *)(uintptr_t)address ? 0 : -1;
}

static void reserve_guest(void)
{
	/* the window from the first 64 KiB boundary above the loader's libc.prx
	(port/ps5/FINDINGS.md, run1) */
	const uint64_t window_floor = 0x80120000ull;
	int image, window, low, high;

	image = reserve(PROBE_IMAGE_BASE, PROBE_IMAGE_RESERVE);
	window = reserve(window_floor, PROBE_WINDOW_BASE + PROBE_WINDOW_SIZE - window_floor);
	low = reserve(PROBE_POOL_LOW_BASE, PROBE_POOL_LOW_END - PROBE_POOL_LOW_BASE);
	high = reserve(PROBE_POOL_HIGH_BASE, 0x40000000ull);
	say("guest reservations: image %#x, window %#x, low pool %#x, high pool %#x (0 each when made)",
		(unsigned)image, (unsigned)window, (unsigned)low, (unsigned)high);
	verdict(!image && !window && !low && !high, "the guest's addresses below 4 GB reserved before Mesa starts");
}

/* ---------- OpenGL ES through EGL and Zink */

#define GL_FUNCTIONS(X) \
	X(PFNGLGETSTRINGPROC, glGetString) \
	X(PFNGLGETERRORPROC, glGetError) \
	X(PFNGLVIEWPORTPROC, glViewport) \
	X(PFNGLCLEARCOLORPROC, glClearColor) \
	X(PFNGLCLEARPROC, glClear) \
	X(PFNGLCREATESHADERPROC, glCreateShader) \
	X(PFNGLSHADERSOURCEPROC, glShaderSource) \
	X(PFNGLCOMPILESHADERPROC, glCompileShader) \
	X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
	X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
	X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
	X(PFNGLATTACHSHADERPROC, glAttachShader) \
	X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
	X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
	X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
	X(PFNGLUSEPROGRAMPROC, glUseProgram) \
	X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
	X(PFNGLUNIFORM1FPROC, glUniform1f) \
	X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
	X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
	X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
	X(PFNGLREADPIXELSPROC, glReadPixels) \
	X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
	X(PFNGLFINISHPROC, glFinish)

#define DECLARE(type, name) static type p_##name;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static EGLConfig chosen_config;
/* the surface's width and height: S5a's pbuffer is WIDTH x HEIGHT; S5b's
window, the native window of the PS5's display (patch 0005), is that or
S5B_SIZE=<width>x<height> from /app0/s5a-env.txt, one of VideoOut's sizes
(3840x2160, 2560x1440, 1920x1080). A display has one swapchain at a time,
and Zink destroys a window's after its GPU work, so a run measures one
size. */
static uint32_t surface_size[2] = { WIDTH, HEIGHT };
static GLuint program;
static GLint blue_location;

static const char vertex_source[] =
	"#version 300 es\n"
	"void main()\n"
	"{\n"
	"\tvec2 corner = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n"
	"\tgl_Position = vec4(corner, 0.0, 1.0);\n"
	"}\n";
/* red follows x, green y, blue the uniform; written as B, G, R, A so that the
bytes glReadPixels gives (RGBA) are those of a B8G8R8A8 swapchain image */
static const char fragment_source[] =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform float blue;\n"
	"out vec4 colour;\n"
	"void main()\n"
	"{\n"
	"\tvec3 c = vec3(floor(gl_FragCoord.x) / 4095.0, floor(gl_FragCoord.y) / 4095.0, blue);\n"
	"\tcolour = vec4(c.b, c.g, c.r, 1.0);\n"
	"}\n";

static GLuint compile(GLenum type, const char *source)
{
	GLuint shader = p_glCreateShader(type);
	GLint compiled = 0;
	char log[512] = "";

	p_glShaderSource(shader, 1, &source, NULL);
	p_glCompileShader(shader);
	p_glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled)
	{
		p_glGetShaderInfoLog(shader, sizeof log, NULL, log);
		say("shader: %s", log);
	}
	return compiled ? shader : 0;
}

/* the display, an OpenGL ES 3.0 context on a WIDTH x HEIGHT surface (a
pbuffer, or with window a window on the display) and the gradient's
program */
static int gl_start(int window)
{
	const EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, window ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE
	};
	static const EGLint pbuffer_attributes[] = { EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE };
	static const EGLint context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLint major = 0, minor = 0, configs = 0;
	const char *renderer, *version;
	GLuint vertex, fragment, array;
	GLint linked = 0;
	int loaded = 1;
	uint64_t start = ticks();

	display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
	{
		say("EGL: no display or eglInitialize failed: %#x", eglGetError());
		return 0;
	}
	say("EGL %d.%d initialised in %.1f ms: vendor %s, version %s, APIs %s", major, minor, (ticks() - start) / 1e6,
		eglQueryString(display, EGL_VENDOR), eglQueryString(display, EGL_VERSION),
		eglQueryString(display, EGL_CLIENT_APIS));
	if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(display, config_attributes, &chosen_config, 1, &configs)
		|| configs < 1)
	{
		say("EGL: no RGBA8, depth 24, stencil 8 %s configuration for OpenGL ES 3: %#x",
			window ? "window" : "pbuffer", eglGetError());
		return 0;
	}
	if (window)
		surface = eglCreatePlatformWindowSurface(display, chosen_config, surface_size, NULL);
	else
		surface = eglCreatePbufferSurface(display, chosen_config, pbuffer_attributes);
	context = eglCreateContext(display, chosen_config, EGL_NO_CONTEXT, context_attributes);
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
	{
		say("EGL: surface %p, context %p, make current failed: %#x", surface, context, eglGetError());
		return 0;
	}
	if (window && !eglSwapInterval(display, 1))
		say("EGL: eglSwapInterval(1) failed: %#x", eglGetError());
#define LOAD(type, name) \
	p_##name = (type)eglGetProcAddress(#name); \
	if (!p_##name) \
	{ \
		say("EGL: no %s", #name); \
		loaded = 0; \
	}
	GL_FUNCTIONS(LOAD)
#undef LOAD
	if (!loaded)
		return 0;
	renderer = (const char *)p_glGetString(GL_RENDERER);
	version = (const char *)p_glGetString(GL_VERSION);
	say("GL_VENDOR %s", (const char *)p_glGetString(GL_VENDOR));
	say("GL_RENDERER %s", renderer ? renderer : "(none)");
	say("GL_VERSION %s", version ? version : "(none)");
	say("GL_SHADING_LANGUAGE_VERSION %s", (const char *)p_glGetString(GL_SHADING_LANGUAGE_VERSION));
	say("context made in %.1f ms from the start of EGL", (ticks() - start) / 1e6);
	verdict(renderer && strstr(renderer, "zink") && strstr(renderer, "RADV"), "the context's renderer is Zink on RADV");

	vertex = compile(GL_VERTEX_SHADER, vertex_source);
	fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
	program = p_glCreateProgram();
	if (vertex && fragment)
	{
		p_glAttachShader(program, vertex);
		p_glAttachShader(program, fragment);
		p_glLinkProgram(program);
		p_glGetProgramiv(program, GL_LINK_STATUS, &linked);
	}
	if (!linked)
	{
		char log[512] = "";

		p_glGetProgramInfoLog(program, sizeof log, NULL, log);
		say("program: %s", log);
		return 0;
	}
	p_glUseProgram(program);
	blue_location = p_glGetUniformLocation(program, "blue");
	p_glGenVertexArrays(1, &array);
	p_glBindVertexArray(array);
	p_glViewport(0, 0, (GLsizei)surface_size[0], (GLsizei)surface_size[1]);
	p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
	return 1;
}

/* draws the gradient with this blue and reads it back into pixels (the
surface's size, 4 bytes a pixel); the readback's nanoseconds */
static uint64_t gl_frame(float blue, uint8_t *pixels)
{
	uint64_t start;

	p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	p_glClear(GL_COLOR_BUFFER_BIT);
	p_glUniform1f(blue_location, blue);
	p_glDrawArrays(GL_TRIANGLES, 0, 3);
	start = ticks();
	p_glReadPixels(0, 0, (GLsizei)surface_size[0], (GLsizei)surface_size[1], GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	return ticks() - start;
}

static uint8_t unorm8(float value)
{
	return (uint8_t)(value * 255.0f + 0.5f);
}

/* every 97th pixel of the frame against the gradient's formula: the bytes
are blue, green (y), red (x), alpha, within 1 of the exact value */
static int gl_check(const uint8_t *pixels, float blue, uint64_t *wrong_out)
{
	const uint64_t width = surface_size[0];
	uint64_t index, wrong = 0, first = UINT64_MAX;

	for (index = 0; index < width * surface_size[1]; index += 97)
	{
		unsigned x = (unsigned)(index % width), y = (unsigned)(index / width);
		const uint8_t *p = pixels + index * 4;

		if (abs(p[0] - unorm8(blue)) > 1 || abs(p[1] - unorm8(y / 4095.0f)) > 1
			|| abs(p[2] - unorm8(x / 4095.0f)) > 1 || p[3] != 255)
		{
			if (first == UINT64_MAX)
				first = index;
			wrong++;
		}
	}
	if (first != UINT64_MAX)
		say("first wrong pixel at (%llu, %llu): %02x %02x %02x %02x", (unsigned long long)(first % width),
			(unsigned long long)(first / width), pixels[first * 4], pixels[first * 4 + 1], pixels[first * 4 + 2],
			pixels[first * 4 + 3]);
	*wrong_out = wrong;
	return wrong == 0;
}

#ifndef PROBE_S5B
/* ---------- S5a's presentation: RADV's VK_KHR_display on VideoOut */

#define VK_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkCreateDevice) \
	X(vkGetDeviceProcAddr) \
	X(vkGetPhysicalDeviceDisplayPropertiesKHR) \
	X(vkGetDisplayModePropertiesKHR) \
	X(vkCreateDisplayPlaneSurfaceKHR) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkDestroySurfaceKHR)

#define VK_DEVICE_FUNCTIONS(X) \
	X(vkGetDeviceQueue) \
	X(vkCreateSwapchainKHR) \
	X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR) \
	X(vkQueuePresentKHR) \
	X(vkQueueSubmit) \
	X(vkCreateBuffer) \
	X(vkGetBufferMemoryRequirements) \
	X(vkAllocateMemory) \
	X(vkBindBufferMemory) \
	X(vkMapMemory) \
	X(vkCreateCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkResetCommandBuffer) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdCopyBufferToImage) \
	X(vkCreateSemaphore) \
	X(vkCreateFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkDeviceWaitIdle) \
	X(vkDestroySwapchainKHR) \
	X(vkDestroyDevice)

#define DECLARE(name) static PFN_##name name;
VK_INSTANCE_FUNCTIONS(DECLARE)
VK_DEVICE_FUNCTIONS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance;
#undef DECLARE

static struct
{
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkImage images[8];
	uint32_t image_count;
	VkBuffer staging;
	uint8_t *staging_memory;
	VkCommandBuffer commands;
	VkSemaphore acquired, copied;
	VkFence done;
} vk;

static int vk_check(VkResult result, const char *what)
{
	if (result != VK_SUCCESS)
		say("Vulkan: %s: %d", what, (int)result);
	return result == VK_SUCCESS;
}

static int vk_start(void)
{
	static const char *const instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
	static const char *const device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	const VkApplicationInfo application = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "OpenCE S5a", .apiVersion = VK_API_VERSION_1_3,
	};
	const VkInstanceCreateInfo instance_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application,
		.enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_extensions,
	};
	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
		.pQueuePriorities = &priority,
	};
	const VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = device_extensions,
	};
	VkDisplayPropertiesKHR display_properties;
	VkDisplayModePropertiesKHR modes[4];
	VkSurfaceCapabilitiesKHR capabilities;
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceMemoryProperties memory_properties;
	VkMemoryRequirements requirements;
	VkBool32 supported = VK_FALSE;
	VkDeviceMemory memory;
	VkCommandPool pool;
	uint32_t count = 1, type;
	int loaded = 1;

	vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
	if (!vkCreateInstance || !vk_check(vkCreateInstance(&instance_info, NULL, &vk.instance), "vkCreateInstance"))
		return 0;
#define LOAD(name) \
	name = (PFN_##name)vk_icdGetInstanceProcAddr(vk.instance, #name); \
	loaded &= name != NULL;
	VK_INSTANCE_FUNCTIONS(LOAD)
#undef LOAD
	if (!loaded || !vk_check(vkEnumeratePhysicalDevices(vk.instance, &count, &vk.physical), "a physical device"))
		return 0;
	vkGetPhysicalDeviceProperties(vk.physical, &properties);
	say("presenting Vulkan device: %s, API %u.%u", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
		VK_API_VERSION_MINOR(properties.apiVersion));
	if (!vk_check(vkCreateDevice(vk.physical, &device_info, NULL, &vk.device),
			"vkCreateDevice beside Zink's device"))
		return 0;
#define LOAD(name) \
	name = (PFN_##name)vkGetDeviceProcAddr(vk.device, #name); \
	loaded &= name != NULL;
	VK_DEVICE_FUNCTIONS(LOAD)
#undef LOAD
	if (!loaded)
	{
		say("Vulkan: a device command is missing");
		return 0;
	}
	vkGetDeviceQueue(vk.device, 0, 0, &vk.queue);

	count = 1;
	if (!vk_check(vkGetPhysicalDeviceDisplayPropertiesKHR(vk.physical, &count, &display_properties), "the display"))
		return 0;
	count = 4;
	if (vkGetDisplayModePropertiesKHR(vk.physical, display_properties.display, &count, modes) < 0 || !count)
	{
		say("Vulkan: no display mode");
		return 0;
	}
	say("display %s, %ux%u, first mode %.3f Hz", display_properties.displayName,
		display_properties.physicalResolution.width, display_properties.physicalResolution.height,
		modes[0].parameters.refreshRate / 1000.0);
	{
		const VkDisplaySurfaceCreateInfoKHR surface_info = {
			.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR, .displayMode = modes[0].displayMode,
			.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
			.imageExtent = { WIDTH, HEIGHT },
		};

		if (!vk_check(vkCreateDisplayPlaneSurfaceKHR(vk.instance, &surface_info, NULL, &vk.surface), "the surface"))
			return 0;
	}
	vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, 0, vk.surface, &supported);
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities);
	if (!supported || !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
	{
		say("Vulkan: the surface is not supported, or its images cannot be copied to (usage %#x)",
			capabilities.supportedUsageFlags);
		return 0;
	}
	{
		const VkSwapchainCreateInfoKHR swapchain_info = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk.surface,
			.minImageCount = capabilities.minImageCount > 3 ? capabilities.minImageCount : 3,
			.imageFormat = VK_FORMAT_B8G8R8A8_UNORM, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
			.imageExtent = { WIDTH, HEIGHT }, .imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
			.clipped = VK_TRUE,
		};

		if (!vk_check(vkCreateSwapchainKHR(vk.device, &swapchain_info, NULL, &vk.swapchain), "the swapchain"))
			return 0;
	}
	vk.image_count = 8;
	if (!vk_check(vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, vk.images), "its images"))
		return 0;

	{
		const VkBufferCreateInfo buffer_info = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)WIDTH * HEIGHT * 4,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		if (!vk_check(vkCreateBuffer(vk.device, &buffer_info, NULL, &vk.staging), "the staging buffer"))
			return 0;
	}
	vkGetBufferMemoryRequirements(vk.device, vk.staging, &requirements);
	vkGetPhysicalDeviceMemoryProperties(vk.physical, &memory_properties);
	for (type = 0; type < memory_properties.memoryTypeCount; type++)
	{
		VkMemoryPropertyFlags flags = memory_properties.memoryTypes[type].propertyFlags;

		if ((requirements.memoryTypeBits & (1u << type)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
			&& (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			break;
	}
	{
		const VkMemoryAllocateInfo allocate_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type,
		};
		void *mapped = NULL;

		if (type == memory_properties.memoryTypeCount
			|| !vk_check(vkAllocateMemory(vk.device, &allocate_info, NULL, &memory), "staging memory")
			|| !vk_check(vkBindBufferMemory(vk.device, vk.staging, memory, 0), "binding it")
			|| !vk_check(vkMapMemory(vk.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "mapping it"))
			return 0;
		vk.staging_memory = mapped;
		say("staging buffer: %llu MiB host-visible at %p", (unsigned long long)(requirements.size / MIB), mapped);
	}
	{
		const VkCommandPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0,
		};
		const VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		const VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkCommandBufferAllocateInfo command_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};

		if (!vk_check(vkCreateCommandPool(vk.device, &pool_info, NULL, &pool), "the command pool"))
			return 0;
		command_info.commandPool = pool;
		if (!vk_check(vkAllocateCommandBuffers(vk.device, &command_info, &vk.commands), "the command buffer")
			|| !vk_check(vkCreateSemaphore(vk.device, &semaphore_info, NULL, &vk.acquired), "a semaphore")
			|| !vk_check(vkCreateSemaphore(vk.device, &semaphore_info, NULL, &vk.copied), "a semaphore")
			|| !vk_check(vkCreateFence(vk.device, &fence_info, NULL, &vk.done), "a fence"))
			return 0;
	}
	say("swapchain: %u images of %ux%u B8G8R8A8, FIFO", vk.image_count, WIDTH, HEIGHT);
	return 1;
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access,
	VkAccessFlags target_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags target_stage)
{
	const VkImageMemoryBarrier image_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = source_access, .dstAccessMask = target_access,
		.oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};

	vkCmdPipelineBarrier(vk.commands, source_stage, target_stage, 0, 0, NULL, 0, NULL, 1, &image_barrier);
}

/* the staging buffer into the next swapchain image, presented */
static int vk_present(void)
{
	const VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	/* GL's rows run bottom-up, the image's top-down: the picture is shown
	upside down, which a gradient makes plain and which does not matter here */
	const VkBufferImageCopy region = {
		.bufferRowLength = WIDTH, .bufferImageHeight = HEIGHT,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { WIDTH, HEIGHT, 1 },
	};
	const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.acquired,
		.pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &vk.commands,
		.signalSemaphoreCount = 1, .pSignalSemaphores = &vk.copied,
	};
	VkPresentInfoKHR present = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.copied,
		.swapchainCount = 1, .pSwapchains = &vk.swapchain,
	};
	uint32_t index;

	if (!vk_check(vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE, &index),
			"acquire"))
		return 0;
	vkResetCommandBuffer(vk.commands, 0);
	vkBeginCommandBuffer(vk.commands, &begin);
	barrier(vk.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyBufferToImage(vk.commands, vk.staging, vk.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	barrier(vk.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	vkEndCommandBuffer(vk.commands);
	vkResetFences(vk.device, 1, &vk.done);
	if (!vk_check(vkQueueSubmit(vk.queue, 1, &submit, vk.done), "submit"))
		return 0;
	present.pImageIndices = &index;
	if (!vk_check(vkQueuePresentKHR(vk.queue, &present), "present"))
		return 0;
	/* the staging buffer is written again for the next frame */
	return vk_check(vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, UINT64_MAX), "the copy's fence");
}
#endif

/* NAME=VALUE lines of /app0/s5a-env.txt (MESA_DEBUG, ZINK_DEBUG, RADV_DEBUG),
set before Mesa starts: a run changes them without a new build */
static void read_environment_file(void)
{
	FILE *file = fopen("/app0/s5a-env.txt", "r");
	char line[256];

	while (file && fgets(line, sizeof line, file))
	{
		char *equals = strchr(line, '=');

		line[strcspn(line, "\r\n")] = 0;
		if (equals && line[0] != '#')
		{
			*equals = 0;
			setenv(line, equals + 1, 1);
			say("environment: %s=%s", line, equals + 1);
		}
	}
	if (file)
		fclose(file);
}

/* what RADV offers before Zink asks: the instance version and extensions */
static void vk_survey(void)
{
	PFN_vkEnumerateInstanceVersion version_of =
		(PFN_vkEnumerateInstanceVersion)vk_icdGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
	PFN_vkEnumerateInstanceExtensionProperties extensions_of = (PFN_vkEnumerateInstanceExtensionProperties)
		vk_icdGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
	PFN_vkVoidFunction layers_of = vk_icdGetInstanceProcAddr(NULL, "vkEnumerateInstanceLayerProperties");
	VkExtensionProperties extensions[64];
	uint32_t version = 0, count = 64, index;
	char names[2048] = "";

	if (version_of)
		version_of(&version);
	if (extensions_of)
		extensions_of(NULL, &count, extensions);
	else
		count = 0;
	for (index = 0; index < count; index++)
	{
		strncat(names, " ", sizeof names - strlen(names) - 1);
		strncat(names, extensions[index].extensionName, sizeof names - strlen(names) - 1);
	}
	say("RADV: instance version %u.%u.%u, vkEnumerateInstanceLayerProperties %s, %u instance extensions:%s",
		VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version),
		layers_of ? "present" : "absent", count, names);
}

#ifdef PROBE_S5B
/* WINDOW_FRAMES frames of the gradient on the current window surface,
width x height, presented with eglSwapBuffers: 1 if every swap succeeded */
static int swap_run(const char *label, unsigned width, unsigned height)
{
	uint64_t start, previous, interval, interval_max = 0, interval_min = UINT64_MAX, mean;
	int frame, swapped = 0;

	p_glViewport(0, 0, (GLsizei)width, (GLsizei)height);
	start = previous = ticks();
	for (frame = 0; frame < WINDOW_FRAMES; frame++)
	{
		p_glClear(GL_COLOR_BUFFER_BIT);
		p_glUniform1f(blue_location, (float)(frame % 120) / 119.0f);
		p_glDrawArrays(GL_TRIANGLES, 0, 3);
		if (!eglSwapBuffers(display, surface))
		{
			say("%s: eglSwapBuffers failed at frame %d: %#x", label, frame, eglGetError());
			break;
		}
		interval = ticks() - previous;
		previous += interval;
		/* the first 60 frames start the swapchain */
		if (frame >= 60)
		{
			interval_max = interval > interval_max ? interval : interval_max;
			interval_min = interval < interval_min ? interval : interval_min;
		}
		swapped++;
	}
	mean = swapped ? (previous - start) / swapped : 0;
	say("%s %ux%u: %d frames swapped in %.2f s (%.2f fps); mean interval %.3f ms, after frame 60 min %.2f ms, "
		"max %.2f ms", label, width, height, swapped, (previous - start) / 1e9, swapped / ((previous - start) / 1e9),
		mean / 1e6, interval_min / 1e6, interval_max / 1e6);
	/* FIFO at 119.88 Hz at most: frames faster than 7 ms each were not shown
	(a swap without a swapchain returns at once) */
	return swapped == WINDOW_FRAMES && mean >= 7000000;
}
#endif

int main(void)
{
	uint8_t *check_pixels = malloc((size_t)WIDTH * HEIGHT * 4);
	uint64_t wrong = 0;
	int gl_ok, presented = 0;

	log_file = fopen(LOG_PATH, "w");
	say("OpenCE PS5 %s probe: Zink on RADV (docs/ps5_port_plan.md), log %s", PROBE_NAME, LOG_PATH);
	say("standard error to klog: %s", ps5_klog_capture_stderr("[mesa] ") == 0 ? "captured" : "not captured");
	setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
	setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
	setenv("EGL_LOG_LEVEL", "info", 1);
	read_environment_file();
	if (getenv("S5B_SIZE"))
		sscanf(getenv("S5B_SIZE"), "%ux%u", &surface_size[0], &surface_size[1]);

	reserve_guest();
	vk_survey();
#ifdef PROBE_S5B
	gl_ok = check_pixels && gl_start(1);
	verdict(gl_ok, "EGL window surface on the display, OpenGL ES 3.0 context on Zink, shaders built");
#else
	gl_ok = check_pixels && gl_start(0);
	verdict(gl_ok, "EGL surfaceless pbuffer, OpenGL ES 3.0 context on Zink, shaders built");
#endif
	if (gl_ok)
	{
		uint64_t nanoseconds = gl_frame(0.5f, check_pixels);
		GLenum error = p_glGetError();

		say("first frame read back in %.1f ms, GL error %#x", nanoseconds / 1e6, error);
		verdict(error == GL_NO_ERROR && gl_check(check_pixels, 0.5f, &wrong), "the drawn gradient read back exactly");
		say("pixels checked wrong: %llu", (unsigned long long)wrong);
	}
#ifdef PROBE_S5B
	if (gl_ok)
	{
		sceSystemServiceHideSplashScreen();
		presented = swap_run("window", surface_size[0], surface_size[1]);
	}
	verdict(presented, "1200 frames drawn by Zink and presented by kopper on VideoOut at the display's pace");
#else
	if (gl_ok && vk_start())
	{
		uint64_t start, readback = 0, readback_max = 0;
		int frame;

		sceSystemServiceHideSplashScreen();
		start = ticks();
		for (frame = 0; frame < FRAMES; frame++)
		{
			uint64_t nanoseconds = gl_frame((float)(frame % 120) / 119.0f, vk.staging_memory);

			readback += nanoseconds;
			if (nanoseconds > readback_max)
				readback_max = nanoseconds;
			if (!vk_present())
				break;
			presented++;
		}
		say("%d frames presented in %.2f s (%.2f fps); readback mean %.2f ms, max %.2f ms", presented,
			(ticks() - start) / 1e9, presented / ((ticks() - start) / 1e9), presented ? readback / 1e6 / presented : 0.0,
			readback_max / 1e6);
		vkDeviceWaitIdle(vk.device);
	}
	verdict(presented == FRAMES, "300 frames drawn by Zink, read back, and presented by RADV on VideoOut");
#endif
	say("done: %d failed check(s)", failures);
	if (log_file)
		fclose(log_file);
	sceSystemServiceLoadExec("exit", NULL);
	for (;;)
		usleep(100000);
	return 0;
}
