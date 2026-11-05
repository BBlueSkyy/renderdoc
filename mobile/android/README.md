# RenderDoc Mobile: Android ARM64 capture inspector (experimental)

This is a **separate Android Activity** that uses RenderDoc's existing native
capture-file API. It is not a port of the desktop Qt interface, and it cannot
capture or replay Vulkan frames yet.

## Current scope

- Open an RDC capture from the Android Storage Access Framework (no broad
  storage permission or external-server dependency).
- Import the file into app-specific storage and read it through
  `RENDERDOC_OpenCaptureFile` / `ICaptureFile::OpenFile`.
- Display the original graphics API, recorded machine identifier, section
  names and sizes, local replay support hint, and embedded thumbnail if present.
- Perform file import and native inspection off the Android UI thread.

Metadata inspection is not a proof that full Vulkan replay will succeed.

## Build

Build the upstream RenderDoc Android `arm64-v8a` core first. Then run:

```bash
bash mobile/android/build-apk.sh
```

This script expects `ANDROID_HOME`, `ANDROID_NDK_HOME` (or `ANDROID_NDK`),
Java 8, Android SDK platform 23, Android build-tools 26.0.1, and the compiled
`build-android-arm64` directory. GitHub Actions prepares these dependencies.

The APK is published as `renderdoc-mobile-arm64-debug` in the experimental
workflow. It is debug-signed and is not intended for distribution as a stable
product.

## Planned next steps

1. Verify installation and opening real Vulkan RDC files on Android 12.
2. Add precise error reporting, memory and storage-pressure handling.
3. Prototype local Vulkan replay in a separate, opt-in code path.
4. Add action browsing and resource/texture viewers only after actual replay
   works on device.

RenderDoc is licensed under MIT; see the root `LICENSE.md`.
