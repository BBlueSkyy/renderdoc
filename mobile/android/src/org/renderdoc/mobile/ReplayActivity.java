/*
 * RenderDoc Mobile - Vulkan replay event and texture viewer.
 * SPDX-License-Identifier: MIT
 *
 * This Activity lives in a separate Android process to isolate replay memory,
 * GPU driver failures and Vulkan state from the metadata inspector.
 */
package org.renderdoc.mobile;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Environment;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ReplayActivity extends Activity {
    private static final int BACKGROUND = Color.rgb(21, 24, 29);
    private static final int PANEL = Color.rgb(34, 39, 47);
    private static final int ACCENT = Color.rgb(89, 163, 239);
    private static final int MUTED = Color.rgb(170, 179, 194);
    private static final int PAGE_SIZE = 40;
    private static final int SAVE_AI_DIAGNOSTICS = 1901;
    private static final String[] AI_FILES = {
        "README.txt", "manifest.json", "events.jsonl", "resources.jsonl",
        "textures.jsonl", "buffers.jsonl", "resource-usage.jsonl",
        "pipeline-samples.jsonl", "shader-disassembly.jsonl", "debug-messages.jsonl",
        "descriptor-details.jsonl", "hdr-snapshots.jsonl", "material-textures.jsonl",
        "shader-artifacts.jsonl"
    };

    private static final String LOAD_ERROR;
    static {
        String failure = null;
        try {
            // Unlike the metadata inspector, this process must be recognised
            // as a replay app before the RenderDoc core library is constructed.
            System.loadLibrary("renderdoc_replay_marker");
            if(!ReplayMarker.enableGlobal())
                throw new UnsatisfiedLinkError("Replay marker not globally visible to Android linker");
            System.loadLibrary("renderdoc_replay_jni");
        } catch (Throwable error) {
            failure = error.toString();
        }
        LOAD_ERROR = failure;
    }

    private static native String[] nativeOpen(String capturePath, String diagnosticsPath);
    private static native String[] nativeActions(int start, int size);
    private static native String[] nativeTextures(int start, int size);
    private static native String nativeSeek(int eventId);
    private static native String nativeExportAI(String directory);
    private static native byte[] nativeTexturePreview(int textureIndex, int maxSide);
    private static native String nativeLastPreviewError();
    private static native void nativeClose();

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean destroyed;
    private boolean opened;
    private boolean exportAIMode;
    private File aiReportDirectory;
    private boolean showingTextures;
    private int currentPage;
    private int totalActions;
    private int totalTextures;
    private int selectedTexture = -1;
    private TextView status;
    private TextView detail;
    private LinearLayout listing;
    private ImageView preview;
    private Button actionsButton;
    private Button texturesButton;
    private Button previousButton;
    private Button nextButton;

    @Override public void onCreate(Bundle bundle) {
        super.onCreate(bundle);
        exportAIMode = getIntent().getBooleanExtra("ai_export", false);
        getWindow().setStatusBarColor(BACKGROUND);
        getWindow().setNavigationBarColor(BACKGROUND);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACKGROUND);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(18), dp(18), dp(18), dp(32));
        scroll.addView(page);
        setContentView(scroll);

        page.addView(label(exportAIMode ? "AI Vulkan diagnostics" : "Vulkan frame replay", 24, Color.WHITE, true));
        TextView explanation = label(
                exportAIMode
                    ? "Automatic Vulkan events, resources, sampled pipelines and shaders " +
                      "for AI analysis. The original RDC must be shared separately."
                    : "Actual GPU replay / Event Browser / Texture Viewer. " +
                      "Large captures may exhaust device memory.", 12, MUTED, false);
        explanation.setPadding(0, dp(8), 0, dp(14));
        page.addView(explanation);

        status = label("Opening capture on the GPU…", 16, Color.WHITE, true);
        page.addView(status);
        detail = label("Replay is running in an isolated Android process.", 13, MUTED, false);
        detail.setPadding(0, dp(5), 0, dp(12));
        page.addView(detail);

        preview = new ImageView(this);
        preview.setAdjustViewBounds(true);
        preview.setScaleType(ImageView.ScaleType.FIT_CENTER);
        preview.setMaxHeight(dp(290));
        preview.setVisibility(View.GONE);
        page.addView(preview, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        page.addView(tabs);
        actionsButton = button("Draw calls / events", tabs);
        texturesButton = button("Textures", tabs);
        actionsButton.setEnabled(false);
        texturesButton.setEnabled(false);
        actionsButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { switchMode(false); }
        });
        texturesButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { switchMode(true); }
        });

        LinearLayout pager = new LinearLayout(this);
        pager.setOrientation(LinearLayout.HORIZONTAL);
        page.addView(pager);
        previousButton = button("Previous", pager);
        nextButton = button("Next", pager);
        previousButton.setEnabled(false);
        nextButton.setEnabled(false);
        previousButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if(currentPage > 0) {
                    currentPage--;
                    fetchPage();
                }
            }
        });
        nextButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if((currentPage + 1) * PAGE_SIZE <
                        (showingTextures ? totalTextures : totalActions)) {
                    currentPage++;
                    fetchPage();
                }
            }
        });

        listing = new LinearLayout(this);
        listing.setOrientation(LinearLayout.VERTICAL);
        listing.setPadding(dp(10), dp(10), dp(10), dp(15));
        listing.setBackgroundColor(PANEL);
        page.addView(listing);

        if(LOAD_ERROR != null) {
            status.setText("Replay library unavailable");
            detail.setText(LOAD_ERROR);
            return;
        }

        final String path = getIntent().getStringExtra("capture_path");
        final File file = validateCapture(path);
        if(file == null) {
            status.setText("Capture path rejected");
            detail.setText("Only .rdc files imported into this app can be replayed.");
            return;
        }

        ReplayDiagnostics.begin(this, file);
        // Keep the native subprocess disposable, but retain Java exception
        // details if the failure is in UI/JNI glue rather than the GPU driver.
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override public void uncaughtException(Thread thread, Throwable problem) {
                java.io.StringWriter text = new java.io.StringWriter();
                problem.printStackTrace(new java.io.PrintWriter(text));
                ReplayDiagnostics.append(ReplayActivity.this, "Uncaught Java exception: " + text.toString());
                if(previous != null) previous.uncaughtException(thread, problem);
            }
        });
        worker.execute(new Runnable() {
            @Override public void run() {
                try {
                    final String[] result = nativeOpen(
                            file.getAbsolutePath(), ReplayDiagnostics.traceFile(ReplayActivity.this).getAbsolutePath());
                    ReplayDiagnostics.append(ReplayActivity.this, result != null &&
                            result.length > 0 && "ok".equals(result[0])
                            ? "nativeOpen returned successfully"
                            : "nativeOpen returned an error");
                    post(new Runnable() {
                        @Override public void run() {
                            if(result == null || result.length < 4 || !"ok".equals(result[0])) {
                                status.setText("Vulkan replay unavailable");
                                detail.setText(result != null && result.length > 1 ?
                                        result[1] : "Unknown replay initialization failure");
                                return;
                            }
                            opened = true;
                            totalActions = Integer.parseInt(result[2]);
                            totalTextures = Integer.parseInt(result[3]);
                            status.setText("Vulkan replay opened");
                            detail.setText(result[1] + "\nEvents: " + totalActions +
                                    "  /  Textures: " + totalTextures +
                                    "\nSelect an event, then a texture to see its replayed contents.");
                            if(exportAIMode) {
                                actionsButton.setVisibility(View.GONE);
                                texturesButton.setVisibility(View.GONE);
                                previousButton.setVisibility(View.GONE);
                                nextButton.setVisibility(View.GONE);
                                listing.removeAllViews();
                                beginAIExport();
                            } else {
                                actionsButton.setEnabled(true);
                                texturesButton.setEnabled(true);
                                fetchPage();
                            }
                        }
                    });
                } catch(final Throwable failure) {
                    ReplayDiagnostics.append(ReplayActivity.this,
                            "Replay worker exception: " + failure.toString());
                    post(new Runnable() {
                        @Override public void run() {
                            status.setText("Replay error");
                            detail.setText(failure.toString());
                        }
                    });
                }
            }
        });
    }

    private void beginAIExport() {
        status.setText("Extracting Vulkan diagnostics...");
        detail.setText("Reading events, resources and sampled replay states.\n" +
                "Large game captures can use substantial GPU memory.");
        final File folder = new File(getCacheDir(), "ai-diagnostics");
        worker.execute(new Runnable() {
            @Override public void run() {
                try {
                    if(!folder.exists() && !folder.mkdirs())
                        throw new IOException("Cannot create private diagnostic folder");
                    File[] old = folder.listFiles();
                    if(old != null)
                        for(File entry : old)
                            if(entry.isFile() && !entry.delete())
                                throw new IOException("Could not clear previous diagnostics");
                    final String outcome = nativeExportAI(folder.getAbsolutePath());
                    if(outcome == null || !outcome.startsWith("ok:"))
                        throw new IOException(outcome == null ? "Native extraction failed" : outcome);
                    aiReportDirectory = folder;
                    ReplayDiagnostics.append(ReplayActivity.this, "AI report: " + outcome);
                    post(new Runnable() {
                        @Override public void run() {
                            status.setText("AI diagnostic report ready");
                            detail.setText(outcome + "\n\nChoose where to save the ZIP. " +
                                    "Then send it with the original .rdc and Strato emulation.log.");
                            Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                            save.setType("application/zip");
                            save.addCategory(Intent.CATEGORY_OPENABLE);
                            save.putExtra(Intent.EXTRA_TITLE, "renderdoc-ai-diagnostics.zip");
                            startActivityForResult(save, SAVE_AI_DIAGNOSTICS);
                        }
                    });
                } catch(final Throwable failure) {
                    ReplayDiagnostics.append(ReplayActivity.this,
                            "AI export failed: " + failure.toString());
                    post(new Runnable() {
                        @Override public void run() {
                            status.setText("AI diagnostics unavailable");
                            detail.setText(failure.toString() +
                                    "\n\nYou can still send the original .rdc and emulation.log. " +
                                    "If the subprocess crashed, export replay crash diagnostics " +
                                    "from the main screen.");
                        }
                    });
                }
            }
        });
    }

    @Override protected void onActivityResult(int request, int resultCode, Intent data) {
        super.onActivityResult(request, resultCode, data);
        if(request != SAVE_AI_DIAGNOSTICS) return;
        if(resultCode != RESULT_OK || data == null || data.getData() == null) {
            status.setText("Report generated; ZIP saving cancelled");
            return;
        }
        final Uri destination = data.getData();
        final File folder = aiReportDirectory;
        status.setText("Writing AI diagnostics ZIP...");
        worker.execute(new Runnable() {
            @Override public void run() {
                try {
                    if(folder == null || !folder.isDirectory())
                        throw new IOException("Diagnostic files not found");
                    OutputStream raw = getContentResolver().openOutputStream(destination, "w");
                    if(raw == null) throw new IOException("Cannot write selected file");
                    try(ZipOutputStream zip = new ZipOutputStream(raw)) {
                        long total = 0;
                        byte[] buffer = new byte[32 * 1024];
                        for(String name : AI_FILES) {
                            File source = new File(folder, name);
                            if(!source.isFile())
                                throw new IOException("Missing diagnostic file: " + name);
                            zip.putNextEntry(new ZipEntry(name));
                            try(InputStream input = new FileInputStream(source)) {
                                int count;
                                while((count = input.read(buffer)) != -1) {
                                    total += count;
                                    if(total > 96L * 1024L * 1024L)
                                        throw new IOException("Diagnostics exceeded 96 MiB limit");
                                    zip.write(buffer, 0, count);
                                }
                            }
                            zip.closeEntry();
                        }
                        // Include only native-generated assets, not arbitrary user files.
                        // The entire report is capped so a 700 MiB game capture does
                        // not force a similarly huge upload from the user's phone.
                        File[] assets = folder.listFiles();
                        ArrayList<String> skipped = new ArrayList<String>();
                        if(assets != null) {
                            Arrays.sort(assets, new Comparator<File>() {
                                @Override public int compare(File a, File b) {
                                    return a.getName().compareTo(b.getName());
                                }
                            });
                            int included = 0;
                            for(File asset : assets) {
                                String name = asset.getName();
                                if(!asset.isFile() || !name.matches(
                                        "^(snapshot_|material_|fragment_)[A-Za-z0-9_]+\\.(png|exr|dds|spv|shaderbin)$"))
                                    continue;
                                if(included >= 70 || asset.length() <= 0 ||
                                        asset.length() > 32L * 1024L * 1024L ||
                                        total + asset.length() > 192L * 1024L * 1024L) {
                                    skipped.add(name + ": size/file-count budget");
                                    continue;
                                }
                                zip.putNextEntry(new ZipEntry(name));
                                try(InputStream input = new FileInputStream(asset)) {
                                    int count;
                                    while((count = input.read(buffer)) != -1) {
                                        total += count;
                                        zip.write(buffer, 0, count);
                                    }
                                }
                                zip.closeEntry();
                                included++;
                            }
                        }
                        if(!skipped.isEmpty()) {
                            zip.putNextEntry(new ZipEntry("assets-omitted.txt"));
                            StringBuilder warning = new StringBuilder(
                                    "Some binary image/shader assets were omitted to limit the ZIP size:\n");
                            for(String item : skipped) warning.append(item).append('\n');
                            byte[] omission = warning.toString().getBytes("UTF-8");
                            zip.write(omission, 0, omission.length);
                            zip.closeEntry();
                        }
                        File trace = ReplayDiagnostics.traceFile(ReplayActivity.this);
                        if(trace.isFile()) {
                            zip.putNextEntry(new ZipEntry("replay-steps.txt"));
                            try(InputStream input = new FileInputStream(trace)) {
                                int count;
                                while((count = input.read(buffer)) != -1) {
                                    total += count;
                                    if(total > 96L * 1024L * 1024L)
                                        throw new IOException("Diagnostics size limit exceeded");
                                    zip.write(buffer, 0, count);
                                }
                            }
                            zip.closeEntry();
                        }
                    }
                    post(new Runnable() {
                        @Override public void run() {
                            status.setText("AI diagnostics exported");
                            detail.setText("ZIP saved. Send the ZIP and original .rdc, " +
                                    "with emulation.log, for analysis. No code defect is " +
                                    "automatically proven by this report.");
                        }
                    });
                } catch(final Throwable failure) {
                    post(new Runnable() {
                        @Override public void run() {
                            status.setText("AI diagnostics save failed");
                            detail.setText(failure.toString() +
                                    "\nDiscard the incomplete ZIP. Retry the export.");
                        }
                    });
                }
            }
        });
    }

    private File validateCapture(String path) {
        if(path == null || !path.endsWith(".rdc")) return null;
        try {
            final File file = new File(path).getCanonicalFile();
            if(!file.isFile()) return null;
            File documents = getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS);
            File captures = getExternalFilesDir("captures");
            File privateDir = getFilesDir();
            File[] roots = {documents, captures, privateDir};
            for(File root : roots) {
                if(root != null && file.getPath().startsWith(
                        root.getCanonicalPath() + File.separator)) return file;
            }
        } catch(IOException ignored) { }
        return null;
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP,
                value, getResources().getDisplayMetrics());
    }

    private TextView label(String value, int size, int color, boolean bold) {
        TextView text = new TextView(this);
        text.setText(value);
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        text.setTextColor(color);
        text.setPadding(dp(4), dp(5), dp(4), dp(5));
        if(bold) text.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return text;
    }

    private Button button(String title, LinearLayout parent) {
        Button b = new Button(this);
        b.setText(title);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                0, dp(52), 1.0f);
        params.rightMargin = dp(4);
        parent.addView(b, params);
        return b;
    }

    private void post(final Runnable callback) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if(!destroyed && !isFinishing()) callback.run();
            }
        });
    }

    private void switchMode(boolean textures) {
        if(!opened) return;
        showingTextures = textures;
        currentPage = 0;
        fetchPage();
    }

    private void fetchPage() {
        if(!opened) return;
        final boolean mode = showingTextures;
        final int start = currentPage * PAGE_SIZE;
        final int total = mode ? totalTextures : totalActions;
        previousButton.setEnabled(false);
        nextButton.setEnabled(false);
        listing.removeAllViews();
        listing.addView(label("Loading replay resources…", 13, MUTED, false));
        worker.execute(new Runnable() {
            @Override public void run() {
                try {
                    final String[] rows = mode
                            ? nativeTextures(start, PAGE_SIZE)
                            : nativeActions(start, PAGE_SIZE);
                    post(new Runnable() {
                        @Override public void run() {
                            if(mode != showingTextures || start != currentPage * PAGE_SIZE) return;
                            listing.removeAllViews();
                            TextView summary = label((mode ? "Textures" : "Actions") +
                                    "  " + (start + 1) + "–" +
                                    Math.min(total, start + PAGE_SIZE) + " of " + total,
                                    13, MUTED, true);
                            listing.addView(summary);
                            if(rows == null || rows.length == 0) {
                                listing.addView(label("No entries.", 13, MUTED, false));
                            } else {
                                for(String row : rows) addItem(row, mode);
                            }
                            previousButton.setEnabled(currentPage > 0);
                            nextButton.setEnabled((currentPage + 1) * PAGE_SIZE < total);
                        }
                    });
                } catch(final Throwable failure) {
                    post(new Runnable() {
                        @Override public void run() {
                            listing.removeAllViews();
                            listing.addView(label(failure.toString(), 13, MUTED, false));
                        }
                    });
                }
            }
        });
    }

    private void addItem(String row, final boolean textureMode) {
        final String[] fields = row.split("\t", 3);
        if(fields.length < 3) return;
        final int identifier;
        try { identifier = Integer.parseInt(fields[0]); }
        catch(NumberFormatException ignored) { return; }

        Button b = new Button(this);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        if(textureMode) {
            b.setText("Texture " + fields[0] + "   " + fields[1] + "\n" + fields[2]);
        } else {
            int depth = 0;
            try { depth = Math.min(8, Integer.parseInt(fields[1])); }
            catch(NumberFormatException ignored) { }
            String indent = "";
            for(int i = 0; i < depth; i++) indent += "  ";
            b.setText("EID " + identifier + "   " + indent + fields[2]);
        }
        listing.addView(b, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        b.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if(textureMode) showTexture(identifier);
                else seekEvent(identifier);
            }
        });
    }

    private void seekEvent(final int event) {
        status.setText("Replaying event " + event + "…");
        worker.execute(new Runnable() {
            @Override public void run() {
                try {
                    final String result = nativeSeek(event);
                    post(new Runnable() {
                        @Override public void run() {
                            status.setText("Vulkan replay");
                            detail.setText(result);
                            preview.setVisibility(View.GONE);
                        }
                    });
                    // GPU texture previews must be updated after the event changes.
                    if(selectedTexture >= 0) previewTexture(selectedTexture);
                } catch(final Throwable failure) {
                    post(new Runnable() {
                        @Override public void run() {
                            status.setText("Replay event failed");
                            detail.setText(failure.toString());
                        }
                    });
                }
            }
        });
    }

    private void showTexture(int index) {
        selectedTexture = index;
        status.setText("Rendering texture " + index + "…");
        worker.execute(new Runnable() {
            @Override public void run() {
                previewTexture(selectedTexture);
            }
        });
    }

    private void previewTexture(final int index) {
        try {
            final byte[] rgba = nativeTexturePreview(index, 384);
            if(rgba == null || rgba.length < 8) {
                String problem = nativeLastPreviewError();
                throw new IOException((problem == null || problem.isEmpty())
                        ? "Texture could not be read by the replay driver."
                        : problem);
            }
            ByteBuffer header = ByteBuffer.wrap(rgba).order(ByteOrder.LITTLE_ENDIAN);
            final int width = header.getInt();
            final int height = header.getInt();
            if(width < 1 || width > 512 || height < 1 || height > 512 ||
                    rgba.length != 8 + width * height * 4)
                throw new IOException("Invalid texture preview dimensions.");
            final Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(rgba, 8, width * height * 4));
            post(new Runnable() {
                @Override public void run() {
                    preview.setImageBitmap(bitmap);
                    preview.setVisibility(View.VISIBLE);
                    status.setText("Replay texture " + index + " (" + width + " × " + height + ")");
                }
            });
        } catch(final Throwable failure) {
            post(new Runnable() {
                @Override public void run() {
                    preview.setVisibility(View.GONE);
                    status.setText("Texture preview unavailable");
                    detail.setText(failure.toString());
                }
            });
        }
    }

    @Override protected void onDestroy() {
        destroyed = true;
        worker.execute(new Runnable() {
            @Override public void run() {
                if(LOAD_ERROR == null) {
                    ReplayDiagnostics.append(ReplayActivity.this, "nativeClose entering");
                    nativeClose();
                    ReplayDiagnostics.append(ReplayActivity.this, "nativeClose completed");
                }
            }
        });
        worker.shutdown();
        super.onDestroy();
    }
}
