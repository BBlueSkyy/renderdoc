/*
 * RenderDoc Mobile - experimental Android capture inspector.
 * Copyright (c) 2026 RenderDoc Mobile contributors.
 * SPDX-License-Identifier: MIT
 */
package org.renderdoc.mobile;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final int REQUEST_CAPTURE = 1001;
    private static final int BACKGROUND = Color.rgb(21, 24, 29);
    private static final int PANEL = Color.rgb(34, 39, 47);
    private static final int MUTED = Color.rgb(170, 179, 194);
    private static final int ACCENT = Color.rgb(89, 163, 239);

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private TextView status;
    private TextView details;
    private ImageView thumbnail;
    private Button openButton;
    private volatile boolean closing;

    private static final String NATIVE_LOAD_ERROR;
    static {
        String error = null;
        try {
            System.loadLibrary("renderdoc_mobile_jni");
        } catch (Throwable t) {
            error = t.getClass().getSimpleName() + ": " + t.getMessage();
        }
        NATIVE_LOAD_ERROR = error;
    }

    private static native String[] inspectCapture(String absolutePath);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BACKGROUND);
        getWindow().setNavigationBarColor(BACKGROUND);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BACKGROUND);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(20), dp(26), dp(20), dp(24));
        scroll.addView(page);
        setContentView(scroll);

        TextView heading = label("RenderDoc Mobile", 26, Color.WHITE, true);
        page.addView(heading);
        TextView subtitle = label("Android ARM64  /  Capture inspector", 13, MUTED, false);
        subtitle.setPadding(0, dp(5), 0, dp(20));
        page.addView(subtitle);

        openButton = new Button(this);
        openButton.setAllCaps(false);
        openButton.setText("Open .rdc capture");
        openButton.setTextColor(Color.WHITE);
        openButton.setBackgroundTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        page.addView(openButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(54)));
        openButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { selectCapture(); }
        });

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(16), dp(16), dp(16), dp(18));
        panel.setBackgroundColor(PANEL);
        LinearLayout.LayoutParams panelParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        panelParams.topMargin = dp(20);
        page.addView(panel, panelParams);

        status = label("Choose a RenderDoc capture to inspect.", 15, Color.WHITE, true);
        panel.addView(status);

        thumbnail = new ImageView(this);
        thumbnail.setAdjustViewBounds(true);
        thumbnail.setMaxHeight(dp(260));
        thumbnail.setScaleType(ImageView.ScaleType.FIT_CENTER);
        thumbnail.setVisibility(View.GONE);
        LinearLayout.LayoutParams imageParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        imageParams.topMargin = dp(14);
        panel.addView(thumbnail, imageParams);

        details = label(
                "This first release reads metadata, capture sections, and an embedded thumbnail " +
                "using the native RenderDoc API. Frame replay and live capture are not enabled yet.",
                14, MUTED, false);
        details.setPadding(0, dp(12), 0, 0);
        panel.addView(details);

        TextView footer = label("Local processing only. Files are imported into private " +
                "app storage; no network permission is requested.", 12, MUTED, false);
        footer.setPadding(0, dp(18), 0, 0);
        page.addView(footer);

        if (NATIVE_LOAD_ERROR != null) {
            status.setText("Native library unavailable");
            details.setText(NATIVE_LOAD_ERROR);
            openButton.setEnabled(false);
        }
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP,
                value, getResources().getDisplayMetrics());
    }

    private TextView label(String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        view.setTextColor(color);
        view.setGravity(Gravity.START);
        view.setLineSpacing(dp(2), 1.0f);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    private void selectCapture() {
        Intent select = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        select.setType("*/*");
        select.addCategory(Intent.CATEGORY_OPENABLE);
        select.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(select, REQUEST_CAPTURE);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQUEST_CAPTURE || result != RESULT_OK || data == null
                || data.getData() == null) return;

        final Uri uri = data.getData();
        final String name = getDocumentName(uri);
        status.setText("Importing capture…");
        details.setText(name);
        thumbnail.setVisibility(View.GONE);
        openButton.setEnabled(false);
        executor.execute(new Runnable() {
            @Override public void run() { importAndInspect(uri, name); }
        });
    }

    private String getDocumentName(Uri uri) {
        Cursor cursor = null;
        try {
            cursor = getContentResolver().query(uri,
                    new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                String name = cursor.getString(0);
                if (name != null && !name.isEmpty()) return name;
            }
        } catch (Exception ignored) {
            // A provider can omit DISPLAY_NAME.
        } finally {
            if (cursor != null) cursor.close();
        }
        return "selected capture";
    }

    private File importCapture(Uri uri) throws IOException {
        File folder = getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS);
        if (folder == null) folder = getFilesDir();
        if (!folder.exists() && !folder.mkdirs()) {
            throw new IOException("Cannot create the app's capture folder.");
        }

        File staged = new File(folder, "capture-import.tmp");
        File imported = new File(folder, "capture.rdc");
        InputStream input = null;
        FileOutputStream output = null;
        try {
            input = getContentResolver().openInputStream(uri);
            if (input == null) throw new IOException("Cannot open the selected file.");
            output = new FileOutputStream(staged);
            byte[] buffer = new byte[256 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (Thread.currentThread().isInterrupted()) {
                    throw new IOException("Import interrupted.");
                }
                output.write(buffer, 0, count);
            }
            output.getFD().sync();
        } finally {
            if (output != null) try { output.close(); } catch (IOException ignored) { }
            if (input != null) try { input.close(); } catch (IOException ignored) { }
        }
        if (imported.exists() && !imported.delete()) {
            throw new IOException("Cannot replace the previous imported capture.");
        }
        if (!staged.renameTo(imported)) {
            throw new IOException("Cannot finish importing capture.");
        }
        return imported;
    }

    private void importAndInspect(final Uri uri, final String name) {
        try {
            final File file = importCapture(uri);
            final String[] result = inspectCapture(file.getAbsolutePath());
            if (result == null || result.length < 6) {
                throw new IOException("The native inspector returned incomplete metadata.");
            }
            runOnUiThread(new Runnable() {
                @Override public void run() {
                    if (closing) return;
                    openButton.setEnabled(true);
                    if (!"ok".equals(result[0])) {
                        status.setText("Could not open capture");
                        details.setText(name + "\n\n" + result[1]);
                        return;
                    }
                    status.setText("Capture opened successfully");
                    String information = "File: " + name +
                            "\nSize: " + formatBytes(file.length()) +
                            "\nGraphics API: " + result[1] +
                            "\nRecorded machine: " + result[2] +
                            "\nLocal replay support: " + result[3] +
                            "\n\nCapture sections:\n" + result[4] +
                            "\n\nReplay has not been tested by this metadata inspection.";
                    details.setText(information);
                    if (result[5] != null && !result[5].isEmpty()) {
                        try {
                            byte[] bytes = Base64.decode(result[5], Base64.NO_WRAP);
                            Bitmap image = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
                            if (image != null) {
                                thumbnail.setImageBitmap(image);
                                thumbnail.setVisibility(View.VISIBLE);
                            }
                        } catch (IllegalArgumentException ignored) {
                            // Invalid or unsupported thumbnail; metadata stays visible.
                        }
                    }
                }
            });
        } catch (final Throwable failure) {
            runOnUiThread(new Runnable() {
                @Override public void run() {
                    if (closing) return;
                    openButton.setEnabled(true);
                    status.setText("Import or inspection failed");
                    details.setText(name + "\n\n" + failure.toString());
                }
            });
        }
    }

    private String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        double value = bytes;
        String[] units = {"B", "KiB", "MiB", "GiB", "TiB"};
        int unit = 0;
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024;
            unit++;
        }
        return String.format(Locale.US, "%.1f %s", value, units[unit]);
    }

    @Override protected void onDestroy() {
        closing = true;
        executor.shutdownNow();
        super.onDestroy();
    }
}
