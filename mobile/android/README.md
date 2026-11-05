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

Android NDK guidance explains that `RTLD_LOCAL` is the default from API 23. The marker bootstrap is linked with `-Wl,-z,global` (ELF `DF_1_GLOBAL`), making its symbol visible to `dlsym(RTLD_DEFAULT, ...)` even when originally loaded through Android `System.loadLibrary`. The APK build verifies both the exported symbol and global ELF flag before packaging. This is intentionally independent from RenderDoc core and only applies to `:replay`.

## v0.3.3 — GPU replay works, RGBA8 texture preview

The Moto G100 replay smoke test **successfully opened** an app-generated Vulkan
RDC. The on-device video confirmed 5 event actions (queue submit, clear image,
copy image to buffer) and one `R8G8B8A8_UNORM` 128×128 texture. The
earlier crash is no longer reproduced by this small test.

The remaining failure was isolated to the **texture preview**: the first
implementation always attempted to sample the image through RenderDoc's
`IReplayOutput::DrawThumbnail`, but the demo's RGBA8 image is a transfer
resource rather than a guaranteed sampleable texture.

For uncompressed 8-bit RGBA/BGRA normalised textures (up to 32 MiB mip0),
the app now uses RenderDoc's **real replay-controller `GetTextureData`**,
checks the exact byte count, converts BGRA to RGBA when necessary, and performs
a bounded nearest-neighbour downsample on CPU. This avoids drawing the
transfer-only resource through an unsupported sampled-image path. Complex
formats (compressed, float, depth, multisample, large) still use RenderDoc's
GPU thumbnail path when available. If neither path works, the UI now reports
the native result size/failure stage instead of the generic
"Texture could not be rendered".

Validation required: open the small app-generated RDC; **Replay Vulkan
frame** should show 5 events / 1 texture. Open **Textures → Texture 0** and
confirm that the *image pixels*, not just the RDC metadata screenshot, are
displayed correctly. Only then investigate a Strato game capture. No merge.

## v0.4.0 — Automatic AI diagnostics ZIP (no manual event navigation)

1. Import a Strato .rdc and confirm that the capture inspector opens.
2. Tap **Export AI diagnostics (.zip)**; the isolated replay subprocess creates a bounded report.
3. Choose Downloads in the Android Save dialog.
4. Share the diagnostic ZIP, the original .rdc, Strato emulation.log and an image/video of the defect with the AI.

Report files: manifest.json (counts and limits); events.jsonl (event IDs/actions);
resources.jsonl (resource creation chunks, names and parent/derived IDs);
textures.jsonl and buffers.jsonl (dimensions/formats/memory);
resource-usage.jsonl (representative per-resource usage history);
pipeline-samples.jsonl (up to 16 replay events with Vulkan pipeline and shader state);
shader-disassembly.jsonl (up to four shader disassemblies, if supported);
debug-messages.jsonl and replay-steps.txt (driver/replay diagnostics).

IMPORTANT: the original RDC is not embedded, and the ZIP is not a substitute
for a full Replay capture. Resources and pipelines are sampled under strict
memory/time caps. Vulkan ResourceIds are local to this capture and do not
automatically resolve to Nintendo Switch TIC, IOVA or Strato authority regions.
No game/driver root cause is inferred automatically. The app performs
everything offline; you explicitly save/share the ZIP. The files may contain
game shaders, debug names and addresses, so review before sharing.
If the GPU driver cannot replay a heavy capture, export the existing replay
crash ZIP and share the original .rdc instead. The Strato PR #422 is untouched.
