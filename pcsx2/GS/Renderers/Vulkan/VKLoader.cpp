// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name name;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

void Vulkan::ResetVulkanLibraryFunctionPointers()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) name = nullptr;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

static DynamicLibrary s_vulkan_library;

#if defined(__ANDROID__)
// GE1 platform seam: the shipped Turnip is a Vulkan HAL, not a loader .so.
// Use the same HMI path proven by GE2's foreign-device present test. This is
// selected only by GE1_VK_TURNIP=1; the system loader remains the default.
struct Ge1HalModuleMethods { int (*open)(const void*, const char*, void**); };
struct Ge1HalModule {
	u32 tag;
	u16 moduleApiVersion;
	u16 halApiVersion;
	const char* id;
	const char* name;
	const char* author;
	Ge1HalModuleMethods* methods;
};
struct Ge1HalDevice {
	u32 tag;
	u32 version;
	void* module;
	u64 reserved[12];
	int (*close)(void*);
	void* enumerateInstanceExtensions;
	void* createInstance;
	PFN_vkGetInstanceProcAddr getInstanceProcAddr;
};
static Ge1HalDevice* s_turnip_hal_device = nullptr;

static void VKAPI_PTR Ge1TurnipDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
	auto* destroy = reinterpret_cast<PFN_vkDestroyInstance>(
		vkGetInstanceProcAddr(instance, "vkDestroyInstance"));
	if (destroy)
		destroy(instance, allocator);
}

static bool Ge1LoadTurnipHAL(Error* error)
{
	if (!s_vulkan_library.Open("libvulkan_freedreno.so", error))
		return false;
	Ge1HalModule* hmi = nullptr;
	if (!s_vulkan_library.GetSymbol("HMI", &hmi) || !hmi || hmi->tag != 0x48574d54u ||
		!hmi->methods || !hmi->methods->open)
	{
		std::fprintf(stderr, "GE1 Turnip: invalid HMI\n");
		s_vulkan_library.Close();
		return false;
	}
	void* device = nullptr;
	if (hmi->methods->open(hmi, "vulkan0", &device) != 0 || !device)
	{
		std::fprintf(stderr, "GE1 Turnip: HAL open failed\n");
		s_vulkan_library.Close();
		return false;
	}
	s_turnip_hal_device = static_cast<Ge1HalDevice*>(device);
	vkGetInstanceProcAddr = s_turnip_hal_device->getInstanceProcAddr;
	if (!vkGetInstanceProcAddr)
		return false;
	bool missing = false;
#define GE1_TURNIP_MODULE(name, required) \
	name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)); \
	if (!name && required) { std::fprintf(stderr, "GE1 Turnip: missing %s\n", #name); missing = true; }
	GE1_TURNIP_MODULE(vkCreateInstance, true)
	GE1_TURNIP_MODULE(vkEnumerateInstanceExtensionProperties, true)
	GE1_TURNIP_MODULE(vkEnumerateInstanceLayerProperties, true)
	GE1_TURNIP_MODULE(vkEnumerateInstanceVersion, false)
#undef GE1_TURNIP_MODULE
	vkDestroyInstance = &Ge1TurnipDestroyInstance;
	if (missing)
	{
		Vulkan::ResetVulkanLibraryFunctionPointers();
		s_vulkan_library.Close();
		s_turnip_hal_device = nullptr;
		return false;
	}
	std::fprintf(stderr, "GE1 Turnip: HAL selected\n");
	return true;
}
#endif

bool Vulkan::IsVulkanLibraryLoaded()
{
	return s_vulkan_library.IsOpen();
}

bool Vulkan::LoadVulkanLibrary(Error* error)
{
	pxAssertRel(!s_vulkan_library.IsOpen(), "Vulkan module is not loaded.");

#if defined(__ANDROID__)
	if (const char* turnip = std::getenv("GE1_VK_TURNIP"); turnip && std::strcmp(turnip, "1") == 0)
		return Ge1LoadTurnipHAL(error);
#endif

#ifdef __APPLE__
	// Check if a path to a specific Vulkan library has been specified.
	char* libvulkan_env = getenv("LIBVULKAN_PATH");
	if (libvulkan_env)
		s_vulkan_library.Open(libvulkan_env, error);
	if (!s_vulkan_library.IsOpen() &&
		!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("MoltenVK").c_str(), error))
	{
		return false;
	}
#else
	// try versioned first, then unversioned.
	if (!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str(), error) &&
		!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan").c_str(), error))
	{
		return false;
	}
#endif

	bool required_functions_missing = false;
#define VULKAN_MODULE_ENTRY_POINT(name, required) \
	if (!s_vulkan_library.GetSymbol(#name, &name)) \
	{ \
		ERROR_LOG("Vulkan: Failed to load required module function {}", #name); \
		required_functions_missing = true; \
	}

#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT

	if (required_functions_missing)
	{
		ResetVulkanLibraryFunctionPointers();
		s_vulkan_library.Close();
		return false;
	}

	return true;
}

void Vulkan::UnloadVulkanLibrary()
{
	ResetVulkanLibraryFunctionPointers();
	s_vulkan_library.Close();
#if defined(__ANDROID__)
	s_turnip_hal_device = nullptr; // GE2 HAL route leaves close to process teardown.
#endif
}

bool Vulkan::LoadVulkanInstanceFunctions(VkInstance instance)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, instance](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetInstanceProcAddr(instance, name);
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required instance function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_INSTANCE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

	return !required_functions_missing;
}

bool Vulkan::LoadVulkanDeviceFunctions(VkDevice device)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, device](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetDeviceProcAddr(device, name);
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required device function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_DEVICE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

	return !required_functions_missing;
}
