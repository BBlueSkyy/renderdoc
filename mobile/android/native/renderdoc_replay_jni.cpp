/*
 * RenderDoc Mobile native Vulkan replay implementation.
 * SPDX-License-Identifier: MIT
 *
 * Runs inside :replay process, distinct from capture/metadata inspector.
 * All calls into IReplayController run on ReplayActivity's one worker thread.
 * This code never interprets a thumbnail as a successful replay.
 */
#include <jni.h>
#include <dlfcn.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "replay/renderdoc_replay.h"

namespace {
using OpenCaptureFileFn = ICaptureFile *(RENDERDOC_CC *)();
using InitialiseReplayFn = void(RENDERDOC_CC *)(GlobalEnvironment, const rdcarray<rdcstr> &);

struct ActionRow {
  uint32_t eventId;
  uint32_t depth;
  std::string name;
};

std::mutex replayMutex;
void *renderdocLibrary = nullptr;
bool replayInitialised = false;
ICaptureFile *openFile = nullptr;
IReplayController *controller = nullptr;
IReplayOutput *output = nullptr;
std::vector<ActionRow> actions;
uint32_t currentEvent = 0;

std::string utf8(JNIEnv *env, jstring str)
{
  if(!str)
    return {};
  const char *chars = env->GetStringUTFChars(str, nullptr);
  if(!chars)
    return {};
  std::string value(chars);
  env->ReleaseStringUTFChars(str, chars);
  return value;
}

jobjectArray array(JNIEnv *env, const std::vector<std::string> &values)
{
  jclass type = env->FindClass("java/lang/String");
  if(!type)
    return nullptr;
  jobjectArray result = env->NewObjectArray((jsize)values.size(), type, nullptr);
  if(!result)
    return nullptr;
  for(size_t i = 0; i < values.size(); ++i)
  {
    jstring s = env->NewStringUTF(values[i].c_str());
    if(!s)
      return result;
    env->SetObjectArrayElement(result, (jsize)i, s);
    env->DeleteLocalRef(s);
  }
  return result;
}

jobjectArray error(JNIEnv *env, const std::string &message)
{
  return array(env, {"error", message, "0", "0"});
}

// RenderDoc's ResultDetails::Message() may inline DoStringise<ResultCode>(),
// which is internal to the core and not exported from Android layer builds.
// Decode the public status code and its optional native detail without depending
// on a non-exported C++ symbol.
std::string replayErrorText(const ResultDetails &result)
{
  std::string text = "RenderDoc status " + std::to_string((uint32_t)result.code);
  if(result.internal_msg)
    text += ": " + std::string(result.internal_msg->c_str());
  return text;
}

void closeCapture()
{
  // Output is owned by ReplayController and destroyed on controller shutdown.
  output = nullptr;
  if(controller)
  {
    controller->Shutdown();
    controller = nullptr;
  }
  if(openFile)
  {
    openFile->Shutdown();
    openFile = nullptr;
  }
  actions.clear();
  currentEvent = 0;
}

void flatten(const rdcarray<ActionDescription> &children, uint32_t depth, const SDFile &structured)
{
  if(depth > 48 || actions.size() >= 200000)
    return;
  for(const ActionDescription &a : children)
  {
    std::string name = a.GetName(structured).c_str();
    if(name.empty())
      name = "Vulkan action";
    for(char &c : name)
      if(c == '\n' || c == '\r' || c == '\t')
        c = ' ';
    actions.push_back({a.eventId, depth, name});
    if(!a.children.empty())
      flatten(a.children, depth + 1, structured);
    if(actions.size() >= 200000)
      return;
  }
}
} // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeOpen(JNIEnv *env, jclass, jstring pathValue)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  closeCapture();

  const std::string path = utf8(env, pathValue);
  if(path.empty())
    return error(env, "No capture selected");

  if(!renderdocLibrary)
    renderdocLibrary = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_LOCAL);
  if(!renderdocLibrary)
  {
    const char *message = dlerror();
    return error(env, std::string("RenderDoc Vulkan replay library could not load: ") +
                           (message ? message : "unknown error"));
  }

  auto open = reinterpret_cast<OpenCaptureFileFn>(
      dlsym(renderdocLibrary, "RENDERDOC_OpenCaptureFile"));
  auto initialise = reinterpret_cast<InitialiseReplayFn>(
      dlsym(renderdocLibrary, "RENDERDOC_InitialiseReplay"));
  if(!open || !initialise)
    return error(env, "The RenderDoc library has no native replay entry points");

  if(!replayInitialised)
  {
    GlobalEnvironment globals{};
    globals.enumerateGPUs = true;
    initialise(globals, {});
    replayInitialised = true;
  }

  openFile = open();
  if(!openFile)
    return error(env, "RenderDoc failed to create a capture reader");

  ResultDetails metadata = openFile->OpenFile(path.c_str(), "rdc");
  if(!metadata.OK())
  {
    const std::string message = replayErrorText(metadata);
    closeCapture();
    return error(env, "Invalid RDC capture: " + message);
  }

  // A hint of replay support is not a successful GPU replay.
  // OpenCapture actually creates/replays Vulkan state, and returns specific errors.
  const auto result = openFile->OpenCapture(ReplayOptions{});
  if(!result.first.OK() || !result.second)
  {
    const std::string message = replayErrorText(result.first);
    if(result.second)
      result.second->Shutdown();
    closeCapture();
    return error(env, "GPU replay failed: " + message);
  }

  controller = result.second;
  // The controller owns its replay state independently of the RDC metadata handle.
  openFile->Shutdown();
  openFile = nullptr;

  WindowingData window{};
  window.system = WindowingSystem::Unknown;
  output = controller->CreateOutput(window, ReplayOutputType::Headless);

  const SDFile &structured = controller->GetStructuredFile();
  flatten(controller->GetRootActions(), 0, structured);

  if(!actions.empty())
  {
    currentEvent = actions.back().eventId;
    controller->SetFrameEvent(currentEvent, false);
  }

  const APIProperties props = controller->GetAPIProperties();
  return array(env, {"ok",
                     std::string("Replay initialised on device") +
                         (props.degraded ? " (degraded graphics support)" : ""),
                     std::to_string(actions.size()),
                     std::to_string(controller->GetTextures().size())});
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeActions(JNIEnv *env, jclass,
                                                        jint first, jint max)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  if(!controller)
    return array(env, {});
  const size_t offset = (size_t)std::max(0, first);
  const size_t count = (size_t)std::min(200, std::max(0, max));
  std::vector<std::string> rows;
  for(size_t i = offset; i < actions.size() && rows.size() < count; ++i)
  {
    const ActionRow &row = actions[i];
    rows.push_back(std::to_string(row.eventId) + "\t" +
                   std::to_string(row.depth) + "\t" + row.name);
  }
  return array(env, rows);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeTextures(JNIEnv *env, jclass,
                                                         jint first, jint max)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  if(!controller)
    return array(env, {});
  const auto &textures = controller->GetTextures();
  const size_t offset = (size_t)std::max(0, first);
  const size_t count = (size_t)std::min(200, std::max(0, max));
  std::vector<std::string> rows;
  for(size_t i = offset; i < textures.size() && rows.size() < count; ++i)
  {
    const TextureDescription &tex = textures[i];
    rows.push_back(std::to_string(i) + "\t" +
                   std::to_string(tex.width) + " x " + std::to_string(tex.height) +
                   " (" + std::to_string(tex.mips) + " mips)\t" +
                   std::string(tex.format.Name().c_str()));
  }
  return array(env, rows);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeSeek(JNIEnv *env, jclass, jint requested)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  if(!controller)
    return env->NewStringUTF("Replay not open");
  if(requested < 0)
    return env->NewStringUTF("Invalid event");

  auto it = std::find_if(actions.begin(), actions.end(), [&](const ActionRow &a) {
    return a.eventId == (uint32_t)requested;
  });
  if(it == actions.end())
    return env->NewStringUTF("Event not found in this capture");

  controller->SetFrameEvent((uint32_t)requested, false);
  currentEvent = (uint32_t)requested;
  const auto &descriptorAccess = controller->GetDescriptorAccess();
  std::string response = "Event " + std::to_string(requested) + " replayed: " + it->name +
                         "\nDescriptor accesses: " + std::to_string(descriptorAccess.size());
  // The Vulkan state is taken after actually seeking the replay to this event.
  // This is pipeline/descriptor metadata from RenderDoc, not guessed from RDC bytes.
  const VKPipe::State *vk = controller->GetVulkanPipelineState();
  if(vk)
  {
    response += "\nGraphics descriptor sets: " + std::to_string(vk->graphics.descriptorSets.size());
    response += "\nCompute descriptor sets: " + std::to_string(vk->compute.descriptorSets.size());
    response += "\nPush constant bytes: " + std::to_string(vk->pushconsts.size());
    if(!vk->vertexShader.entryPoint.empty())
      response += "\nVertex shader: " + std::string(vk->vertexShader.entryPoint.c_str());
    if(!vk->fragmentShader.entryPoint.empty())
      response += "\nFragment shader: " + std::string(vk->fragmentShader.entryPoint.c_str());
    if(!vk->computeShader.entryPoint.empty())
      response += "\nCompute shader: " + std::string(vk->computeShader.entryPoint.c_str());
  }
  return env->NewStringUTF(response.c_str());
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeTexturePreview(JNIEnv *env, jclass,
                                                               jint index, jint maxSide)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  if(!controller || !output || index < 0 || maxSide < 1)
    return nullptr;

  const auto &textures = controller->GetTextures();
  if((size_t)index >= textures.size())
    return nullptr;
  const TextureDescription &tex = textures[(size_t)index];
  if(!tex.width || !tex.height)
    return nullptr;

  const int maxDimension = std::min(512, std::max(32, (int)maxSide));
  const double scale = std::min(1.0, (double)maxDimension /
                                                (double)std::max(tex.width, tex.height));
  const int width = std::max(1, (int)(tex.width * scale));
  const int height = std::max(1, (int)(tex.height * scale));

  // DrawThumbnail renders the *selected event's GPU texture contents* via
  // RenderDoc's replay output; it is not the embedded RDC screenshot.
  const bytebuf rgba = output->DrawThumbnail(width, height, tex.resourceId,
                                              Subresource(0, 0, 0), CompType::Typeless);
  const size_t expected = (size_t)width * (size_t)height * 4;
  if(rgba.size() != expected || expected > 512 * 512 * 4)
    return nullptr;

  jbyteArray encoded = env->NewByteArray((jsize)(expected + 8));
  if(!encoded)
    return nullptr;

  // Prefix 2 little-endian uint32s for width/height, then tightly packed RGBA8.
  unsigned char dimensions[8]{};
  for(int i = 0; i < 4; ++i)
  {
    dimensions[i] = (unsigned char)(((uint32_t)width >> (i * 8)) & 255);
    dimensions[4 + i] = (unsigned char)(((uint32_t)height >> (i * 8)) & 255);
  }
  env->SetByteArrayRegion(encoded, 0, 8, (const jbyte *)dimensions);
  env->SetByteArrayRegion(encoded, 8, (jsize)expected, (const jbyte *)rgba.data());
  return encoded;
}

extern "C" JNIEXPORT void JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeClose(JNIEnv *, jclass)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  closeCapture();
}
