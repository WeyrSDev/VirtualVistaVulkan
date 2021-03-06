
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <chrono>
#include <array>

#include "VulkanRenderer.h"
#include "Settings.h"
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

#ifdef _WIN32
    #define NOMINMAX  
    #include <Windows.h>
#endif

namespace vv
{
	VkResult createDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT* pCreateInfo,
		const VkAllocationCallbacks* pAllocator, VkDebugReportCallbackEXT* pCallback)
	{
		auto func = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
		if (func != nullptr)
			return func(instance, pCreateInfo, pAllocator, pCallback);
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}


	void destroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback, const VkAllocationCallbacks* pAllocator)
	{
#ifdef _DEBUG
		auto func = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT");
		if (func != nullptr)
			func(instance, callback, pAllocator);
#endif
	}


	///////////////////////////////////////////////////////////////////////////////////////////// Public
	VulkanRenderer::VulkanRenderer()
	{
	}


	VulkanRenderer::~VulkanRenderer()
	{
	}


	void VulkanRenderer::create()
	{
		try
		{
	        window_ = new GLFWWindow;
            window_->create();

			createVulkanInstance();
			window_->createSurface(instance_);

			setupDebugCallback();
			createVulkanDevices();

			swap_chain_ = new VulkanSwapChain();
			swap_chain_->create(physical_device_, window_);

			render_pass_ = new VulkanRenderPass();
			render_pass_->create(physical_device_, swap_chain_);

			createFrameBuffers();

			image_ready_semaphore_ = util::createVulkanSemaphore(this->physical_device_->logical_device);
			rendering_complete_semaphore_ = util::createVulkanSemaphore(this->physical_device_->logical_device);

            scene_ = new Scene();
            scene_->create(physical_device_, render_pass_);
		}
		catch (const std::runtime_error& e)
		{
			throw e;
		}
	}


	void VulkanRenderer::shutDown()
	{
		// accounts for the issue of a logical device that might be executing commands when a terminating command is issued.
		vkDeviceWaitIdle(physical_device_->logical_device);

	  	vkDestroySemaphore(physical_device_->logical_device, image_ready_semaphore_, nullptr);
	  	vkDestroySemaphore(physical_device_->logical_device, rendering_complete_semaphore_, nullptr);

        scene_->shutDown();

	  	render_pass_->shutDown(); delete render_pass_;

	  	for (std::size_t j = 0; j < frame_buffers_.size(); ++j)
	  		vkDestroyFramebuffer(physical_device_->logical_device, frame_buffers_[j], nullptr);

	  	swap_chain_->shutDown(physical_device_); delete swap_chain_;

	  	physical_device_->shutDown(); delete physical_device_;
        window_->shutDown(instance_); delete window_;
		destroyDebugReportCallbackEXT(instance_, debug_callback_, nullptr);
		vkDestroyInstance(instance_, nullptr);
	}


	void VulkanRenderer::run(float delta_time)
	{
		// Poll window specific updates and input.
		window_->run();

        scene_->updateUniformData(swap_chain_->extent, delta_time);

		// Draw Frame
		/// Acquire an image from the swap chain
		uint32_t image_index = 0;
		swap_chain_->acquireNextImage(physical_device_, image_ready_semaphore_, image_index);

		VkSubmitInfo submit_info = {};
		submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		/// tell the queue to wait until a command buffer successfully attaches a swap chain image as a color attachment (wait until its ready to begin rendering).
		std::array<VkPipelineStageFlags, 1> wait_stages = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &image_ready_semaphore_;
		submit_info.pWaitDstStageMask = wait_stages.data();

		/// Set the command buffer that will be used to rendering to be the one we waited for.
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers = &command_buffers_[image_index];

		/// Detail the semaphore that marks when rendering is complete.
		std::array<VkSemaphore, 1> signal_semaphores = { rendering_complete_semaphore_ };
		submit_info.signalSemaphoreCount = 1;
		submit_info.pSignalSemaphores = signal_semaphores.data();

		VV_CHECK_SUCCESS(vkQueueSubmit(physical_device_->graphics_queue, 1, &submit_info, VK_NULL_HANDLE));
		swap_chain_->queuePresent(physical_device_->graphics_queue, image_index, rendering_complete_semaphore_);
	}


    void VulkanRenderer::recordCommandBuffers()
    {
        command_buffers_.resize(frame_buffers_.size());

        VkCommandBufferAllocateInfo command_buffer_allocate_info = {};
        command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_buffer_allocate_info.commandPool = physical_device_->command_pools["graphics"];

        // primary can be sent to pool for execution, but cant be called from other buffers. secondary cant be sent to pool, but can be called from other buffers.
        command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; 
        command_buffer_allocate_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());

        VV_CHECK_SUCCESS(vkAllocateCommandBuffers(physical_device_->logical_device, &command_buffer_allocate_info, command_buffers_.data()));

        std::vector<VkClearValue> clear_values;
        VkClearValue color_value, depth_value;
        color_value.color = { 0.3f, 0.5f, 0.5f, 1.0f };
        depth_value.depthStencil = {1.0f, 0};
        clear_values.push_back(color_value);
        clear_values.push_back(depth_value);

        scene_->allocateSceneDescriptorSets();

		for (std::size_t i = 0; i < command_buffers_.size(); ++i)
		{
            VkCommandBufferBeginInfo command_buffer_begin_info = {};
            command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT; // <- tells how long this buffer will be executed
            command_buffer_begin_info.pInheritanceInfo = nullptr; // for if this is a secondary buffer
            VV_CHECK_SUCCESS(vkBeginCommandBuffer(command_buffers_[i], &command_buffer_begin_info));

            render_pass_->beginRenderPass(command_buffers_[i], VK_SUBPASS_CONTENTS_INLINE, frame_buffers_[i], swap_chain_->extent, clear_values);

            scene_->render(command_buffers_[i]);

            render_pass_->endRenderPass(command_buffers_[i]);
            VV_CHECK_SUCCESS(vkEndCommandBuffer(command_buffers_[i]));
		}
    }


    Scene* VulkanRenderer::getScene() const
    {
        return scene_;
    }


	bool VulkanRenderer::shouldStop()
	{
		// todo: add other conditions
		return window_->shouldClose();
	}


	///////////////////////////////////////////////////////////////////////////////////////////// Private
	void VulkanRenderer::createVulkanInstance()
	{
		VV_ASSERT(checkValidationLayerSupport(), "Validation layers requested are not available on this system.");

		// Instance Creation
		std::string application_name = Settings::inst()->getApplicationName();
		std::string engine_name = Settings::inst()->getEngineName();

		VkApplicationInfo app_info = {};
		app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app_info.pApplicationName = application_name.c_str();
		app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		app_info.pEngineName = engine_name.c_str();
		app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		app_info.apiVersion = VK_API_VERSION_1_0;

		// Ensure required extensions are found.
		VV_ASSERT(checkInstanceExtensionSupport(), "Extensions requested, but are not available on this system.");

		VkInstanceCreateInfo instance_create_info = {};
		instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_create_info.pApplicationInfo = &app_info;

		auto required_extensions = getRequiredExtensions();
        instance_create_info.enabledExtensionCount = static_cast<uint32_t>(required_extensions.size());
		instance_create_info.ppEnabledExtensionNames = required_extensions.data();

#ifdef _DEBUG
		instance_create_info.enabledLayerCount = static_cast<uint32_t>(used_validation_layers_.size());
		instance_create_info.ppEnabledLayerNames = const_cast<const char* const*>(used_validation_layers_.data());
#else
		instance_create_info.enabledLayerCount = 0;
#endif

		VV_CHECK_SUCCESS(vkCreateInstance(&instance_create_info, nullptr, &instance_));
	}


	std::vector<const char*> VulkanRenderer::getRequiredExtensions()
	{
		std::vector<const char*> extensions;

		// GLFW specific
		for (uint32_t i = 0; i < window_->glfw_extension_count; i++)
			extensions.push_back(window_->glfw_extensions[i]);

		// System wide required (hardcoded)
		for (auto extension : used_instance_extensions_)
			extensions.push_back(extension);

		return extensions;
	}


	bool VulkanRenderer::checkInstanceExtensionSupport()
	{
		std::vector<const char*> required_extensions = getRequiredExtensions();

		uint32_t extension_count = 0;
		VV_CHECK_SUCCESS(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr));
		std::vector<VkExtensionProperties> available_extensions(extension_count);
		VV_CHECK_SUCCESS(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extensions.data()));

		// Compare found extensions with requested ones.
		for (const auto& extension : required_extensions)
		{
			bool extension_found = false;
			for (const auto& found_extension : available_extensions)
				if (strcmp(extension, found_extension.extensionName) == 0)
				{
					extension_found = true;
					break;
				}

			if (!extension_found) return false;
		}

		return true;
	}


	bool VulkanRenderer::checkValidationLayerSupport()
	{
		uint32_t layer_count = 0;
		VV_CHECK_SUCCESS(vkEnumerateInstanceLayerProperties(&layer_count, nullptr));
		std::vector<VkLayerProperties> available_layers(layer_count);
		VV_CHECK_SUCCESS(vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data()));

		// Compare found layers with requested ones.
		for (const char* layer : used_validation_layers_)
		{
			bool layer_found = false;
			for (const auto& found_layer : available_layers)
				if (strcmp(layer, found_layer.layerName) == 0)
				{
					layer_found = true;
					break;
				}

			if (!layer_found) return false;
		}

		return true;
	}


    VKAPI_ATTR VkBool32 VKAPI_CALL
		vulkanDebugCallback(VkDebugReportFlagsEXT flags,
			VkDebugReportObjectTypeEXT obj_type, // object that caused the error
			uint64_t src_obj,
			size_t location,
			int32_t msg_code,
			const char *layer_prefix,
			const char *msg,
			void *usr_data)
	{
		std::ostringstream stream;
		if (flags & VK_DEBUG_REPORT_WARNING_BIT_EXT)
			stream << "WARNING: ";
		if (flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT)
			stream << "PERFORMANCE: ";
		if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
			stream << "ERROR: ";
		if (flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT)
			stream << "DEBUG: ";

		stream << "@[" << layer_prefix << "]" << std::endl;
		stream << msg << std::endl;
		std::cout << stream.str() << std::endl;

#ifdef _WIN32
		if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
			MessageBox(NULL, stream.str().c_str(), "VirtualVista Vulkan Error", 0);
#endif

		return false;
	}


	void VulkanRenderer::setupDebugCallback()
	{
#ifdef _DEBUG

		VkDebugReportCallbackCreateInfoEXT debug_callback_create_info = {};
		debug_callback_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
		debug_callback_create_info.pfnCallback = vulkanDebugCallback;
		debug_callback_create_info.flags = VK_DEBUG_REPORT_INFORMATION_BIT_EXT |
			VK_DEBUG_REPORT_WARNING_BIT_EXT |
			VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
			VK_DEBUG_REPORT_ERROR_BIT_EXT |
			VK_DEBUG_REPORT_DEBUG_BIT_EXT |
			VK_DEBUG_REPORT_FLAG_BITS_MAX_ENUM_EXT;

		VV_CHECK_SUCCESS(createDebugReportCallbackEXT(instance_, &debug_callback_create_info, nullptr, &debug_callback_));
#endif
	}


	void VulkanRenderer::createVulkanDevices()
	{
		uint32_t physical_device_count = 0;
		vkEnumeratePhysicalDevices(instance_, &physical_device_count, nullptr);

		VV_ASSERT(physical_device_count != 0, "Vulkan Error: no gpu with Vulkan support found");

		std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
		vkEnumeratePhysicalDevices(instance_, &physical_device_count, physical_devices.data());

		// Find any physical devices that might be suitable for on screen rendering.
		for (const auto& device : physical_devices)
		{
            physical_device_ = new VulkanDevice;
            physical_device_->create(device);
			VulkanSurfaceDetailsHandle surface_details_handle = {};
			if (physical_device_->isSuitable(window_->surface, surface_details_handle))
			{
				physical_device_->createLogicalDevice(true, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT);
				window_->surface_settings[physical_device_] = surface_details_handle;
				break;
			}
			else
			{
				physical_device_->shutDown();
				delete physical_device_;
                physical_device_ = nullptr;
			}
		}

		VV_ASSERT(physical_device_ != nullptr, "Vulkan Error: no gpu with Vulkan support found");
	}


	void VulkanRenderer::createFrameBuffers()
	{
		frame_buffers_.resize(swap_chain_->color_image_views.size());

		for (std::size_t i = 0; i < swap_chain_->color_image_views.size(); ++i)
		{
			std::vector<VkImageView> attachments = { swap_chain_->color_image_views[i]->image_view, swap_chain_->depth_image_view->image_view };

			VkFramebufferCreateInfo frame_buffer_create_info = {};
			frame_buffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			frame_buffer_create_info.flags = 0;
			frame_buffer_create_info.renderPass = render_pass_->render_pass;
			frame_buffer_create_info.attachmentCount = (uint32_t)attachments.size();
			frame_buffer_create_info.pAttachments = attachments.data();
			frame_buffer_create_info.width = swap_chain_->extent.width;
			frame_buffer_create_info.height = swap_chain_->extent.height;
			frame_buffer_create_info.layers = 1;

			VV_CHECK_SUCCESS(vkCreateFramebuffer(physical_device_->logical_device, &frame_buffer_create_info, nullptr, &frame_buffers_[i]));
		}
	}
}