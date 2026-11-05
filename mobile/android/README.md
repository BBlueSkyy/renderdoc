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
