/*
 * Copyright (c) 2026 RenderDoc Mobile contributors.
 * SPDX-License-Identifier: MIT
 * Offscreen Vulkan test: only RenderDoc can produce the output .rdc.
 */
#include <jni.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include "renderdoc_app.h"

namespace {
struct Result {
  std::string state, path, detail;
};
Result fail(const std::string &s) { return {"error", "", s}; }
std::string vkError(const char *where, VkResult code) {
  return std::string(where) + " failed (VkResult " + std::to_string((int)code) + ")";
}
bool memoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags, uint32_t &index) {
  VkPhysicalDeviceMemoryProperties props = {};
  vkGetPhysicalDeviceMemoryProperties(gpu, &props);
  for(uint32_t i = 0; i < props.memoryTypeCount; i++)
    if((bits & (1U << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
      index = i;
      return true;
    }
  return false;
}
struct Objects {
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory imageMem = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory bufferMem = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  RENDERDOC_API_1_7_0 *rdoc = nullptr;
  void *captureDevice = nullptr;
  bool capturing = false;
  ~Objects() {
    if(capturing && rdoc) rdoc->DiscardFrameCapture(captureDevice, nullptr);
    if(device) {
      vkDeviceWaitIdle(device);
      if(pool) vkDestroyCommandPool(device, pool, nullptr);
      if(buffer) vkDestroyBuffer(device, buffer, nullptr);
      if(bufferMem) vkFreeMemory(device, bufferMem, nullptr);
      if(image) vkDestroyImage(device, image, nullptr);
      if(imageMem) vkFreeMemory(device, imageMem, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if(instance) vkDestroyInstance(instance, nullptr);
  }
};

Result generate(const std::string &dir) {
  struct stat info = {};
  if(dir.empty() || dir[0] != '/' || stat(dir.c_str(), &info) != 0 ||
     !S_ISDIR(info.st_mode))
    return fail("Cannot use the capture output directory.");

  void *module = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_GLOBAL);
  if(!module) return fail("RenderDoc Vulkan layer library missing from APK.");
  auto getAPI = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(module, "RENDERDOC_GetAPI"));
  if(!getAPI) return fail("RENDERDOC_GetAPI symbol missing.");
  RENDERDOC_API_1_7_0 *api = nullptr;
  if(!getAPI(eRENDERDOC_API_Version_1_7_0, (void **)&api) || !api)
    return fail("Could not initialise RenderDoc's in-application capture API.");

  // The Android Vulkan loader must actually discover the bundled layer.
  uint32_t count = 0;
  VkResult vr = vkEnumerateInstanceLayerProperties(&count, nullptr);
  if(vr != VK_SUCCESS) return fail(vkError("vkEnumerateInstanceLayerProperties", vr));
  std::vector<VkLayerProperties> available(count);
  if(count) {
    vr = vkEnumerateInstanceLayerProperties(&count, available.data());
    if(vr != VK_SUCCESS && vr != VK_INCOMPLETE)
      return fail(vkError("vkEnumerateInstanceLayerProperties(list)", vr));
  }
  const char *layerName = "VK_LAYER_RENDERDOC_Capture";
  bool found = false;
  for(const auto &item : available)
    if(strcmp(item.layerName, layerName) == 0) found = true;
  if(!found)
    return fail("Android Vulkan loader did not enumerate VK_LAYER_RENDERDOC_Capture. "
                "The library is bundled, but the capture layer is not enabled. "
                "No .rdc was created. Check Vulkan GPU debug layer support/settings.");

  Objects o;
  o.rdoc = api;
  VkApplicationInfo app = {};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "RenderDoc Mobile Vulkan Test";
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  ci.enabledLayerCount = 1;
  ci.ppEnabledLayerNames = &layerName;
  vr = vkCreateInstance(&ci, nullptr, &o.instance);
  if(vr != VK_SUCCESS) return fail(vkError("vkCreateInstance(RenderDoc layer)", vr));

  uint32_t gpuCount = 0;
  vr = vkEnumeratePhysicalDevices(o.instance, &gpuCount, nullptr);
  if(vr != VK_SUCCESS || !gpuCount) return fail("No Vulkan GPU found.");
  std::vector<VkPhysicalDevice> gpus(gpuCount);
  vr = vkEnumeratePhysicalDevices(o.instance, &gpuCount, gpus.data());
  if(vr != VK_SUCCESS && vr != VK_INCOMPLETE)
    return fail(vkError("vkEnumeratePhysicalDevices", vr));
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  uint32_t family = UINT32_MAX;
  for(auto candidate : gpus) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
    std::vector<VkQueueFamilyProperties> queues(n);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, queues.data());
    for(uint32_t i = 0; i < n; i++)
      if((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queues[i].queueCount) {
        gpu = candidate; family = i; break;
      }
    if(gpu) break;
  }
  if(!gpu) return fail("No Vulkan graphics queue available.");
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qi = {};
  qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qi.queueFamilyIndex = family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  VkDeviceCreateInfo di = {};
  di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  vr = vkCreateDevice(gpu, &di, nullptr, &o.device);
  if(vr != VK_SUCCESS) return fail(vkError("vkCreateDevice", vr));
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(o.device, family, 0, &queue);
  if(!queue) return fail("vkGetDeviceQueue returned NULL.");

  const uint32_t width = 128, height = 128;
  VkImageCreateInfo imageInfo = {};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vr = vkCreateImage(o.device, &imageInfo, nullptr, &o.image);
  if(vr != VK_SUCCESS) return fail(vkError("vkCreateImage", vr));

  VkMemoryRequirements req = {};
  vkGetImageMemoryRequirements(o.device, o.image, &req);
  uint32_t index = 0;
  if(!memoryType(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, index) &&
     !memoryType(gpu, req.memoryTypeBits, 0, index))
    return fail("No compatible image memory type.");
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = index;
  vr = vkAllocateMemory(o.device, &alloc, nullptr, &o.imageMem);
  if(vr != VK_SUCCESS) return fail(vkError("vkAllocateMemory(image)", vr));
  vr = vkBindImageMemory(o.device, o.image, o.imageMem, 0);
  if(vr != VK_SUCCESS) return fail(vkError("vkBindImageMemory", vr));

  VkBufferCreateInfo bufferInfo = {};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = width * height * 4;
  bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vr = vkCreateBuffer(o.device, &bufferInfo, nullptr, &o.buffer);
  if(vr != VK_SUCCESS) return fail(vkError("vkCreateBuffer", vr));
  vkGetBufferMemoryRequirements(o.device, o.buffer, &req);
  if(!memoryType(gpu, req.memoryTypeBits,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 index))
    return fail("No coherent host-visible readback memory type.");
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = index;
  vr = vkAllocateMemory(o.device, &alloc, nullptr, &o.bufferMem);
  if(vr != VK_SUCCESS) return fail(vkError("vkAllocateMemory(buffer)", vr));
  vr = vkBindBufferMemory(o.device, o.buffer, o.bufferMem, 0);
  if(vr != VK_SUCCESS) return fail(vkError("vkBindBufferMemory", vr));

  VkCommandPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = family;
  vr = vkCreateCommandPool(o.device, &poolInfo, nullptr, &o.pool);
  if(vr != VK_SUCCESS) return fail(vkError("vkCreateCommandPool", vr));
  VkCommandBufferAllocateInfo cmdInfo = {};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdInfo.commandPool = o.pool;
  cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdInfo.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vr = vkAllocateCommandBuffers(o.device, &cmdInfo, &cmd);
  if(vr != VK_SUCCESS) return fail(vkError("vkAllocateCommandBuffers", vr));

  std::string prefix = dir + "/renderdoc-mobile-vulkan";
  api->SetCaptureFilePathTemplate(prefix.c_str());
  uint32_t before = api->GetNumCaptures();
  o.captureDevice = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(o.instance);
  api->StartFrameCapture(o.captureDevice, nullptr);
  o.capturing = (api->IsFrameCapturing() != 0);
  if(!o.capturing)
    return fail("StartFrameCapture did not activate: Vulkan was not intercepted.");

  VkCommandBufferBeginInfo begin = {};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vr = vkBeginCommandBuffer(cmd, &begin);
  if(vr != VK_SUCCESS) return fail(vkError("vkBeginCommandBuffer", vr));
  VkImageMemoryBarrier imageBarrier = {};
  imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  imageBarrier.srcAccessMask = 0;
  imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  imageBarrier.image = o.image;
  imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &imageBarrier);
  VkClearColorValue color = {};
  color.float32[0] = 0.1f;
  color.float32[1] = 0.55f;
  color.float32[2] = 0.9f;
  color.float32[3] = 1.0f;
  vkCmdClearColorImage(cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       &color, 1, &imageBarrier.subresourceRange);
  imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &imageBarrier);
  VkBufferImageCopy copy = {};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         o.buffer, 1, &copy);
  VkBufferMemoryBarrier bufferBarrier = {};
  bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufferBarrier.buffer = o.buffer;
  bufferBarrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0,
                       0, nullptr, 1, &bufferBarrier, 0, nullptr);
  vr = vkEndCommandBuffer(cmd);
  if(vr != VK_SUCCESS) return fail(vkError("vkEndCommandBuffer", vr));
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vr = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  if(vr != VK_SUCCESS) return fail(vkError("vkQueueSubmit", vr));
  vr = vkQueueWaitIdle(queue);
  if(vr != VK_SUCCESS) return fail(vkError("vkQueueWaitIdle", vr));
  void *ptr = nullptr;
  vr = vkMapMemory(o.device, o.bufferMem, 0, VK_WHOLE_SIZE, 0, &ptr);
  if(vr != VK_SUCCESS || !ptr) return fail(vkError("vkMapMemory", vr));
  const uint8_t *rgba = static_cast<const uint8_t *>(ptr);
  const int r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
  vkUnmapMemory(o.device, o.bufferMem);
  if(r < 15 || r > 40 || g < 120 || g > 158 || b < 210 || b > 245 || a < 250)
    return fail("GPU readback mismatch. RGBA=" + std::to_string(r) + "," +
                std::to_string(g) + "," + std::to_string(b) + "," + std::to_string(a));

  api->SetCaptureTitle("RenderDoc Mobile - Offscreen Vulkan Clear");
  uint32_t ended = api->EndFrameCapture(o.captureDevice, nullptr);
  o.capturing = false;
  if(!ended) return fail("RenderDoc EndFrameCapture failed.");
  uint32_t after = api->GetNumCaptures();
  if(after <= before) return fail("RenderDoc did not register a new capture.");

  uint32_t size = 0;
  if(!api->GetCapture(after - 1, nullptr, &size, nullptr) || size > 8192)
    return fail("RenderDoc did not return a valid capture filename.");
  std::vector<char> path(size + 2, '\0');
  if(!api->GetCapture(after - 1, path.data(), &size, nullptr))
    return fail("Unable to retrieve RenderDoc capture path.");
  std::string filename(path.data());
  if(filename.compare(0, dir.size() + 1, dir + "/") != 0 ||
     filename.size() < 4 || filename.substr(filename.size() - 4) != ".rdc")
    return fail("RenderDoc capture filename failed directory/extension validation.");
  struct stat saved = {};
  if(stat(filename.c_str(), &saved) != 0 || saved.st_size < 64)
    return fail("RenderDoc reported capture but no non-empty .rdc exists.");

  VkPhysicalDeviceProperties gpuProps = {};
  vkGetPhysicalDeviceProperties(gpu, &gpuProps);
  return {"ok", filename,
          "GPU: " + std::string(gpuProps.deviceName) +
          "\nVulkan commands: clear image and GPU readback" +
          "\nVerified RGBA: " + std::to_string(r) + "," + std::to_string(g) + "," +
          std::to_string(b) + "," + std::to_string(a) +
          "\nRDC size: " + std::to_string((long long)saved.st_size) + " bytes"};
}
jobjectArray toJava(JNIEnv *env, const Result &r) {
  jclass type = env->FindClass("java/lang/String");
  if(!type) return nullptr;
  jobjectArray result = env->NewObjectArray(3, type, nullptr);
  if(!result) return nullptr;
  const std::string values[3] = {r.state, r.path, r.detail};
  for(int i = 0; i < 3; i++) {
    jstring text = env->NewStringUTF(values[i].c_str());
    if(!text) return result;
    env->SetObjectArrayElement(result, i, text);
    env->DeleteLocalRef(text);
  }
  return result;
}
}
extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_renderdoc_mobile_DemoCaptureActivity_captureOffscreenVulkan(JNIEnv *env, jclass,
                                                                       jstring directory) {
  if(!directory) return toJava(env, fail("Capture folder missing."));
  const char *chars = env->GetStringUTFChars(directory, nullptr);
  if(!chars) return toJava(env, fail("Could not read capture folder."));
  std::string folder(chars);
  env->ReleaseStringUTFChars(directory, chars);
  return toJava(env, generate(folder));
}
