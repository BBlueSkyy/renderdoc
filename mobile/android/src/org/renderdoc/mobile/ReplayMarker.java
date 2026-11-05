package org.renderdoc.mobile;

/**
 * Separate, small JNI library loaded before RenderDoc's core.
 * Does not depend on libVkLayer_GLES_RenderDoc.so itself.
 */
final class ReplayMarker {
    private ReplayMarker() {}
    static native boolean enableGlobal();
}
