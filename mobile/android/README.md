# RenderDoc Mobile — Android ARM64 inspector and Vulkan replay

Version 0.3.0 adds an **experimental real Vulkan GPU replay** path to the standalone RenderDoc Mobile app. The existing metadata reader and app-local Vulkan capture test are retained.

## Open and replay a capture

1. Choose **Open .rdc capture** and import a previously exported RenderDoc `.rdc` file, such as the frame captured by Strato. If you received a ZIP, extract the `.rdc` before importing it.
2. After native metadata inspection succeeds, tap **Replay Vulkan frame**.
3. The new replay interface runs in the separate Android process `:replay`. It uses the real RenderDoc `ICaptureFile::OpenCapture()` interface, not the capture's embedded JPEG thumbnail. A successful metadata read alone is never presented as replay success.
4. Use **Draw calls / events** to browse RenderDoc's action hierarchy and select an event to call `IReplayController::SetFrameEvent()`.
5. Use **Textures** to select a GPU texture and inspect a bounded RGB(A) preview obtained with `IReplayOutput::DrawThumbnail()` at the current replay event.
6. Return to the inspector to view another capture. The replay controller is shut down when its isolated Activity closes.

## Technical details

The native JNI bridge is in `mobile/android/native/renderdoc_replay_jni.cpp`. It opens an RDC through RenderDoc's own ARM64 core, initialises the Vulkan replay controller, flattens/paginates native actions for the Android UI, allows real GPU event replay, and renders small texture previews. GPU calls are performed on a single executor thread to preserve RenderDoc replay thread affinity.

The Android `:replay` process isolates GPU replay state and memory consumption from the metadata inspector and local capture demo. It does not eliminate risk of driver crashes, device-lost failures, or out-of-memory conditions.

On-device validation remains required for Motorola Moto G100 (Android 12, Adreno 650), especially for large Strato game captures. A green Android build validates the native implementation compiles, **not** that the Vulkan driver successfully replays a particular capture. Error messages from the native RenderDoc result are displayed when replay fails.

This is not desktop Qt parity: pixel history, full shader debugger, mesh viewer, descriptor drilldown, arbitrary mips and slices, GPU counters, and custom shaders are not yet implemented.

## Build

Compile the upstream Android ARM64 RenderDoc core, then run `bash mobile/android/build-apk.sh`. The GitHub workflow `RenderDoc Mobile - Android ARM64 core` performs this and uploads `renderdoc-mobile-arm64-debug`. Version 0.3.0 bundles a separate `librenderdoc_replay_jni.so` as well as the existing inspector and capture test.

The app uses Android's document picker for capture import and needs no ADB, root, network, or broad storage access. The RenderDoc code is licensed under MIT; see the root LICENSE.md.

## v0.3.1: crash investigation on Android 12 (no ADB)

If the `Vulkan frame replay` screen disappears as soon as `Opening capture on the GPU...` appears:

- **Do not infer the cause from the video alone**. The isolated `:replay` process may have crashed natively, thrown a Java exception, or been killed under memory pressure.
- Version 0.3.1 defers headless replay-output setup and initial event replays until the user requests them. This removes extra GPU work during capture open but does not prove all driver crashes are fixed.
- Native JNI writes durable breadcrumbs before/after `RENDERDOC_InitialiseReplay`, file open, `ICaptureFile::OpenCapture`, event parsing and texture preview.
- The main activity reads these breadcrumbs and Android 12's `ApplicationExitInfo` for the `:replay` subprocess. It provides **Export replay diagnostics (.zip)** with `replay-steps.txt`, `android-exit-reason.txt`, and an Android 12 native tombstone protobuf when available.
- There is no requirement to install ADB, Shizuku or root. Tombstones may be evicted by Android before export; in that case the stage and exit reason remain valuable.

**Reproduction protocol:** use the tiny app-generated RDC first, tap replay once, then (if the subprocess closes) export and share the diagnostic ZIP from the main inspector. Diagnose the first native failure using these records before implementing additional speculative GPU fixes. Only move to the larger SpongeBob RDC after the small capture opens successfully.

## v0.3.2: correct replay bootstrap

RenderDoc requires the `renderdoc__replay__marker` symbol globally visible before `libVkLayer_GLES_RenderDoc.so` loads. Without it, the core mistakenly initialises as a capture app and hooks graphics APIs. The Android `:replay` process now first loads `librenderdoc_replay_marker.so`, promotes the marker via `dlopen(RTLD_GLOBAL)`, verifies it with `dlsym(RTLD_DEFAULT)`, **then** loads the replay JNI library and its RenderDoc core dependency. An unavailable marker is reported instead of starting broken GPU replay. This mode transition is isolated from the normal capture and metadata-inspection processes.
