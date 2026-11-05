/*
 * Copyright (c) 2026 RenderDoc Mobile contributors.
 * SPDX-License-Identifier: MIT
 */
package org.renderdoc.mobile;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.util.TypedValue;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class DemoCaptureActivity extends Activity {
    private static final int SAVE_RDC = 2012;
    private static final int BACKGROUND = Color.rgb(21, 24, 29);
    private static final int MUTED = Color.rgb(170, 179, 194);
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private TextView info;
    private Button startButton, inspectButton, exportButton;
    private volatile boolean stopped;
    private String captureFile;

    private static final String NATIVE_LOAD_ERROR;
    static {
        String error = null;
        try {
            System.loadLibrary("renderdoc_vulkan_demo");
        } catch (Throwable t) {
            error = t.getClass().getSimpleName() + ": " + t.getMessage();
        }
        NATIVE_LOAD_ERROR = error;
    }

    // [status, capture path, diagnostic].
    private static native String[] captureOffscreenVulkan(String directory);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BACKGROUND);
        getWindow().setNavigationBarColor(BACKGROUND);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACKGROUND);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(20), dp(25), dp(20), dp(24));
        scroll.addView(page);
        setContentView(scroll);

        TextView title = text("Vulkan capture test", 24, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        page.addView(title);
        TextView intro = text("Runs real offscreen Vulkan commands on your GPU. " +
                "RenderDoc must intercept and write the .rdc file; no placeholder capture is created.",
                14, MUTED);
        intro.setPadding(0, dp(8), 0, dp(18));
        page.addView(intro);

        startButton = button(page, "Generate Vulkan .rdc");
        info = text("No capture created yet. If Vulkan cannot load the capture " +
                "layer, the exact diagnostic will appear here.", 14, Color.WHITE);
        info.setPadding(0, dp(15), 0, dp(18));
        page.addView(info);
        inspectButton = button(page, "Inspect generated capture");
        exportButton = button(page, "Export .rdc to Downloads");
        inspectButton.setEnabled(false);
        exportButton.setEnabled(false);

        startButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { runCapture(); }
        });
        inspectButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (captureFile == null) return;
                Intent target = new Intent(DemoCaptureActivity.this, MainActivity.class);
                target.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
                target.putExtra("generated_capture_path", captureFile);
                startActivity(target);
                finish();
            }
        });
        exportButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (captureFile == null) return;
                Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                save.addCategory(Intent.CATEGORY_OPENABLE);
                save.setType("application/octet-stream");
                save.putExtra(Intent.EXTRA_TITLE, new File(captureFile).getName());
                startActivityForResult(save, SAVE_RDC);
            }
        });

        if (NATIVE_LOAD_ERROR != null) {
            info.setText("Native Vulkan test library unavailable:\n" + NATIVE_LOAD_ERROR);
            startButton.setEnabled(false);
        }
    }

    private void runCapture() {
        final File directory = getExternalFilesDir("captures");
        if (directory == null || (!directory.isDirectory() && !directory.mkdirs())) {
            info.setText("Could not create the capture output folder.");
            return;
        }
        startButton.setEnabled(false);
        inspectButton.setEnabled(false);
        exportButton.setEnabled(false);
        captureFile = null;
        info.setText("Checking capture layer and executing Vulkan commands…");

        worker.execute(new Runnable() {
            @Override public void run() {
                String[] result;
                try {
                    result = captureOffscreenVulkan(directory.getAbsolutePath());
                    if (result == null || result.length != 3)
                        throw new IOException("Invalid result from native capture test.");
                } catch (Throwable error) {
                    result = new String[]{"error", "", error.toString()};
                }
                final String[] response = result;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        if (stopped) return;
                        startButton.setEnabled(true);
                        if ("ok".equals(response[0])) {
                            captureFile = response[1];
                            inspectButton.setEnabled(true);
                            exportButton.setEnabled(true);
                            info.setText("A real .rdc was saved successfully!\n\n" +
                                    response[2] + "\n\n" + captureFile);
                        } else {
                            info.setText("Capture not created.\n\n" + response[2]);
                        }
                    }
                });
            }
        });
    }

    @Override protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (request != SAVE_RDC || code != RESULT_OK || data == null ||
                data.getData() == null || captureFile == null) return;
        final String filename = captureFile;
        final Uri destination = data.getData();
        worker.execute(new Runnable() {
            @Override public void run() {
                String result;
                try (InputStream in = new FileInputStream(filename);
                     OutputStream out = getContentResolver().openOutputStream(destination)) {
                    if (out == null) throw new IOException("Destination could not be opened.");
                    byte[] buffer = new byte[262144];
                    int count;
                    while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                    out.flush();
                    result = "Capture exported successfully.";
                } catch (IOException error) {
                    result = "Export failed: " + error.getMessage();
                }
                final String message = result;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        if (!stopped) info.append("\n\n" + message);
                    }
                });
            }
        });
    }

    private int dp(int n) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, n, getResources().getDisplayMetrics());
    }
    private TextView text(String value, int size, int color) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextColor(color);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        t.setLineSpacing(dp(2), 1.0f);
        return t;
    }
    private Button button(LinearLayout page, String label) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextColor(Color.WHITE);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(54));
        params.bottomMargin = dp(10);
        page.addView(button, params);
        return button;
    }
    @Override protected void onDestroy() {
        stopped = true;
        worker.shutdownNow();
        super.onDestroy();
    }
}
