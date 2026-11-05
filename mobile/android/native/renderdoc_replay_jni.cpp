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
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
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
std::string diagnosticPath;
std::string lastPreviewError;

// An append-only checkpoint survives SIGSEGV/SIGABRT in the :replay process.
// Each entry is committed before potentially crashing native driver calls.
void checkpoint(const char *stage)
{
  if(diagnosticPath.empty()) return;
  int fd = open(diagnosticPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if(fd < 0) return;
  const std::string message = std::to_string((long)getpid()) + "  " + stage + "\n";
  const char *ptr = message.c_str();
  size_t left = message.size();
  while(left)
  {
    ssize_t count = write(fd, ptr, left);
    if(count > 0) { left -= (size_t)count; ptr += count; }
    else if(count < 0 && errno == EINTR) continue;
    else break;
  }
  fsync(fd);
  close(fd);
}


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
  lastPreviewError.clear();
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
Java_org_renderdoc_mobile_ReplayActivity_nativeOpen(JNIEnv *env, jclass, jstring pathValue,
                                                     jstring diagnosticFile)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  closeCapture();
  diagnosticPath = utf8(env, diagnosticFile);
  checkpoint("nativeOpen: entered");

  const std::string path = utf8(env, pathValue);
  if(path.empty())
    return error(env, "No capture selected");

  checkpoint("RenderDoc: dlopen begin");
  if(!renderdocLibrary)
    renderdocLibrary = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_LOCAL);
  if(!renderdocLibrary)
  {
    const char *message = dlerror();
    return error(env, std::string("RenderDoc Vulkan replay library could not load: ") +
                           (message ? message : "unknown error"));
  }

  checkpoint("RenderDoc: dlopen succeeded");
  auto open = reinterpret_cast<OpenCaptureFileFn>(
      dlsym(renderdocLibrary, "RENDERDOC_OpenCaptureFile"));
  auto initialise = reinterpret_cast<InitialiseReplayFn>(
      dlsym(renderdocLibrary, "RENDERDOC_InitialiseReplay"));
  if(!open || !initialise)
    return error(env, "The RenderDoc library has no native replay entry points");

  if(!replayInitialised)
  {
    GlobalEnvironment globals{};
    // Enumerating the GPU twice during bootstrap is unnecessary. OpenCapture
    // selects the Vulkan device on its own. Keep initialisation minimal.
    globals.enumerateGPUs = false;
    checkpoint("InitialiseReplay: entering");
    initialise(globals, {});
    replayInitialised = true;
    checkpoint("InitialiseReplay: succeeded");
  }

  checkpoint("OpenCaptureFile: entering");
  openFile = open();
  checkpoint("OpenCaptureFile: returned");
  if(!openFile)
    return error(env, "RenderDoc failed to create a capture reader");

  checkpoint("OpenFile metadata: entering");
  ResultDetails metadata = openFile->OpenFile(path.c_str(), "rdc");
  checkpoint("OpenFile metadata: returned");
  if(!metadata.OK())
  {
    const std::string message = replayErrorText(metadata);
    closeCapture();
    return error(env, "Invalid RDC capture: " + message);
  }

  // A hint of replay support is not a successful GPU replay.
  // OpenCapture actually creates/replays Vulkan state, and returns specific errors.
  checkpoint("OpenCapture GPU: entering");
  const auto result = openFile->OpenCapture(ReplayOptions{});
  checkpoint("OpenCapture GPU: returned");
  if(!result.first.OK() || !result.second)
  {
    const std::string message = replayErrorText(result.first);
    if(result.second)
      result.second->Shutdown();
    closeCapture();
    checkpoint("OpenCapture GPU: failed with RenderDoc result");
    return error(env, "GPU replay failed: " + message);
  }

  controller = result.second;
  // The controller owns its replay state independently of the RDC metadata handle.
  openFile->Shutdown();
  openFile = nullptr;

  // Do not create replay outputs or issue an extra SetFrameEvent during the
  // initial open: some Android Vulkan drivers reject headless output setup.
  // The controller's initial event is enough to browse. Create an output
  // lazily on the user's first texture-preview request.
  checkpoint("GetStructuredFile: entering");
  const SDFile &structured = controller->GetStructuredFile();
  checkpoint("GetRootActions: entering");
  flatten(controller->GetRootActions(), 0, structured);
  checkpoint("GetRootActions: flattened");
  if(!actions.empty())
    currentEvent = actions.back().eventId;

  const APIProperties props = controller->GetAPIProperties();
  checkpoint("nativeOpen: success");
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

  checkpoint("SetFrameEvent: entering");
  controller->SetFrameEvent((uint32_t)requested, false);
  checkpoint("SetFrameEvent: returned");
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

// Serialise exactly one small RGBA8 image to Java.
// The header carries uint32 little-endian width and height.
static jbyteArray makePreview(JNIEnv *env, int width, int height,
                             const unsigned char *rgba, size_t length)
{
  if(width <= 0 || height <= 0 || width > 512 || height > 512)
    return nullptr;
  const size_t expected = (size_t)width * (size_t)height * 4;
  if(!rgba || length != expected || expected > 512U * 512U * 4U)
    return nullptr;
  jbyteArray result = env->NewByteArray((jsize)(expected + 8));
  if(!result) return nullptr;
  unsigned char dimensions[8]{};
  for(int i = 0; i < 4; ++i)
  {
    dimensions[i] = (unsigned char)(((uint32_t)width >> (i * 8)) & 255);
    dimensions[i + 4] = (unsigned char)(((uint32_t)height >> (i * 8)) & 255);
  }
  env->SetByteArrayRegion(result, 0, 8, (const jbyte *)dimensions);
  env->SetByteArrayRegion(result, 8, (jsize)expected, (const jbyte *)rgba);
  return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeLastPreviewError(JNIEnv *env, jclass)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  return env->NewStringUTF(lastPreviewError.c_str());
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeTexturePreview(JNIEnv *env, jclass,
                                                               jint index, jint maxSide)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  lastPreviewError.clear();
  if(!controller)
  {
    lastPreviewError = "Replay controller is not open";
    return nullptr;
  }
  if(index < 0 || maxSide < 1)
  {
    lastPreviewError = "Invalid texture index or size";
    return nullptr;
  }

  const auto &textures = controller->GetTextures();
  if((size_t)index >= textures.size())
  {
    lastPreviewError = "Texture index is outside the replay resource list";
    return nullptr;
  }
  const TextureDescription &tex = textures[(size_t)index];
  if(!tex.width || !tex.height || tex.width > 16384 || tex.height > 16384)
  {
    lastPreviewError = "Texture has invalid or unsupported dimensions";
    return nullptr;
  }

  const int maxDimension = std::min(512, std::max(32, (int)maxSide));
  const double scale = std::min(1.0, (double)maxDimension /
                                            (double)std::max(tex.width, tex.height));
  const int width = std::max(1, (int)(tex.width * scale));
  const int height = std::max(1, (int)(tex.height * scale));

  // RenderDoc's GetTextureData() retrieves the selected event's actual
  // subresource contents. Transfer-only images used by the self-capture demo
  // aren't necessarily legal to bind as sampled images for DrawThumbnail().
  // For ordinary RGBA8/BGRA8 2D textures, read back without another GPU draw.
  const bool rgba8 = tex.format.type == ResourceFormatType::Regular &&
                     tex.format.compCount == 4 &&
                     tex.format.compByteWidth == 1 &&
                     (tex.format.compType == CompType::UNorm ||
                      tex.format.compType == CompType::UNormSRGB);
  const uint64_t sourceBytes = uint64_t(tex.width) * uint64_t(tex.height) * 4U;
  if(rgba8 && sourceBytes <= 32ULL * 1024ULL * 1024ULL &&
     tex.depth == 1 && tex.arraysize >= 1 && tex.mips >= 1 &&
     tex.msSamp <= 1)
  {
    checkpoint("GetTextureData RGBA8: entering");
    const bytebuf raw = controller->GetTextureData(tex.resourceId, Subresource(0, 0, 0));
    checkpoint("GetTextureData RGBA8: returned");
    if(raw.size() == sourceBytes)
    {
      const bool bgra = tex.format.BGRAOrder();
      std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4U);
      const unsigned char *data = raw.data();
      for(int y = 0; y < height; ++y)
      {
        const uint32_t sy = std::min(tex.height - 1U,
                                    uint32_t(uint64_t(y) * tex.height / height));
        for(int x = 0; x < width; ++x)
        {
          const uint32_t sx = std::min(tex.width - 1U,
                                      uint32_t(uint64_t(x) * tex.width / width));
          const size_t src = (size_t(sy) * tex.width + sx) * 4U;
          const size_t dst = (size_t(y) * width + x) * 4U;
          pixels[dst] = data[src + (bgra ? 2 : 0)];
          pixels[dst + 1] = data[src + 1];
          pixels[dst + 2] = data[src + (bgra ? 0 : 2)];
          pixels[dst + 3] = data[src + 3];
        }
      }
      checkpoint("GetTextureData RGBA8: preview generated");
      return makePreview(env, width, height, pixels.data(), pixels.size());
    }
    lastPreviewError = "GetTextureData returned " + std::to_string(raw.size()) +
                       " bytes, expected " + std::to_string(sourceBytes) +
                       "; falling back to GPU thumbnail";
  }

  // For compressed, depth, HDR, multisampled, or very large textures, let
  // RenderDoc's own GPU display path do format conversion and scaling.
  // Preserve the error details for unsupported device/output combinations.
  if(!output)
  {
    checkpoint("CreateOutput Headless: entering");
    WindowingData window{};
    window.system = WindowingSystem::Unknown;
    output = controller->CreateOutput(window, ReplayOutputType::Headless);
    checkpoint("CreateOutput Headless: returned");
    if(!output)
    {
      lastPreviewError += " / CreateOutput(Headless) returned null";
      return nullptr;
    }
  }
  checkpoint("DrawThumbnail: entering");
  const bytebuf rgba = output->DrawThumbnail(width, height, tex.resourceId,
                                              Subresource(0, 0, 0), CompType::Typeless);
  checkpoint("DrawThumbnail: returned");
  const size_t expected = (size_t)width * (size_t)height * 4U;
  if(rgba.size() != expected)
  {
    lastPreviewError += " / DrawThumbnail returned " + std::to_string(rgba.size()) +
                        " bytes, expected " + std::to_string(expected);
    return nullptr;
  }

  jbyteArray result = makePreview(env, width, height, rgba.data(), rgba.size());
  if(!result)
    lastPreviewError = "Failed to create Java RGBA8 preview buffer";
  return result;
}

extern "C" JNIEXPORT void JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeClose(JNIEnv *, jclass)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  closeCapture();
}
