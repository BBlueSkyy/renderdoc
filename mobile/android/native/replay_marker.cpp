/*
 * RenderDoc Mobile Android replay bootstrap marker.
 * SPDX-License-Identifier: MIT
 *
 * RenderDoc checks RTLD_DEFAULT for this symbol inside its native .so
 * constructor. The marker must be globally visible BEFORE the main RenderDoc
 * library (a DT_NEEDED dependency of librenderdoc_replay_jni.so) loads.
 */
#include <jni.h>
#include <dlfcn.h>

extern "C" __attribute__((visibility("default"))) void renderdoc__replay__marker()
{
}

namespace {
void *globalMarkerHandle = nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_renderdoc_mobile_ReplayMarker_enableGlobal(JNIEnv *, jclass)
{
  // System.loadLibrary loads this small bootstrap library first. Re-opening
  // with RTLD_GLOBAL promotes the marker to the Android linker's global group.
  // Unlike merely defining the marker in the replay JNI .so, this ensures it
  // is visible during libVkLayer_GLES_RenderDoc.so's global constructors.
  if(!globalMarkerHandle)
    globalMarkerHandle = dlopen("librenderdoc_replay_marker.so", RTLD_NOW | RTLD_GLOBAL);
  if(!globalMarkerHandle)
    return JNI_FALSE;

  void *symbol = dlsym(RTLD_DEFAULT, "renderdoc__replay__marker");
  return symbol ? JNI_TRUE : JNI_FALSE;
}
