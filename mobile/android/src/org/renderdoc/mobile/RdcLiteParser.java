/*
 * Replay-free RenderDoc RDC header inspector for Android.
 * Copyright (c) 2026 RenderDoc Mobile contributors.
 * SPDX-License-Identifier: MIT
 *
 * Pure Java: works on an RDC or an RDC inside a ZIP, reads only the
 * capture header and first section header, and never opens the GPU.
 */
package org.renderdoc.mobile;

import java.io.BufferedInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class RdcLiteParser {
    private static final long MAGIC = 0x434f4452L; // RDOC + 4 null bytes
    private static final long MAX_HEADER = 16L * 1024 * 1024;
    private static final long MAX_THUMBNAIL = 10L * 1024 * 1024;
    private static final long EXPORTED_THUMBNAIL = 2L * 1024 * 1024;
    private static final int MAX_ENTRIES = 32;
    private static final int MAX_SECTION_NAME = 2048;

    private RdcLiteParser() { }

    public static final class CaptureSummary {
        public String sourceName;
        public String entryName;
        public boolean fromZip;
        public long declaredRdcBytes = -1;
        public long serialiseVersion;
        public String renderdocVersion;
        public long headerBytes;
        public int thumbnailWidth, thumbnailHeight;
        public long thumbnailBytes;
        public byte[] thumbnailJpeg;
        public long machineId, driverId;
        public String driverName;
        public long timeBase;
        public double timeFrequency;
        public String firstSectionName, firstSectionStatus;
        public long firstSectionType, firstSectionVersion, firstSectionCompressed;
        public long firstSectionUncompressed, firstSectionFlags;

        public String toJson() {
            StringBuilder b = new StringBuilder(1400);
            b.append("{\n  \"schema\": 1,");
            b.append("\n  \"extraction\": \"streamed RDC header only; no Vulkan replay\",");
            b.append("\n  \"source_name\": ").append(quote(sourceName)).append(',');
            b.append("\n  \"container\": ").append(quote(fromZip ? "zip" : "rdc")).append(',');
            b.append("\n  \"rdc_entry\": ").append(quote(entryName)).append(',');
            b.append("\n  \"declared_rdc_bytes\": ").append(declaredRdcBytes >= 0 ? String.valueOf(declaredRdcBytes) : "null").append(',');
            b.append("\n  \"rdc_serialisation_version\": ").append(serialiseVersion).append(',');
            b.append("\n  \"renderdoc_build\": ").append(quote(renderdocVersion)).append(',');
            b.append("\n  \"header_bytes\": ").append(headerBytes).append(',');
            b.append("\n  \"driver_api\": ").append(quote(driverName)).append(',');
            b.append("\n  \"rdc_driver_id\": ").append(driverId).append(',');
            b.append("\n  \"recorded_machine_id_hex\": ").append(quote("0x" + Long.toHexString(machineId))).append(',');
            b.append("\n  \"actual_gpu_driver_verified\": false,");
            b.append("\n  \"capture_time_base\": ").append(quote(Long.toUnsignedString(timeBase))).append(',');
            b.append("\n  \"capture_time_frequency\": ").append(Double.isFinite(timeFrequency) ? timeFrequency : "null").append(',');
            b.append("\n  \"thumbnail\": {\"width\": ").append(thumbnailWidth);
            b.append(", \"height\": ").append(thumbnailHeight);
            b.append(", \"bytes_in_rdc\": ").append(thumbnailBytes);
            b.append(", \"image_exported\": ").append(thumbnailJpeg != null).append("},");
            b.append("\n  \"first_section\": {\"status\": ").append(quote(firstSectionStatus));
            b.append(", \"name\": ").append(quote(firstSectionName));
            b.append(", \"type\": ").append(firstSectionType);
            b.append(", \"version\": ").append(firstSectionVersion);
            b.append(", \"compressed_bytes\": ").append(firstSectionCompressed);
            b.append(", \"uncompressed_bytes\": ").append(firstSectionUncompressed);
            b.append(", \"flags\": ").append(firstSectionFlags).append("},");
            b.append("\n  \"frame_draws_extracted\": false,");
            b.append("\n  \"shader_bytes_extracted\": false,");
            b.append("\n  \"post_vertex_positions_extracted\": false,");
            b.append("\n  \"note\": \"The RDC header identifies Vulkan, not Turnip versus Qualcomm. Verify the actual driver from Strato emulation.log.\"\n}");
            return b.toString();
        }
    }

    private static final class Cursor {
        private final InputStream input;
        long offset;
        Cursor(InputStream source) { input = source; }
        int u8() throws IOException {
            int x = input.read();
            if(x == -1) throw new EOFException("Truncated RDC header at offset " + offset);
            offset++;
            return x;
        }
        int u16() throws IOException { return u8() | (u8() << 8); }
        long u32() throws IOException {
            return (long)u8() | ((long)u8() << 8) | ((long)u8() << 16) | ((long)u8() << 24);
        }
        long u64() throws IOException { return u32() | (u32() << 32); }
        byte[] bytes(int n) throws IOException {
            if(n < 0 || n > MAX_HEADER) throw new IOException("Invalid RDC header length");
            byte[] out = new byte[n];
            int p = 0;
            while(p < n) {
                int read = input.read(out, p, n - p);
                if(read < 0) throw new EOFException("Truncated RDC header at offset " + offset);
                if(read == 0) { out[p++] = (byte)u8(); continue; }
                p += read;
                offset += read;
            }
            return out;
        }
        void skip(long n) throws IOException {
            if(n < 0 || n > MAX_HEADER) throw new IOException("Invalid RDC header skip");
            byte[] buffer = new byte[8192];
            while(n > 0) {
                int read = input.read(buffer, 0, (int)Math.min(n, buffer.length));
                if(read < 0) throw new EOFException("Truncated RDC header at offset " + offset);
                if(read == 0) { u8(); n--; continue; }
                offset += read;
                n -= read;
            }
        }
    }

    private static String clean(byte[] bytes) {
        int n = 0;
        while(n < bytes.length && bytes[n] != 0) n++;
        return new String(bytes, 0, n, StandardCharsets.UTF_8);
    }

    private static CaptureSummary parseRdc(InputStream stream, String sourceName,
                                            String entryName, boolean inZip,
                                            long declaredBytes) throws IOException {
        Cursor in = new Cursor(stream);
        CaptureSummary s = new CaptureSummary();
        s.sourceName = sourceName;
        s.entryName = entryName;
        s.fromZip = inZip;
        s.declaredRdcBytes = declaredBytes;
        if(in.u64() != MAGIC) throw new IOException("Not an RDC: missing RDOC magic");
        s.serialiseVersion = in.u32();
        s.headerBytes = in.u32();
        if(s.serialiseVersion < 0x100 || s.serialiseVersion > 0x102)
            throw new IOException("Unsupported RDC serialisation version: " + s.serialiseVersion);
        if(s.headerBytes < 48 || s.headerBytes > MAX_HEADER)
            throw new IOException("Invalid RDC header length: " + s.headerBytes);
        s.renderdocVersion = clean(in.bytes(16));
        s.thumbnailWidth = in.u16();
        s.thumbnailHeight = in.u16();
        s.thumbnailBytes = in.u32();
        if(s.thumbnailBytes > MAX_THUMBNAIL || in.offset + s.thumbnailBytes > s.headerBytes)
            throw new IOException("Invalid RDC thumbnail length");
        if(s.thumbnailBytes > 0 && s.thumbnailBytes <= EXPORTED_THUMBNAIL) {
            byte[] image = in.bytes((int)s.thumbnailBytes);
            if(image.length > 2 && (image[0] & 255) == 255 && (image[1] & 255) == 216)
                s.thumbnailJpeg = image;
        } else in.skip(s.thumbnailBytes);
        s.machineId = in.u64();
        s.driverId = in.u32();
        int driverNameLength = in.u8();
        if(driverNameLength < 1 || in.offset + driverNameLength > s.headerBytes)
            throw new IOException("Invalid RDC driver name length");
        s.driverName = clean(in.bytes(driverNameLength));
        if(s.serialiseVersion >= 0x102) {
            s.timeBase = in.u64();
            s.timeFrequency = Double.longBitsToDouble(in.u64());
        } else s.timeFrequency = 1.0;
        if(in.offset > s.headerBytes) throw new IOException("RDC fields exceed header length");
        in.skip(s.headerBytes - in.offset);
        int sectionMarker = in.u8();
        if(sectionMarker == 0) {
            in.skip(3);
            s.firstSectionType = in.u32();
            s.firstSectionCompressed = in.u64();
            s.firstSectionUncompressed = in.u64();
            s.firstSectionVersion = in.u64();
            s.firstSectionFlags = in.u32();
            long nameLength = in.u32();
            if(nameLength < 1 || nameLength > MAX_SECTION_NAME)
                throw new IOException("Invalid first RDC section name length");
            s.firstSectionName = clean(in.bytes((int)nameLength));
            s.firstSectionStatus = "binary_section_header_only";
        } else if(sectionMarker == 'A') {
            s.firstSectionStatus = "ASCII_section_not_scanned";
            s.firstSectionName = "";
        } else throw new IOException("Unrecognised RDC section header type: " + sectionMarker);
        return s;
    }

    public static CaptureSummary read(InputStream source, String name) throws IOException {
        BufferedInputStream buffered = new BufferedInputStream(source, 64 * 1024);
        buffered.mark(8);
        byte[] magic = new byte[4];
        int count = buffered.read(magic);
        buffered.reset();
        if(count != 4) throw new IOException("Empty or truncated capture");
        if(magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4) {
            ZipInputStream zip = new ZipInputStream(buffered);
            ZipEntry entry;
            int entries = 0;
            while((entry = zip.getNextEntry()) != null && ++entries <= MAX_ENTRIES) {
                if(!entry.isDirectory() && entry.getName().toLowerCase(Locale.ROOT).endsWith(".rdc"))
                    return parseRdc(zip, name, entry.getName(), true, entry.getSize());
                // Avoid decompressing unrelated large files before the RDC.
                if(entry.getSize() < 0 || entry.getSize() > 1024 * 1024)
                    throw new IOException("Large non-RDC ZIP member before capture");
                zip.closeEntry();
            }
            throw new IOException("No RDC in the ZIP's first " + MAX_ENTRIES + " entries");
        }
        return parseRdc(buffered, name, name, false, -1);
    }

    private static String quote(String s) {
        if(s == null) return "null";
        StringBuilder b = new StringBuilder(s.length() + 4).append('"');
        for(int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if(c == '"' || c == '\\') b.append('\\').append(c);
            else if(c == '\n') b.append("\\n");
            else if(c == '\r') b.append("\\r");
            else if(c == '\t') b.append("\\t");
            else if(c < 32) {
                String hex = Integer.toHexString(c);
                b.append("\\u");
                for(int p = hex.length(); p < 4; p++) b.append('0');
                b.append(hex);
            } else b.append(c);
        }
        return b.append('"').toString();
    }

    private static void entry(ZipOutputStream zip, String name, byte[] data) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(data);
        zip.closeEntry();
    }

    public static void export(InputStream source, String sourceName, OutputStream destination)
            throws IOException {
        CaptureSummary summary = read(source, sourceName);
        ZipOutputStream zip = new ZipOutputStream(destination);
        entry(zip, "capture-metadata.json", summary.toJson().getBytes(StandardCharsets.UTF_8));
        if(summary.thumbnailJpeg != null)
            entry(zip, "capture-preview.jpg", summary.thumbnailJpeg);
        String limitations =
            "RenderDoc Mobile: lightweight, replay-free capture inspection\n\n" +
            "Parsed only the RDC header, embedded thumbnail and first section header.\n" +
            "The framecapture payload was NOT decompressed and the GPU was NOT used.\n" +
            "This ZIP does NOT include shaders, vertex/index buffers, per-draw states,\n" +
            "clipping statistics, or Post-VS gl_Position. It cannot prove the source\n" +
            "of missing Zelda geometry. No fake data is substituted.\n\n" +
            "For Zelda: Echoes of Wisdom, send this ZIP and Strato emulation.log\n" +
            "to verify Turnip versus Qualcomm. Preserve the original RDC.\n";
        entry(zip, "limitations.txt", limitations.getBytes(StandardCharsets.UTF_8));
        zip.finish();
        zip.flush();
    }
}
