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
#include <fstream>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "replay/renderdoc_replay.h"

namespace {
using OpenCaptureFileFn = ICaptureFile *(RENDERDOC_CC *)();
using InitialiseReplayFn = void(RENDERDOC_CC *)(GlobalEnvironment, const rdcarray<rdcstr> &);

struct ActionRow {
  uint32_t eventId;
  uint32_t depth;
  std::string name;
  std::vector<ResourceId> colourOutputs;
  ResourceId copySource;
  ResourceId copyDestination;
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
    ActionRow row{a.eventId, depth, name, {}, a.copySource, a.copyDestination};
    for(const ResourceId out : a.outputs)
      if(out != ResourceId()) row.colourOutputs.push_back(out);
    actions.push_back(std::move(row));
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
  const size_t pixelCount = (size_t)width * (size_t)height;
  const size_t expected = pixelCount * 4U;
  // DrawThumbnail may return RGB8. Java always expects RGBA8, so expand
  // the 3-channel result with fully opaque alpha; preserve native RGBA8.
  std::vector<unsigned char> expanded;
  if(rgba.size() == pixelCount * 3U)
  {
    expanded.resize(expected);
    for(size_t pixel = 0; pixel < pixelCount; ++pixel)
    {
      expanded[4 * pixel] = rgba[3 * pixel];
      expanded[4 * pixel + 1] = rgba[3 * pixel + 1];
      expanded[4 * pixel + 2] = rgba[3 * pixel + 2];
      expanded[4 * pixel + 3] = 255;
    }
  }
  else if(rgba.size() != expected)
  {
    lastPreviewError += " / DrawThumbnail returned " + std::to_string(rgba.size()) +
                        " bytes; expected RGB8 (" + std::to_string(pixelCount * 3U) +
                        ") or RGBA8 (" + std::to_string(expected) + ")";
    return nullptr;
  }

  const unsigned char *pixels = expanded.empty() ? rgba.data() : expanded.data();
  jbyteArray result = makePreview(env, width, height, pixels, expected);
  if(!result)
    lastPreviewError = "Failed to create Java RGBA8 preview buffer";
  return result;
}


/*
 * Machine-readable diagnostic export for offline AI-assisted GPU debugging.
 * Exports analysis metadata from ACTUAL RenderDoc replay. This does not infer
 * guest IOVA/TIC, GPU causality or missing writes from captured Vulkan state.
 * Bounded event/resource iteration and 16 event seeks protect Android memory.
 */
namespace {
std::string aiQuote(const std::string &value)
{
  std::string result;
  result.reserve(value.size() + 8);
  result += '"';
  for(unsigned char c : value)
  {
    if(c == '"') result += "\\\"";
    else if(c == '\\') result += "\\\\";
    else if(c == '\n') result += "\\n";
    else if(c == '\r') result += "\\r";
    else if(c == '\t') result += "\\t";
    else if(c < 32)
    {
      char code[7];
      snprintf(code, sizeof(code), "\\u%04x", (unsigned)c);
      result += code;
    }
    else result += (char)c;
  }
  result += '"';
  return result;
}

std::string aiId(ResourceId id)
{
  static_assert(sizeof(ResourceId) == sizeof(uint64_t), "Unexpected ResourceId size");
  uint64_t value = 0;
  memcpy(&value, &id, sizeof(value));
  char result[32]{};
  snprintf(result, sizeof(result), "0x%016llx", (unsigned long long)value);
  return result;
}

std::string aiIds(const rdcarray<ResourceId> &ids, size_t limit = 32)
{
  std::string value = "[";
  for(size_t i = 0; i < ids.size() && i < limit; ++i)
  {
    if(i) value += ",";
    value += aiQuote(aiId(ids[i]));
  }
  return value + "]";
}

std::string aiU32s(const rdcarray<uint32_t> &values, size_t limit = 32)
{
  std::string value = "[";
  for(size_t i = 0; i < values.size() && i < limit; ++i)
  {
    if(i) value += ",";
    value += std::to_string(values[i]);
  }
  return value + "]";
}

std::string aiShader(const VKPipe::Shader &shader)
{
  return "{\"id\":" + aiQuote(aiId(shader.resourceId)) +
         ",\"entry_point\":" + aiQuote(shader.entryPoint.c_str()) +
         ",\"stage\":" + std::to_string((uint32_t)shader.stage) +
         ",\"specialization_bytes\":" + std::to_string(shader.specializationData.size()) +
         ",\"subgroup_size\":" + std::to_string(shader.requiredSubgroupSize) + "}";
}

std::ofstream aiFile(const std::string &folder, const char *name)
{
  return std::ofstream(folder + "/" + name, std::ios::binary | std::ios::trunc);
}
} // namespace


namespace {
bool aiHDRTexture(const TextureDescription &tex)
{
  return tex.format.compType == CompType::Float ||
         tex.format.type == ResourceFormatType::R11G11B10 ||
         tex.format.type == ResourceFormatType::R9G9B9E5 ||
         tex.format.type == ResourceFormatType::BC6;
}

bool aiBCMaterial(const TextureDescription &tex)
{
  return tex.format.type == ResourceFormatType::BC1 ||
         tex.format.type == ResourceFormatType::BC5;
}

const TextureDescription *aiFindTexture(const rdcarray<TextureDescription> &all, ResourceId id)
{
  for(const auto &tex : all)
    if(tex.resourceId == id) return &tex;
  return nullptr;
}

// SaveTexture performs all format handling on RenderDoc's replay thread.
// The PNG is only an SDR visualisation of HDR values. EXR preserves linear
// floating-point values (when the driver supports exporting them).
std::string aiSaveImage(const std::string &folder, const std::string &basename,
                        const TextureDescription &tex, FileType type, size_t maxFileBytes)
{
  const std::string suffix = type == FileType::PNG ? ".png" :
                             type == FileType::EXR ? ".exr" :
                             type == FileType::DDS ? ".dds" : ".bin";
  const std::string filename = basename + suffix;
  TextureSave params{};
  params.resourceId = tex.resourceId;
  params.destType = type;
  // DDS can preserve the full compressed mip chain, unlike the PNG/EXR preview.
  params.mip = type == FileType::DDS ? -1 : 0;
  params.slice.sliceIndex = type == FileType::DDS ? -1 : 0;
  params.comp.blackPoint = 0.0f;
  params.comp.whitePoint = aiHDRTexture(tex) && type == FileType::PNG ? 8.0f : 1.0f;
  const std::string fullpath = folder + "/" + filename;
  checkpoint("AI export: SaveTexture begin");
  const ResultDetails result = controller->SaveTexture(params, fullpath.c_str());
  checkpoint("AI export: SaveTexture returned");
  struct stat st{};
  if(!result.OK() || stat(fullpath.c_str(), &st) != 0 ||
     st.st_size <= 0 || (uint64_t)st.st_size > maxFileBytes)
  {
    std::remove(fullpath.c_str());
    return std::string();
  }
  return filename;
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeExportAI(JNIEnv *env, jclass,
                                                        jstring outputDirectory)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  if(!controller)
    return env->NewStringUTF("ERROR:Replay is not open; no GPU diagnostics exported");

  const std::string folder = utf8(env, outputDirectory);
  struct stat info{};
  if(folder.empty() || folder[0] != '/' || folder.size() > 2048 ||
     stat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
    return env->NewStringUTF("ERROR:Invalid diagnostics output folder");

  checkpoint("AI export: start");
  const size_t maxActions = 50000, maxResources = 20000;
  const size_t maxTextures = 12000, maxBuffers = 12000;
  const size_t maxUsageTextures = 64, maxUsageBuffers = 32;
  const size_t maxSnapshots = 16, maxShaderDisassemblies = 8;
  const size_t maxHDRPairs = 3, maxBCAsssets = 8, maxFragmentBinaries = 8;

  const auto &resources = controller->GetResources();
  const auto &textures = controller->GetTextures();
  const auto &buffers = controller->GetBuffers();

  std::ofstream events = aiFile(folder, "events.jsonl");
  std::ofstream resourceFile = aiFile(folder, "resources.jsonl");
  std::ofstream textureFile = aiFile(folder, "textures.jsonl");
  std::ofstream bufferFile = aiFile(folder, "buffers.jsonl");
  std::ofstream uses = aiFile(folder, "resource-usage.jsonl");
  std::ofstream pipeline = aiFile(folder, "pipeline-samples.jsonl");
  std::ofstream shaders = aiFile(folder, "shader-disassembly.jsonl");
  std::ofstream messages = aiFile(folder, "debug-messages.jsonl");
  std::ofstream descriptorDetails = aiFile(folder, "descriptor-details.jsonl");
  std::ofstream hdrSnapshots = aiFile(folder, "hdr-snapshots.jsonl");
  std::ofstream presentSnapshots = aiFile(folder, "present-snapshots.jsonl");
  std::ofstream uploadSnapshots = aiFile(folder, "upload-snapshots.jsonl");
  std::ofstream materialTextures = aiFile(folder, "material-textures.jsonl");
  std::ofstream shaderArtifacts = aiFile(folder, "shader-artifacts.jsonl");
  std::ofstream manifest = aiFile(folder, "manifest.json");
  std::ofstream readme = aiFile(folder, "README.txt");
  if(!events || !resourceFile || !textureFile || !bufferFile || !uses ||
     !pipeline || !shaders || !messages || !descriptorDetails ||
     !hdrSnapshots || !presentSnapshots || !uploadSnapshots || !materialTextures || !shaderArtifacts || !manifest || !readme)
    return env->NewStringUTF("ERROR:Could not create all output files");

  readme <<
    "RenderDoc Mobile - AI Vulkan diagnostics (schema 1)\n"
    "The original .rdc is NOT included. Share it separately if possible.\n"
    "All JSONL files contain exactly one JSON object per line.\n"
    "events.jsonl: recorded RenderDoc action IDs and labels; IDs are per capture.\n"
    "resources.jsonl: RenderDoc resources, initialization chunks, parent/derived IDs.\n"
    "textures.jsonl / buffers.jsonl: replay resource dimensions and allocations.\n"
    "resource-usage.jsonl: selected textures/buffers with their recorded event usages.\n"
    "pipeline-samples.jsonl: up to 16 actual replayed Vulkan events with shaders,\n"
    "descriptor set counts, push constants and pipeline IDs.\n"
    "shader-disassembly.jsonl: up to 4 shader disassemblies if supported.\n"
    "debug-messages.jsonl: messages available from RenderDoc replay.\n"
    "descriptor-details.jsonl: accessed Vulkan descriptor contents at sampled events.\n"
    "hdr-snapshots.jsonl: selected HDR render target PNG + EXR before/after draws.\n"
    "present-snapshots.jsonl: latest matching RGBA8 image copy, source and destination pre-copy, destination post-copy, separate same-size draw target.\n"
    "upload-snapshots.jsonl: preceding buffer-to-image upload into the chosen copy source, with bounded buffer dump (if fully readable) and PNGs before/after upload.\n"
    "An upload buffer dump is not a confirmed buffer-image region: offsets, row pitch and memory layout require independent inspection.\n"
    "material-textures.jsonl: selected BC1/BC5 material views and DDS/PNG content.\n"
    "shader-artifacts.jsonl: selected original fragment SPIR-V/binary metadata.\n"
    "PNG is a [0,8] SDR preview; EXR may preserve HDR. BC DDS preserves blocks.\n"
    "Some textures are not exportable; empty filename means SaveTexture failed.\n"
    "Snapshot PRE is after the preceding recorded action, not exact pixel history.\n"
    "LIMITATIONS: Not every draw call is replayed during export. Captures from\n"
    "different boots are not automatically aligned. A Vulkan ResourceId cannot\n"
    "be equated with a Switch TIC, IOVA, shader hash or guest region without\n"
    "independent instrumentation in Strato. These records alone do not prove\n"
    "memory barriers, resource authority, races or the root cause of corruption.\n"
    "Data may contain game assets, shaders and debug names. Inspect before sharing.\n";

  checkpoint("AI export: events and resources");
  for(size_t i = 0; i < actions.size() && i < maxActions; ++i)
  {
    const ActionRow &a = actions[i];
    events << "{\"event_id\":" << a.eventId << ",\"depth\":" << a.depth
           << ",\"name\":" << aiQuote(a.name) << "}\n";
  }

  for(size_t i = 0; i < resources.size() && i < maxResources; ++i)
  {
    const ResourceDescription &r = resources[i];
    resourceFile << "{\"id\":" << aiQuote(aiId(r.resourceId))
                 << ",\"type\":" << (uint32_t)r.type
                 << ",\"name\":" << aiQuote(r.name.c_str())
                 << ",\"autogenerated_name\":" << (r.autogeneratedName ? "true" : "false")
                 << ",\"initialization_chunks\":" << aiU32s(r.initialisationChunks)
                 << ",\"parent_resources\":" << aiIds(r.parentResources)
                 << ",\"derived_resources\":" << aiIds(r.derivedResources)
                 << "}\n";
  }

  for(size_t i = 0; i < textures.size() && i < maxTextures; ++i)
  {
    const TextureDescription &t = textures[i];
    textureFile << "{\"id\":" << aiQuote(aiId(t.resourceId))
                << ",\"width\":" << t.width << ",\"height\":" << t.height
                << ",\"depth\":" << t.depth << ",\"mips\":" << t.mips
                << ",\"array_size\":" << t.arraysize << ",\"samples\":" << t.msSamp
                << ",\"format\":" << aiQuote(t.format.Name().c_str())
                << ",\"byte_size\":" << t.byteSize
                << ",\"memory_id\":" << aiQuote(aiId(t.memory))
                << ",\"memory_offset\":" << t.memoryOffset
                << ",\"creation_flags\":" << (uint64_t)t.creationFlags
                << "}\n";
  }

  for(size_t i = 0; i < buffers.size() && i < maxBuffers; ++i)
  {
    const BufferDescription &b = buffers[i];
    bufferFile << "{\"id\":" << aiQuote(aiId(b.resourceId))
               << ",\"byte_length\":" << b.length
               << ",\"vulkan_gpu_address\":" << b.gpuAddress
               << ",\"memory_id\":" << aiQuote(aiId(b.memory))
               << ",\"memory_offset\":" << b.memoryOffset
               << ",\"creation_flags\":" << (uint64_t)b.creationFlags
               << "}\n";
  }

  checkpoint("AI export: sampled resource usages");
  // Representative selection, not a complete per-resource history.
  std::set<size_t> textureUsageIndices;
  for(size_t i = 0; i < textures.size() && i < maxUsageTextures / 2; ++i)
    textureUsageIndices.insert(i);
  std::vector<size_t> largest;
  for(size_t i = 0; i < textures.size(); ++i) largest.push_back(i);
  const size_t top = std::min(largest.size(), maxUsageTextures / 2);
  std::partial_sort(largest.begin(), largest.begin() + top, largest.end(),
                    [&](size_t a, size_t b){ return textures[a].byteSize > textures[b].byteSize; });
  for(size_t i = 0; i < top; ++i) textureUsageIndices.insert(largest[i]);

  auto writeUsage = [&](ResourceId id, const char *kind) {
    const auto usageList = controller->GetUsage(id);
    uses << "{\"resource_id\":" << aiQuote(aiId(id))
         << ",\"kind\":" << aiQuote(kind)
         << ",\"total_usages\":" << usageList.size() << ",\"events\":[";
    for(size_t i = 0; i < usageList.size() && i < 4096; ++i)
    {
      if(i) uses << ",";
      uses << "{\"event_id\":" << usageList[i].eventId
           << ",\"usage_code\":" << (uint32_t)usageList[i].usage << "}";
    }
    uses << "]}\n";
  };

  for(size_t idx : textureUsageIndices)
    writeUsage(textures[idx].resourceId, "texture");
  for(size_t i = 0; i < buffers.size() && i < maxUsageBuffers; ++i)
    writeUsage(buffers[i].resourceId, "buffer");


  // This selects the last image copy with compatible RGBA8 source/destination.
  // It is only a candidate presentation copy, not proof of a swapchain path.
  size_t presentCopiesAttempted = 0, presentImagesSaved = 0;
  size_t selectedCopyIndex = actions.size();
  for(size_t n = actions.size(); n > 1; --n)
  {
    const size_t copyIndex = n - 1;
    const ActionRow &copy = actions[copyIndex];
    if(copy.name.find("CopyImage") == std::string::npos &&
       copy.name.find("BlitImage") == std::string::npos)
      continue;
    if(copy.copySource == ResourceId() || copy.copyDestination == ResourceId() ||
       copy.copySource == copy.copyDestination)
      continue;
    const TextureDescription *src = aiFindTexture(textures, copy.copySource);
    const TextureDescription *dst = aiFindTexture(textures, copy.copyDestination);
    if(!src || !dst || src->width != dst->width || src->height != dst->height ||
       src->depth != 1 || dst->depth != 1 || !src->width || !src->height ||
       src->width > 4096 || src->height > 4096 ||
       src->byteSize > 32ULL * 1024ULL * 1024ULL ||
       dst->byteSize > 32ULL * 1024ULL * 1024ULL)
      continue;
    auto simpleRGBA8 = [](const TextureDescription &tex) {
      return tex.format.type == ResourceFormatType::Regular &&
             tex.format.compCount == 4 && tex.format.compByteWidth == 1 &&
             (tex.format.compType == CompType::UNorm ||
              tex.format.compType == CompType::UNormSRGB) &&
             tex.msSamp <= 1 && tex.arraysize == 1 && tex.mips >= 1;
    };
    if(!simpleRGBA8(*src) || !simpleRGBA8(*dst))
      continue;

    const uint32_t beforeEvent = actions[copyIndex - 1].eventId;
    if(!beforeEvent || beforeEvent >= copy.eventId)
      continue;
    // Another render target of the same size is useful to compare, but is
    // not necessarily the producer of the copy source.
    const ActionRow *otherDraw = nullptr;
    const TextureDescription *otherTarget = nullptr;
    for(size_t k = copyIndex; k > 0 && !otherDraw; --k)
    {
      const ActionRow &draw = actions[k - 1];
      if(draw.name.find("Draw") == std::string::npos)
        continue;
      for(ResourceId id : draw.colourOutputs)
      {
        if(id == copy.copySource || id == copy.copyDestination)
          continue;
        const TextureDescription *candidate = aiFindTexture(textures, id);
        if(candidate && simpleRGBA8(*candidate) &&
           candidate->width == src->width && candidate->height == src->height)
        {
          otherDraw = &draw;
          otherTarget = candidate;
          break;
        }
      }
    }

    const std::string base = "present_" + std::to_string(copy.eventId);
    checkpoint("AI export: before candidate image copy");
    controller->SetFrameEvent(beforeEvent, false);
    const uint64_t bytesExpected = uint64_t(src->width) * uint64_t(src->height) * 4U;
    const bool canCompare = bytesExpected > 0 && bytesExpected <= 16ULL * 1024ULL * 1024ULL;
    bytebuf sourcePixels;
    if(canCompare)
    {
      checkpoint("AI export: source GetTextureData before copy");
      sourcePixels = controller->GetTextureData(src->resourceId, Subresource(0, 0, 0));
    }
    const std::string sourceBefore = aiSaveImage(folder, base + "_source_before", *src,
                                                  FileType::PNG, 12 * 1024 * 1024);
    const std::string destBefore = aiSaveImage(folder, base + "_dest_before", *dst,
                                                FileType::PNG, 12 * 1024 * 1024);
    std::string otherAfter;
    if(otherDraw && otherTarget)
    {
      checkpoint("AI export: separate RGBA8 draw target");
      controller->SetFrameEvent(otherDraw->eventId, false);
      otherAfter = aiSaveImage(folder, base + "_other_draw_after", *otherTarget,
                               FileType::PNG, 12 * 1024 * 1024);
    }
    checkpoint("AI export: after candidate image copy");
    controller->SetFrameEvent(copy.eventId, false);
    bytebuf destinationPixels;
    if(canCompare)
    {
      checkpoint("AI export: destination GetTextureData after copy");
      destinationPixels = controller->GetTextureData(dst->resourceId, Subresource(0, 0, 0));
    }
    const bool pixelsCompared = canCompare && sourcePixels.size() == bytesExpected &&
                                destinationPixels.size() == bytesExpected;
    uint64_t changedPixels = 0;
    if(pixelsCompared)
      for(size_t pixel = 0; pixel < (size_t)(bytesExpected / 4U); ++pixel)
        if(std::memcmp(sourcePixels.data() + 4U * pixel,
                       destinationPixels.data() + 4U * pixel, 4U) != 0)
          ++changedPixels;
    const std::string destAfter = aiSaveImage(folder, base + "_dest_after", *dst,
                                               FileType::PNG, 12 * 1024 * 1024);
    presentImagesSaved += !sourceBefore.empty();
    presentImagesSaved += !destBefore.empty();
    presentImagesSaved += !destAfter.empty();
    presentImagesSaved += !otherAfter.empty();
    presentSnapshots << "{\"copy_event_id\":" << copy.eventId
                     << ",\"before_event_id\":" << beforeEvent
                     << ",\"copy_source_id\":" << aiQuote(aiId(copy.copySource))
                     << ",\"copy_destination_id\":" << aiQuote(aiId(copy.copyDestination))
                     << ",\"source_before_png\":" << aiQuote(sourceBefore)
                     << ",\"destination_before_png\":" << aiQuote(destBefore)
                     << ",\"destination_after_png\":" << aiQuote(destAfter)
                     << ",\"source_raw_bytes\":" << sourcePixels.size()
                     << ",\"destination_raw_bytes\":" << destinationPixels.size()
                     << ",\"full_image_bytes_compared\":" << (pixelsCompared ? bytesExpected : 0)
                     << ",\"different_pixels\":"
                     << (pixelsCompared ? std::to_string(changedPixels) : "null")
                     << ",\"other_draw_event_id\":" << (otherDraw ? otherDraw->eventId : 0)
                     << ",\"other_draw_texture_id\":"
                     << aiQuote(otherTarget ? aiId(otherTarget->resourceId) : aiId(ResourceId()))
                     << ",\"other_draw_after_png\":" << aiQuote(otherAfter)
                     << ",\"classification\":\"last_rgba8_image_copy_candidate\","
                     << "\"note\":\"Pixel difference is descriptive, not proof of corruption: partial copy regions or format interpretation can differ; same-size draw is not a verified producer; empty PNG means unavailable\"}\n";
    selectedCopyIndex = copyIndex;
    ++presentCopiesAttempted;
    break;
  }

  // Follow the input to the chosen final RGBA8 copy back to a preceding
  // buffer-to-image upload in the same capture. Copy metadata comes from
  // RenderDoc's ActionDescription, never guessed from event numbers.
  size_t uploadCandidatesAttempted = 0, uploadBufferDumpsSaved = 0;
  if(selectedCopyIndex < actions.size())
  {
    const ActionRow &copy = actions[selectedCopyIndex];
    const TextureDescription *dstImage = aiFindTexture(textures, copy.copySource);
    if(dstImage)
    {
      // Find the nearest earlier buffer-to-image upload into the exact
      // ResourceId selected above as the presentation-copy source.
      for(size_t u = selectedCopyIndex; u > 1; --u)
      {
        const ActionRow &upload = actions[u - 1];
        if(upload.name.find("CopyBufferToImage") == std::string::npos ||
           upload.copyDestination != copy.copySource)
          continue;
        const BufferDescription *buffer = nullptr;
        for(const auto &item : buffers)
          if(item.resourceId == upload.copySource) { buffer = &item; break; }
        const uint32_t beforeEvent = actions[u - 2].eventId;
        if(!beforeEvent || beforeEvent >= upload.eventId)
          continue;

        const std::string prefix = "upload_" + std::to_string(upload.eventId);
        checkpoint("AI export: before buffer-to-image upload");
        controller->SetFrameEvent(beforeEvent, false);
        const std::string beforeImage = aiSaveImage(folder, prefix + "_image_before",
                                                    *dstImage, FileType::PNG, 12 * 1024 * 1024);
        // Full-buffer read is only attempted for a small buffer. The copy's
        // bufferOffset, bufferRowLength, bufferImageHeight, and region list are
        // not inferred from the ActionDescription.
        const size_t maxBufferDump = 16U * 1024U * 1024U;
        std::string bufferFile;
        size_t dumpedBytes = 0;
        if(buffer && buffer->length > 0 && buffer->length <= maxBufferDump)
        {
          checkpoint("AI export: source GetBufferData before upload");
          const bytebuf raw = controller->GetBufferData(buffer->resourceId, 0, buffer->length);
          if(raw.size() == buffer->length)
          {
            bufferFile = prefix + "_buffer.bin";
            const std::string fullpath = folder + "/" + bufferFile;
            std::ofstream binary(fullpath, std::ios::binary | std::ios::trunc);
            binary.write(reinterpret_cast<const char *>(raw.data()),
                         static_cast<std::streamsize>(raw.size()));
            binary.close();
            if(binary.good())
            {
              dumpedBytes = raw.size();
              ++uploadBufferDumpsSaved;
            }
            else
            {
              std::remove(fullpath.c_str());
              bufferFile.clear();
            }
          }
        }
        checkpoint("AI export: after buffer-to-image upload");
        controller->SetFrameEvent(upload.eventId, false);
        const std::string afterImage = aiSaveImage(folder, prefix + "_image_after",
                                                   *dstImage, FileType::PNG, 12 * 1024 * 1024);
        uploadSnapshots << "{\"event_id\":" << upload.eventId
                        << ",\"before_event_id\":" << beforeEvent
                        << ",\"copy_source_buffer_id\":" << aiQuote(aiId(upload.copySource))
                        << ",\"copy_destination_image_id\":" << aiQuote(aiId(upload.copyDestination))
                        << ",\"source_buffer_total_bytes\":" << (buffer ? buffer->length : 0)
                        << ",\"source_buffer_dump_bytes\":" << dumpedBytes
                        << ",\"source_buffer_file\":" << aiQuote(bufferFile)
                        << ",\"image_before_png\":" << aiQuote(beforeImage)
                        << ",\"image_after_png\":" << aiQuote(afterImage)
                        << ",\"buffer_image_regions_known\":false"
                        << ",\"note\":\"Full buffer starts at offset 0, but Vulkan copy region offsets/pitches are not reconstructed. Missing files mean unsupported readback, not zero pixels.\"}\n";
        ++uploadCandidatesAttempted;
        break;
      }
    }
  }

  checkpoint("AI export: Vulkan pipeline samples");
  std::vector<size_t> candidates;
  // Prefer actual draw/dispatch operations, fall back to the full action list.
  for(size_t i = 0; i < actions.size(); ++i)
    if(actions[i].name.find("Draw") != std::string::npos ||
       actions[i].name.find("Dispatch") != std::string::npos)
      candidates.push_back(i);
  if(candidates.empty())
    for(size_t i = 0; i < actions.size(); ++i) candidates.push_back(i);

  std::set<size_t> sampleIndices;
  for(size_t i = 0; i < candidates.size() && i < 4; ++i)
    sampleIndices.insert(candidates[i]);
  for(size_t i = (candidates.size() > 4 ? candidates.size() - 4 : 0);
      i < candidates.size(); ++i)
    sampleIndices.insert(candidates[i]);
  for(size_t i = 0; i < 8 && !candidates.empty(); ++i)
    sampleIndices.insert(candidates[(i * (candidates.size() - 1)) / 7]);
  // Iterate in event order and avoid gratuitous seeks.
  size_t disassemblyCount = 0;
  size_t hdrPairsExported = 0, bcAssetsExported = 0, fragmentBinariesExported = 0;
  size_t descriptorRows = 0;
  std::set<std::string> disassembled, savedShaders, savedMaterials;
  size_t snapshotCount = 0;
  for(size_t idx : sampleIndices)
  {
    if(snapshotCount >= maxSnapshots) break;
    const ActionRow &a = actions[idx];
    // Capture the nearest *preceding action's* post-state and the selected
    // draw's post-state. These pairs identify changes but do not give exact
    // per-pixel write history or prove which draw caused the discrepancy.
    if(hdrPairsExported < maxHDRPairs && idx > 0 && !a.colourOutputs.empty() &&
       a.name.find("Draw") != std::string::npos)
    {
      for(const ResourceId target : a.colourOutputs)
      {
        if(hdrPairsExported >= maxHDRPairs) break;
        const TextureDescription *tex = aiFindTexture(textures, target);
        if(!tex || !aiHDRTexture(*tex) ||
           tex->width > 2048 || tex->height > 2048 ||
           tex->depth != 1 || tex->byteSize > 32ULL * 1024ULL * 1024ULL)
          continue;
        const uint32_t beforeId = actions[idx - 1].eventId;
        const std::string base = "snapshot_" + std::to_string(a.eventId) +
                                 "_" + aiId(target).substr(2);
        checkpoint("AI export: before HDR draw");
        controller->SetFrameEvent(beforeId, false);
        const std::string beforePNG = aiSaveImage(folder, base + "_before", *tex,
                                                   FileType::PNG, 8 * 1024 * 1024);
        const std::string beforeEXR = aiSaveImage(folder, base + "_before", *tex,
                                                   FileType::EXR, 32 * 1024 * 1024);
        checkpoint("AI export: after HDR draw");
        controller->SetFrameEvent(a.eventId, false);
        const std::string afterPNG = aiSaveImage(folder, base + "_after", *tex,
                                                  FileType::PNG, 8 * 1024 * 1024);
        const std::string afterEXR = aiSaveImage(folder, base + "_after", *tex,
                                                  FileType::EXR, 32 * 1024 * 1024);
        hdrSnapshots << "{\"event_id\":" << a.eventId
                     << ",\"prior_action_event_id\":" << beforeId
                     << ",\"texture_id\":" << aiQuote(aiId(target))
                     << ",\"format\":" << aiQuote(tex->format.Name().c_str())
                     << ",\"before_png\":" << aiQuote(beforePNG)
                     << ",\"after_png\":" << aiQuote(afterPNG)
                     << ",\"before_exr\":" << aiQuote(beforeEXR)
                     << ",\"after_exr\":" << aiQuote(afterEXR)
                     << ",\"png_white_point\":8.0,\"note\":\"before=after previous recorded action\"}\n";
        ++hdrPairsExported;
      }
    }
    checkpoint("AI export: SetFrameEvent entering");
    controller->SetFrameEvent(a.eventId, false);
    checkpoint("AI export: SetFrameEvent returned");
    const VKPipe::State *state = controller->GetVulkanPipelineState();
    if(!state) continue;
    const auto &accesses = controller->GetDescriptorAccess();
    // Full values for the accessed descriptors, bounded per sampled event.
    // Avoid equating "staticallyUnused=false" with proven dynamic use.
    size_t emittedAtEvent = 0;
    for(const DescriptorAccess &access : accesses)
    {
      if(emittedAtEvent++ >= 128) break;
      descriptorDetails << "{\"event_id\":" << a.eventId
                        << ",\"stage\":" << (uint32_t)access.stage
                        << ",\"descriptor_type\":" << (uint32_t)access.type
                        << ",\"shader_binding_index\":" << access.index
                        << ",\"array_element\":" << access.arrayElement
                        << ",\"store_id\":" << aiQuote(aiId(access.descriptorStore))
                        << ",\"store_offset\":" << access.byteOffset
                        << ",\"store_byte_size\":" << access.byteSize
                        << ",\"statically_unused\":" << (access.staticallyUnused ? "true" : "false");
      if(access.descriptorStore != ResourceId() && access.byteSize > 0 &&
         access.byteSize <= 4096)
      {
        rdcarray<DescriptorRange> ranges;
        ranges.push_back(DescriptorRange(access));
        const auto descriptors = controller->GetDescriptors(access.descriptorStore, ranges);
        if(!descriptors.empty())
        {
          const Descriptor &d = descriptors[0];
          descriptorDetails << ",\"bound\":{\"type\":" << (uint32_t)d.type
                            << ",\"resource_id\":" << aiQuote(aiId(d.resource))
                            << ",\"secondary_id\":" << aiQuote(aiId(d.secondary))
                            << ",\"view_id\":" << aiQuote(aiId(d.view))
                            << ",\"format\":" << aiQuote(d.format.Name().c_str())
                            << ",\"offset\":" << d.byteOffset
                            << ",\"size\":" << d.byteSize
                            << ",\"first_mip\":" << (uint32_t)d.firstMip
                            << ",\"mip_count\":" << (uint32_t)d.numMips
                            << ",\"first_slice\":" << d.firstSlice
                            << ",\"slice_count\":" << d.numSlices << "}";
          const TextureDescription *texture = aiFindTexture(textures, d.resource);
          // If Strato software-decompressed guest BC1/BC5, Vulkan sees an
          // RGBA8/RG8 image. Sample those named guest textures as material
          // previews too, but NEVER label their host bytes as original BC blocks.
          bool namedGuestImage{};
          if(texture)
            for(const auto &resource : resources)
              if(resource.resourceId == texture->resourceId &&
                 std::string(resource.name.c_str()).find("STRATO TIC=") == 0)
              { namedGuestImage = true; break; }
          if(texture && (aiBCMaterial(*texture) || namedGuestImage) &&
             bcAssetsExported < maxBCAsssets &&
             texture->byteSize <= 16ULL * 1024ULL * 1024ULL)
          {
            const std::string id = aiId(texture->resourceId);
            if(savedMaterials.insert(id).second)
            {
              const bool nativeBC = aiBCMaterial(*texture);
              const std::string base = "material_" + id.substr(2);
              checkpoint("AI export: bound material texture");
              const std::string dds = nativeBC
                ? aiSaveImage(folder, base, *texture, FileType::DDS, 20 * 1024 * 1024)
                : std::string();
              const std::string png = aiSaveImage(folder, base, *texture,
                                                  FileType::PNG, 8 * 1024 * 1024);
              materialTextures << "{\"event_id\":" << a.eventId
                               << ",\"resource_id\":" << aiQuote(id)
                               << ",\"view_id\":" << aiQuote(aiId(d.view))
                               << ",\"descriptor_type\":" << (uint32_t)d.type
                               << ",\"host_format\":" << aiQuote(texture->format.Name().c_str())
                               << ",\"native_bc_blocks_available\":" << (nativeBC ? "true" : "false")
                               << ",\"strato_tic_name_present\":" << (namedGuestImage ? "true" : "false")
                               << ",\"dds\":" << aiQuote(dds)
                               << ",\"png\":" << aiQuote(png)
                               << ",\"note\":\"If guest BC was decoded before Vulkan, original guest compressed blocks are NOT in this RDC image\"}\n";
              ++bcAssetsExported;
            }
          }
        }
        const auto locations = controller->GetDescriptorLocations(access.descriptorStore, ranges);
        if(!locations.empty())
          descriptorDetails << ",\"logical_binding\":" << locations[0].fixedBindNumber
                            << ",\"logical_name\":" << aiQuote(locations[0].logicalBindName.c_str());
        if(access.type == DescriptorType::Sampler || access.type == DescriptorType::ImageSampler)
        {
          const auto samplers = controller->GetSamplerDescriptors(access.descriptorStore, ranges);
          if(!samplers.empty())
            descriptorDetails << ",\"sampler\":{\"id\":"
                              << aiQuote(aiId(samplers[0].object))
                              << ",\"lod_min\":" << samplers[0].minLOD
                              << ",\"lod_max\":" << samplers[0].maxLOD
                              << ",\"anisotropy\":" << samplers[0].maxAnisotropy << "}";
        }
      }
      descriptorDetails << "}\n";
      ++descriptorRows;
    }

    pipeline << "{\"event_id\":" << a.eventId
             << ",\"action\":" << aiQuote(a.name)
             << ",\"graphics_pipeline_id\":" << aiQuote(aiId(state->graphics.pipelineResourceId))
             << ",\"compute_pipeline_id\":" << aiQuote(aiId(state->compute.pipelineResourceId))
             << ",\"graphics_descriptor_sets\":" << state->graphics.descriptorSets.size()
             << ",\"compute_descriptor_sets\":" << state->compute.descriptorSets.size()
             << ",\"descriptor_accesses\":" << accesses.size()
             << ",\"push_constant_bytes\":" << state->pushconsts.size()
             << ",\"vertex_shader\":" << aiShader(state->vertexShader)
             << ",\"fragment_shader\":" << aiShader(state->fragmentShader)
             << ",\"compute_shader\":" << aiShader(state->computeShader)
             << "}\n";
    ++snapshotCount;

    // Save original shader bytes and reflection bindings, not just disassembly
    // text. Original bytes are sourced from RenderDoc's reflection, not guessed.
    const VKPipe::Shader &frag = state->fragmentShader;
    if(frag.reflection && fragmentBinariesExported < maxFragmentBinaries)
    {
      const auto *refl = frag.reflection;
      const std::string shaderId = aiId(frag.resourceId);
      if(savedShaders.insert(shaderId).second)
      {
        const bool isSpirv = refl->encoding == ShaderEncoding::SPIRV ||
                             refl->encoding == ShaderEncoding::OpenGLSPIRV;
        std::string binaryName;
        if(!refl->rawBytes.empty() && refl->rawBytes.size() <= 4 * 1024 * 1024)
        {
          binaryName = "fragment_" + shaderId.substr(2) +
                       (isSpirv ? ".spv" : ".shaderbin");
          std::ofstream rawShader = aiFile(folder, binaryName.c_str());
          rawShader.write((const char *)refl->rawBytes.data(), refl->rawBytes.size());
          rawShader.close();
          if(!rawShader) binaryName.clear();
        }
        shaderArtifacts << "{\"event_id\":" << a.eventId
                        << ",\"fragment_shader_id\":" << aiQuote(shaderId)
                        << ",\"entry_point\":" << aiQuote(frag.entryPoint.c_str())
                        << ",\"encoding\":" << (uint32_t)refl->encoding
                        << ",\"binary_file\":" << aiQuote(binaryName)
                        << ",\"shader_bytes\":" << refl->rawBytes.size()
                        << ",\"read_only_bindings\":[";
        for(size_t i = 0; i < refl->readOnlyResources.size() && i < 256; ++i)
        {
          const auto &bind = refl->readOnlyResources[i];
          if(i) shaderArtifacts << ",";
          shaderArtifacts << "{\"name\":" << aiQuote(bind.name.c_str())
                          << ",\"set\":" << bind.fixedBindSetOrSpace
                          << ",\"binding\":" << bind.fixedBindNumber
                          << ",\"descriptor_type\":" << (uint32_t)bind.descriptorType
                          << ",\"array_size\":" << bind.bindArraySize << "}";
        }
        shaderArtifacts << "],\"read_write_bindings\":[";
        for(size_t i = 0; i < refl->readWriteResources.size() && i < 256; ++i)
        {
          const auto &bind = refl->readWriteResources[i];
          if(i) shaderArtifacts << ",";
          shaderArtifacts << "{\"name\":" << aiQuote(bind.name.c_str())
                          << ",\"set\":" << bind.fixedBindSetOrSpace
                          << ",\"binding\":" << bind.fixedBindNumber
                          << ",\"descriptor_type\":" << (uint32_t)bind.descriptorType << "}";
        }
        shaderArtifacts << "]}\n";
        ++fragmentBinariesExported;
      }
    }
    const VKPipe::Shader *selected[] = {
      &state->vertexShader, &state->fragmentShader, &state->computeShader,
    };
    for(const VKPipe::Shader *stage : selected)
    {
      if(disassemblyCount >= maxShaderDisassemblies || !stage->reflection)
        continue;
      const std::string shaderId = aiId(stage->resourceId);
      if(shaderId == aiId(ResourceId()) || !disassembled.insert(shaderId).second)
        continue;
      const ResourceId pipe = stage == &state->computeShader
                            ? state->compute.pipelineResourceId
                            : state->graphics.pipelineResourceId;
      checkpoint("AI export: shader disassembly entering");
      const rdcstr result = controller->DisassembleShader(pipe, stage->reflection, rdcstr());
      checkpoint("AI export: shader disassembly returned");
      const std::string text(result.c_str());
      const size_t keep = std::min(text.size(), (size_t)128 * 1024);
      shaders << "{\"shader_id\":" << aiQuote(shaderId)
              << ",\"event_id\":" << a.eventId
              << ",\"entry_point\":" << aiQuote(stage->entryPoint.c_str())
              << ",\"text\":" << aiQuote(text.substr(0, keep))
              << ",\"truncated\":" << (text.size() > keep ? "true" : "false") << "}\n";
      ++disassemblyCount;
    }
    const ResultDetails fatal = controller->GetFatalErrorStatus();
    if(!fatal.OK())
    {
      pipeline << "{\"error\":" << aiQuote(replayErrorText(fatal))
               << ",\"after_event\":" << a.eventId << "}\n";
      break;
    }
  }

  checkpoint("AI export: debug messages");
  const auto debugList = controller->GetDebugMessages();
  for(size_t i = 0; i < debugList.size() && i < 1000; ++i)
  {
    const DebugMessage &m = debugList[i];
    messages << "{\"event_id\":" << m.eventId << ",\"message_id\":" << m.messageID
             << ",\"category\":" << (uint32_t)m.category
             << ",\"severity\":" << (uint32_t)m.severity
             << ",\"source\":" << (uint32_t)m.source
             << ",\"description\":" << aiQuote(m.description.c_str()) << "}\n";
  }

  manifest << "{\n"
           << "\"schema\":1,\"source\":\"RenderDoc native Vulkan replay\","
           << "\"complete\":false,\"bounded\":true,"
           << "\"actions_total\":" << actions.size() << ","
           << "\"actions_exported\":" << std::min(actions.size(), maxActions) << ","
           << "\"resources_total\":" << resources.size() << ","
           << "\"resources_exported\":" << std::min(resources.size(), maxResources) << ","
           << "\"textures_total\":" << textures.size() << ","
           << "\"textures_exported\":" << std::min(textures.size(), maxTextures) << ","
           << "\"buffers_total\":" << buffers.size() << ","
           << "\"buffers_exported\":" << std::min(buffers.size(), maxBuffers) << ","
           << "\"usage_textures_sampled\":" << textureUsageIndices.size() << ","
           << "\"usage_buffers_sampled\":" << std::min(buffers.size(), maxUsageBuffers) << ","
           << "\"pipelines_sampled\":" << snapshotCount << ","
           << "\"shaders_disassembled\":" << disassemblyCount << ","
           << "\"hdr_draw_pairs_attempted\":" << hdrPairsExported << ","
           << "\"present_copy_candidates_attempted\":" << presentCopiesAttempted << ","
           << "\"present_images_saved\":" << presentImagesSaved << ","
           << "\"upload_candidates_attempted\":" << uploadCandidatesAttempted << ","
           << "\"upload_full_buffer_dumps_saved\":" << uploadBufferDumpsSaved << ","
           << "\"bc_material_textures_attempted\":" << bcAssetsExported << ","
           << "\"fragment_shader_binaries_attempted\":" << fragmentBinariesExported << ","
           << "\"accessed_descriptors_exported\":" << descriptorRows << ","
           << "\"full_event_pipeline_coverage\":false,"
           << "\"strato_guest_resource_correlation\":false,"
           << "\"original_rdc_embedded\":false}\n";

  // Explicitly flush/check all files before telling Java the ZIP is ready.
  events.flush(); resourceFile.flush(); textureFile.flush(); bufferFile.flush();
  uses.flush(); pipeline.flush(); shaders.flush(); messages.flush();
  descriptorDetails.flush(); hdrSnapshots.flush(); presentSnapshots.flush(); uploadSnapshots.flush(); materialTextures.flush(); shaderArtifacts.flush();
  manifest.flush(); readme.flush();
  if(!events || !resourceFile || !textureFile || !bufferFile || !uses ||
     !pipeline || !shaders || !messages || !descriptorDetails ||
     !hdrSnapshots || !presentSnapshots || !uploadSnapshots || !materialTextures || !shaderArtifacts || !manifest || !readme)
    return env->NewStringUTF("ERROR:Filesystem error while writing diagnostic files");

  checkpoint("AI export: files complete");
  const std::string summary = "ok: " + std::to_string(actions.size()) + " actions, " +
                              std::to_string(textures.size()) + " textures, " +
                              std::to_string(snapshotCount) + " pipeline samples";
  return env->NewStringUTF(summary.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_org_renderdoc_mobile_ReplayActivity_nativeClose(JNIEnv *, jclass)
{
  std::lock_guard<std::mutex> lock(replayMutex);
  closeCapture();
}
