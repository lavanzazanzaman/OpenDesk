// Real OpenVR rendering test: submits Vulkan textures through xrizer, exercising its
// temporary-instance -> application-device transition (the pose-only smoke test cannot).
#include <vulkan/vulkan.h>
#include <openvr_capi.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
static void check(VkResult r, const char * what) { if(r!=VK_SUCCESS)throw std::runtime_error(std::string(what)+": "+std::to_string(r)); }
static std::vector<std::string> words(const std::vector<char>& text){std::istringstream in(text.data());std::vector<std::string> out;for(std::string s;in>>s;)out.push_back(s);return out;}
int main(int argc,char**argv) try {
 setvbuf(stdout,nullptr,_IOLBF,0);
 const char * path=getenv("OPENDESK_OPENVR_API");if(!path)return 77;
 void * lib=dlopen(path,RTLD_NOW);if(!lib)throw std::runtime_error(dlerror());
 auto init=reinterpret_cast<uint32_t(*)(int*,int,const char*)>(dlsym(lib,"VR_InitInternal2"));
 auto get=reinterpret_cast<void*(*)(const char*,int*)>(dlsym(lib,"VR_GetGenericInterface"));
 auto shutdown=reinterpret_cast<void(*)()>(dlsym(lib,"VR_ShutdownInternal"));
 int error=0;init(&error,1,nullptr);if(error)throw std::runtime_error("VR_Init "+std::to_string(error));
 auto * compositor=static_cast<VR_IVRCompositor_FnTable*>(get("FnTable:IVRCompositor_028",&error));
 if(!compositor)throw std::runtime_error("IVRCompositor_028 unavailable");
 std::vector<char> names(compositor->GetVulkanInstanceExtensionsRequired(nullptr,0));
 compositor->GetVulkanInstanceExtensionsRequired(names.data(),names.size());auto instance_names=words(names);std::vector<const char*> instance_ext;
 for(auto&s:instance_names)instance_ext.push_back(s.c_str());
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="vd-openvr-render";app.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;ici.enabledExtensionCount=instance_ext.size();ici.ppEnabledExtensionNames=instance_ext.data();VkInstance instance;
 check(vkCreateInstance(&ici,nullptr,&instance),"instance");
 uint32_t count=0;vkEnumeratePhysicalDevices(instance,&count,nullptr);std::vector<VkPhysicalDevice> devices(count);vkEnumeratePhysicalDevices(instance,&count,devices.data());
 if(!count)throw std::runtime_error("no GPU");VkPhysicalDevice physical=devices[0];
 auto * system=static_cast<VR_IVRSystem_FnTable*>(get("FnTable:IVRSystem_022",&error));uint64_t output=0;
 system->GetOutputDevice(&output,ETextureType_TextureType_Vulkan,instance);if(output)physical=reinterpret_cast<VkPhysicalDevice>(output);
 names.resize(compositor->GetVulkanDeviceExtensionsRequired(physical,nullptr,0));compositor->GetVulkanDeviceExtensionsRequired(physical,names.data(),names.size());
 auto device_names=words(names);std::vector<const char*> device_ext;for(auto&s:device_names)device_ext.push_back(s.c_str());
 vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
 uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;if(family==count)throw std::runtime_error("no graphics queue");
 float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
 VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.enabledExtensionCount=device_ext.size();di.ppEnabledExtensionNames=device_ext.data();VkDevice device;
 check(vkCreateDevice(physical,&di,nullptr,&device),"device");VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_info.imageType=VK_IMAGE_TYPE_2D;image_info.format=VK_FORMAT_R8G8B8A8_UNORM;image_info.extent={256,256,1};image_info.mipLevels=image_info.arrayLayers=1;image_info.samples=VK_SAMPLE_COUNT_1_BIT;image_info.tiling=VK_IMAGE_TILING_OPTIMAL;image_info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
 VkImage image;check(vkCreateImage(device,&image_info,nullptr,&image),"image");VkMemoryRequirements req;vkGetImageMemoryRequirements(device,image,&req);VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);uint32_t mt=0;while(mt<memory.memoryTypeCount && !(req.memoryTypeBits&(1u<<mt)))++mt;
 VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mi.allocationSize=req.size;mi.memoryTypeIndex=mt;VkDeviceMemory allocation;check(vkAllocateMemory(device,&mi,nullptr,&allocation),"memory");vkBindImageMemory(device,image,allocation,0);
 VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;VkCommandPool pool;check(vkCreateCommandPool(device,&pi,nullptr,&pool),"pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;vkAllocateCommandBuffers(device,&ca,&cmd);
 VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkBeginCommandBuffer(cmd,&bi);VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
 VkClearColorValue color{{0.1f,0.7f,0.3f,1.f}};vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
 barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);vkEndCommandBuffer(cmd);VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;check(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE),"clear");vkQueueWaitIdle(queue);
 VRVulkanTextureData_t data{uint64_t(image),device,physical,instance,queue,family,256,256,uint32_t(VK_FORMAT_R8G8B8A8_UNORM),1};Texture_t texture{&data,ETextureType_TextureType_Vulkan,EColorSpace_ColorSpace_Gamma};
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(argc>1?atoi(argv[1]):8);int frames=0;
 TrackedDevicePose_t poses[16]{};
 while(std::chrono::steady_clock::now()<end){
 compositor->WaitGetPoses(poses,16,nullptr,0);
 for(int eye=0;eye<2;++eye){auto r=compositor->Submit(EVREye(eye),&texture,nullptr,EVRSubmitFlags_Submit_Default);if(r!=EVRCompositorError_VRCompositorError_None)throw std::runtime_error("Submit "+std::to_string(r));}
 compositor->PostPresentHandoff();++frames;
 }
 shutdown();vkDeviceWaitIdle(device);vkDestroyCommandPool(device,pool,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,allocation,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
 printf("%s rendered %d stereo frames through xrizer\n",frames>20?"ALL PASS":"FAIL",frames);return frames>20?0:1;
} catch(const std::exception&e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
