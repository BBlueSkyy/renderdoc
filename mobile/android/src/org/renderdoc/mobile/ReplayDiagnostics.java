/*
 * RenderDoc Mobile - post-mortem diagnostic reports without ADB.
 * SPDX-License-Identifier: MIT
 *
 * Android 12 exposes replay subprocess exit reasons and native tombstones via
 * ApplicationExitInfo. Reflection keeps compilation compatible with the
 * upstream Android API-23 javac build; methods are called only on API >= 30.
 */
package org.renderdoc.mobile;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class ReplayDiagnostics {
    private static final String TRACE = "replay-steps.txt";
    private static final String PREF = "replay_diagnostics";

    private ReplayDiagnostics() { }

    public static File traceFile(Context context) {
        return new File(context.getFilesDir(), TRACE);
    }

    public static synchronized void begin(Context context, File capture) {
        long now = System.currentTimeMillis();
        context.getSharedPreferences(PREF, Context.MODE_PRIVATE)
                .edit().putLong("start", now).commit();
        final File file = traceFile(context);
        try (FileOutputStream out = new FileOutputStream(file, false)) {
            String title = "RenderDoc Mobile Vulkan replay diagnostics\n" +
                    "Started: " + now + "\nCapture: " + capture.getName() +
                    "\nCapture size bytes: " + capture.length() + "\n";
            out.write(title.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (IOException ignored) { }
    }

    public static synchronized void append(Context context, String stage) {
        try (FileOutputStream out = new FileOutputStream(traceFile(context), true)) {
            out.write((System.currentTimeMillis() + "  " + stage + "\n")
                    .getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (IOException ignored) { }
    }

    public static String lastCheckpoint(Context context) {
        File file = traceFile(context);
        if (!file.isFile()) return "";
        try (FileInputStream in = new FileInputStream(file)) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int count;
            while ((count = in.read(buf)) > 0 && bytes.size() < 65536) {
                bytes.write(buf, 0, count);
            }
            String[] lines = new String(bytes.toByteArray(), StandardCharsets.UTF_8).split("\n");
            for (int i = lines.length - 1; i >= 0; i--) {
                if (lines[i].trim().length() > 0) return lines[i].trim();
            }
        } catch (IOException ignored) { }
        return "";
    }

    // Returns the most recent matching subprocess exit from this attempt, or null.
    private static Object replayExit(Context context) {
        if (Build.VERSION.SDK_INT < 30) return null;
        long since = context.getSharedPreferences(PREF, Context.MODE_PRIVATE)
                .getLong("start", 0L);
        if (since <= 0L) return null;
        try {
            ActivityManager manager =
                    (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            Method history = manager.getClass().getMethod(
                    "getHistoricalProcessExitReasons", String.class, int.class, int.class);
            List<?> exits = (List<?>) history.invoke(manager, context.getPackageName(), 0, 24);
            if (exits == null) return null;
            Object best = null;
            long latest = 0L;
            for (Object entry : exits) {
                Class<?> klass = entry.getClass();
                String name = (String) klass.getMethod("getProcessName").invoke(entry);
                long timestamp = ((Number) klass.getMethod("getTimestamp")
                        .invoke(entry)).longValue();
                if (name != null && name.endsWith(":replay")
                        && timestamp >= since - 3000L && timestamp > latest) {
                    best = entry;
                    latest = timestamp;
                }
            }
            return best;
        } catch (Throwable ignored) {
            // Some ROMs restrict the history query. The file checkpoints
            // remain useful even if historical exit records are unavailable.
            return null;
        }
    }

    private static String reasonLabel(int reason) {
        switch (reason) {
            case 2: return "killed by OS signal";
            case 3: return "low memory";
            case 4: return "uncaught Java exception";
            case 5: return "native crash";
            case 6: return "ANR";
            case 7: return "process initialization failure";
            case 9: return "excessive resource usage";
            case 10: return "user requested";
            default: return "reason code " + reason;
        }
    }

    public static String lastExit(Context context) {
        Object record = replayExit(context);
        if (record == null) return "No matching Android exit record yet";
        try {
            Class<?> klass = record.getClass();
            int reason = ((Number) klass.getMethod("getReason").invoke(record)).intValue();
            int signal = ((Number) klass.getMethod("getStatus").invoke(record)).intValue();
            String detail = (String) klass.getMethod("getDescription").invoke(record);
            String result = "Android exit: " + reasonLabel(reason) + " (reason " +
                    reason + ", signal/status " + signal + ")";
            if (detail != null && detail.length() > 0)
                result += "\n" + detail;
            return result;
        } catch (Throwable failure) {
            return "Android exit info could not be decoded: " + failure;
        }
    }

    private static void zipEntry(ZipOutputStream zip, String name, byte[] bytes)
            throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(bytes);
        zip.closeEntry();
    }

    public static void export(Context context, OutputStream destination) throws IOException {
        try (ZipOutputStream zip = new ZipOutputStream(destination)) {
            File trace = traceFile(context);
            if (trace.isFile()) {
                zip.putNextEntry(new ZipEntry("replay-steps.txt"));
                try (FileInputStream in = new FileInputStream(trace)) {
                    byte[] buf = new byte[8192];
                    int count;
                    while ((count = in.read(buf)) != -1) zip.write(buf, 0, count);
                }
                zip.closeEntry();
            }
            zipEntry(zip, "android-exit-reason.txt",
                    lastExit(context).getBytes(StandardCharsets.UTF_8));

            Object record = replayExit(context);
            if (record != null && Build.VERSION.SDK_INT >= 31) {
                // Android 12 can supply a native tombstone as protobuf; do not
                // assume text encoding or claim a native stack when null.
                try {
                    Method method = record.getClass().getMethod("getTraceInputStream");
                    InputStream source = (InputStream) method.invoke(record);
                    if (source != null) {
                        try (InputStream in = source) {
                            zip.putNextEntry(new ZipEntry("android-native-tombstone.pb"));
                            byte[] buf = new byte[8192];
                            int read;
                            int total = 0;
                            while ((read = in.read(buf)) != -1 && total < 24 * 1024 * 1024) {
                                int keep = Math.min(read, 24 * 1024 * 1024 - total);
                                zip.write(buf, 0, keep);
                                total += keep;
                            }
                            zip.closeEntry();
                        }
                    }
                } catch (Throwable ignored) {
                    // Native tombstone may have been evicted from the Android ring buffer.
                }
            }
        }
    }
}
