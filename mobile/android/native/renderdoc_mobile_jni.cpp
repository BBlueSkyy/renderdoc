/*
 * RenderDoc Mobile native capture metadata bridge.
 * Copyright (c) 2026 RenderDoc Mobile contributors.
 * SPDX-License-Identifier: MIT
 *
 * The capture is inspected with ICaptureFile; this is NOT a replay implementation.
 */
#include <jni.h>
#include <dlfcn.h>
#include <mutex>
#include <sstream>
#include <string>
#include <algorithm>

#include "replay/renderdoc_replay.h"

namespace {
std::mutex captureMutex;
void *coreLibrary = nullptr;
bool replayInitialised = false;

typedef ICaptureFile *(RENDERDOC_CC *OpenCaptureFileFn)();
typedef void(RENDERDOC_CC *InitialiseReplayFn)(GlobalEnvironment, const rdcarray<rdcstr> &);

std::string base64(const bytebuf &data)
{
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((data.size() + 2) / 3) * 4);
  for(size_t i = 0; i < data.size(); i += 3)
  {
    uint32_t combined = uint32_t(data[i]) << 16;
    if(i + 1 < data.size())
      combined |= uint32_t(data[i + 1]) << 8;
    if(i + 2 < data.size())
      combined |= uint32_t(data[i + 2]);
    encoded.push_back(alphabet[(combined >> 18) & 63]);
    encoded.push_back(alphabet[(combined >> 12) & 63]);
    encoded.push_back(i + 1 < data.size() ? alphabet[(combined >> 6) & 63] : '=');
    encoded.push_back(i + 2 < data.size() ? alphabet[combined & 63] : '=');
  }
  return encoded;
}

jobjectArray stringArray(JNIEnv *env, const std::string fields[6])
{
  jclass stringType = env->FindClass("java/lang/String");
  if(!stringType)
    return nullptr;

  jobjectArray result = env->NewObjectArray(6, stringType, nullptr);
  if(!result)
    return nullptr;
  for(jsize i = 0; i < 6; ++i)
  {
    jstring value = env->NewStringUTF(fields[i].c_str());
    if(!value)
      return result;
    env->SetObjectArrayElement(result, i, value);
    env->DeleteLocalRef(value);
  }
  return result;
}

jobjectArray error(JNIEnv *env, const std::string &message)
{
  std::string fields[6] = {"error", message, "", "", "", ""};
  return stringArray(env, fields);
}
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_renderdoc_mobile_MainActivity_inspectCapture(JNIEnv *env, jclass, jstring jpath)
{
  if(!jpath)
    return error(env, "No capture file selected.");

  const char *pathChars = env->GetStringUTFChars(jpath, nullptr);
  if(!pathChars)
    return error(env, "Could not read capture path.");
  std::string path(pathChars);
  env->ReleaseStringUTFChars(jpath, pathChars);

  // A replay session must only be initialised once in the process.
  std::lock_guard<std::mutex> lock(captureMutex);
  if(!coreLibrary)
    coreLibrary = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_LOCAL);
  if(!coreLibrary)
  {
    const char *failure = dlerror();
    return error(env, std::string("Could not load native RenderDoc: ") +
                          (failure ? failure : "unknown error"));
  }

  auto openCaptureFile =
      reinterpret_cast<OpenCaptureFileFn>(dlsym(coreLibrary, "RENDERDOC_OpenCaptureFile"));
  auto initialiseReplay =
      reinterpret_cast<InitialiseReplayFn>(dlsym(coreLibrary, "RENDERDOC_InitialiseReplay"));
  if(!openCaptureFile || !initialiseReplay)
    return error(env, "Required RenderDoc capture APIs are not exported.");

  if(!replayInitialised)
  {
    GlobalEnvironment globals;
    globals.enumerateGPUs = false; // Metadata inspection does not require Vulkan.
    initialiseReplay(globals, {});
    replayInitialised = true;
  }

  ICaptureFile *capture = openCaptureFile();
  if(!capture)
    return error(env, "RenderDoc could not allocate a capture reader.");

  const ResultDetails openResult = capture->OpenFile(path.c_str(), "rdc");
  if(!openResult.OK())
  {
    std::string message = "RenderDoc rejected this capture (result code " +
                          std::to_string(static_cast<uint32_t>(openResult.code)) + ").";
    capture->Shutdown();
    return error(env, message);
  }

  std::string fields[6];
  fields[0] = "ok";
  fields[1] = capture->DriverName().c_str();
  fields[2] = capture->RecordedMachineIdent().c_str();

  switch(capture->LocalReplaySupport())
  {
    case ReplaySupport::Supported: fields[3] = "Reported supported (not tested)"; break;
    case ReplaySupport::SuggestRemote: fields[3] = "Remote replay suggested"; break;
    default: fields[3] = "Not supported locally"; break;
  }

  std::ostringstream sectionInfo;
  int32_t count = capture->GetSectionCount();
  for(int32_t i = 0; i < count && i < 128; ++i)
  {
    SectionProperties props = capture->GetSectionProperties(i);
    sectionInfo << "• " << props.name.c_str()
                << "  (" << props.uncompressedSize << " bytes)\n";
  }
  if(count > 128)
    sectionInfo << "… " << (count - 128) << " more sections";
  if(count == 0)
    sectionInfo << "(none)";
  fields[4] = sectionInfo.str();

  // Optional embedded thumbnail. This reads a small preview without replay.
  Thumbnail thumb = capture->GetThumbnail(FileType::PNG, 512);
  if(!thumb.data.empty())
    fields[5] = base64(thumb.data);

  capture->Shutdown();
  return stringArray(env, fields);
}
