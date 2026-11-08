// MIT License
//
// Copyright(c) 2022-2024 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

// These values were added to LibOVR but never published by Meta.
static constexpr int ovrHmd_QuestPro = 22;
static constexpr int ovrHmd_Quest3 = 23;

// Custom flags for VDXR.
static constexpr int ovrLayerFlag_DisableLeft = 0x100000;
static constexpr int ovrLayerFlag_DisableRight = 0x200000;

// Some values we add for 3rd party standalone runtimes based on VDXR.
static constexpr int ovrControllerType_Index = 0x1000;

// libovr-opendesk extensions (Linux and Windows)
static constexpr uint32_t ovrvd_FrameDataSize = 4992; // VirtualDesktop.Mobile.FrameData
extern "C" ovrResult ovrvd_SetSynchronizationQueueFamilyVk(ovrSession session, uint32_t queueFamilyIndex);
extern "C" ovrResult ovrvd_SetSessionPhysicalDeviceVk(ovrSession session, VkInstance instance, VkPhysicalDevice physicalDevice);
extern "C" ovrResult ovrvd_CommitTextureSwapChainIndex(ovrSession session, ovrTextureSwapChain chain, int index);
#ifndef _WIN32
// A new file descriptor (the caller owns it) for the memory of a color swapchain image, and its size: OpenGL imports it
extern "C" ovrResult ovrvd_ExportTextureSwapChainMemory(ovrSession session, ovrTextureSwapChain chain, int index, int* fd, uint64_t* size);
#endif
extern "C" ovrResult ovrvd_GetFrameData(ovrSession session, uint8_t* buffer, uint32_t size);
// libovr-opendesk: the body tracking skeleton the headset sent (SkeletonJoint array), false if none yet
extern "C" bool ovrvd_GetBodySkeleton(ovrSession session, uint8_t* joints, uint32_t size, uint32_t* jointCount, int32_t* changedCount);
// libovr-opendesk: a message for the streamer's log
extern "C" void ovrvd_Log(const char* text);
