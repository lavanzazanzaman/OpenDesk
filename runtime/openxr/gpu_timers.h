#pragma once

#include "pch.h"

namespace opendesk_openxr::utils {

    // An asynchronous GPU timer for Direct3D 11.
    struct VulkanGpuTimer : public ITimer {
        VulkanGpuTimer(const VulkanDispatch& dispatch,
                       VkPhysicalDevice physicalDevice,
                       VkDevice device,
                       VkQueue queue,
                       uint32_t queueFamilyIndex,
                       const std::optional<VkAllocationCallbacks>& allocator)
            : m_dispatch(dispatch), m_device(device), m_queue(queue), m_allocator(allocator) {
            // Query the timestamp period.
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            m_dispatch.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
            m_timestampPeriod = properties.properties.limits.timestampPeriod;

            // Create the command context.
            VkCommandPoolCreateInfo poolCreateInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolCreateInfo.queueFamilyIndex = queueFamilyIndex;
            CHECK_VKCMD(m_dispatch.vkCreateCommandPool(
                m_device, &poolCreateInfo, m_allocator ? &m_allocator.value() : nullptr, &m_cmdPool));
            VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocateInfo.commandPool = m_cmdPool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 2;
            CHECK_VKCMD(m_dispatch.vkAllocateCommandBuffers(m_device, &allocateInfo, m_cmdBuffer));

            // Create the query pool.
            VkQueryPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            createInfo.queryCount = 2;
            CHECK_VKCMD(m_dispatch.vkCreateQueryPool(
                m_device, &createInfo, m_allocator ? &m_allocator.value() : nullptr, &m_queryPool));
        }

        ~VulkanGpuTimer() override {
            if (m_queryPool != VK_NULL_HANDLE) {
                m_dispatch.vkDestroyQueryPool(m_device, m_queryPool, m_allocator ? &m_allocator.value() : nullptr);
            }
            if (m_cmdBuffer[0] != VK_NULL_HANDLE) {
                m_dispatch.vkFreeCommandBuffers(m_device, m_cmdPool, 2, m_cmdBuffer);
            }
            if (m_cmdPool != VK_NULL_HANDLE) {
                m_dispatch.vkDestroyCommandPool(m_device, m_cmdPool, m_allocator ? &m_allocator.value() : nullptr);
            }
        }

        void start() override {
            VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            CHECK_VKCMD(m_dispatch.vkBeginCommandBuffer(m_cmdBuffer[0], &beginInfo));
            m_dispatch.vkCmdResetQueryPool(m_cmdBuffer[0], m_queryPool, 0, 2);
            m_dispatch.vkCmdWriteTimestamp(m_cmdBuffer[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0);
            CHECK_VKCMD(m_dispatch.vkEndCommandBuffer(m_cmdBuffer[0]));
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_cmdBuffer[0];
            CHECK_VKCMD(m_dispatch.vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE));
        }

        void stop() override {
            VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            CHECK_VKCMD(m_dispatch.vkBeginCommandBuffer(m_cmdBuffer[1], &beginInfo));
            m_dispatch.vkCmdWriteTimestamp(m_cmdBuffer[1], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 1);
            CHECK_VKCMD(m_dispatch.vkEndCommandBuffer(m_cmdBuffer[1]));
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_cmdBuffer[1];
            CHECK_VKCMD(m_dispatch.vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE));
            m_valid = true;
        }

        uint64_t query(bool reset = true) const override {
            uint64_t duration = 0;
            if (m_valid) {
                uint64_t buffer[2];
                VkResult result = m_dispatch.vkGetQueryPoolResults(m_device,
                                                                   m_queryPool,
                                                                   0,
                                                                   2,
                                                                   sizeof(uint64_t) * 2,
                                                                   buffer,
                                                                   sizeof(uint64_t),
                                                                   VK_QUERY_RESULT_64_BIT);
                if (result == VK_SUCCESS) {
                    duration = static_cast<uint64_t>(((buffer[1] - buffer[0]) * m_timestampPeriod) / 1000);
                }

                m_valid = !reset;
            }
            return duration;
        }

      private:
        const VulkanDispatch& m_dispatch;
        const VkDevice m_device;
        const VkQueue m_queue;
        const std::optional<VkAllocationCallbacks> m_allocator;
        float m_timestampPeriod{};
        VkCommandPool m_cmdPool{VK_NULL_HANDLE};
        VkCommandBuffer m_cmdBuffer[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkQueryPool m_queryPool{VK_NULL_HANDLE};

        // Can the timer be queried (it might still only read 0).
        mutable bool m_valid{false};
    };

    // An asynchronous GPU timer for OpenGL.
} // namespace opendesk_openxr::utils
