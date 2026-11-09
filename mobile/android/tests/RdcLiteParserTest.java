/*
 * Pure-Java offline tests for the replay-free RDC header reader.
 * SPDX-License-Identifier: MIT
 */
package org.renderdoc.mobile;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class RdcLiteParserTest {
    private static void le16(ByteArrayOutputStream out, long x) {
        out.write((int)x & 255);
        out.write((int)(x >>> 8) & 255);
    }
    private static void le32(ByteArrayOutputStream out, long x) {
        for(int i = 0; i < 4; i++) out.write((int)(x >>> (i * 8)) & 255);
    }
    private static void le64(ByteArrayOutputStream out, long x) {
        for(int i = 0; i < 8; i++) out.write((int)(x >>> (i * 8)) & 255);
    }
    private static void str(ByteArrayOutputStream out, String x, int padded) throws IOException {
        byte[] bytes = x.getBytes("UTF-8");
        out.write(bytes);
        for(int i = bytes.length; i < padded; i++) out.write(0);
    }
    private static byte[] fakeRdc() throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        le64(out, 0x434f4452L);
        le32(out, 0x102);
        // 32 header + 8 thumbnail metadata + 4 JPEG bytes
        // + 13 metadata + 7 name bytes + 16 clock bytes = 80
        le32(out, 80);
        str(out, "1.48 c041c2", 16);
        le16(out, 1920);
        le16(out, 1080);
        le32(out, 4);
        out.write(new byte[]{(byte)255,(byte)216,(byte)255,(byte)217});
        le64(out, 0x2208);
        le32(out, 8);
        out.write(7);
        str(out, "Vulkan", 7);
        le64(out, 100);
        le64(out, Double.doubleToLongBits(1000.0));
        out.write(0);
        out.write(new byte[]{0,0,0});
        le32(out, 1);
        le64(out, 239905931L);
        le64(out, 1634436032L);
        le64(out, 32);
        le32(out, 2);
        le32(out, 32);
        str(out, "renderdoc/internal/framecapture", 32);
        // No need to create the giant section payload. The parser must not touch it.
        return out.toByteArray();
    }
    private static void check(boolean pass, String message) {
        if(!pass) throw new AssertionError(message);
    }
    private static byte[] zipRdc(byte[] rdc) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ZipOutputStream zip = new ZipOutputStream(out);
        zip.putNextEntry(new ZipEntry("test.rdc"));
        zip.write(rdc);
        zip.closeEntry();
        zip.finish();
        return out.toByteArray();
    }
    private static void mustFail(byte[] bytes, String message) throws IOException {
        try {
            RdcLiteParser.read(new ByteArrayInputStream(bytes), "invalid");
            throw new AssertionError("Accepted " + message);
        } catch (IOException expected) { /* reject corrupted or truncated captures */ }
    }
    public static void main(String[] args) throws Exception {
        byte[] rdc = fakeRdc();
        RdcLiteParser.CaptureSummary a =
            RdcLiteParser.read(new ByteArrayInputStream(rdc), "test.rdc");
        check(!a.fromZip && "Vulkan".equals(a.driverName), "RDC header metadata");
        check(a.headerBytes == 80 && a.thumbnailBytes == 4, "RDC thumbnail bounds");
        check(a.firstSectionCompressed == 239905931L &&
              a.firstSectionUncompressed == 1634436032L, "Large frame section sizes");
        check(a.thumbnailJpeg != null, "Tiny JPEG extracted");
        byte[] inputZip = zipRdc(rdc);
        RdcLiteParser.CaptureSummary b =
            RdcLiteParser.read(new ByteArrayInputStream(inputZip), "test.zip");
        check(b.fromZip && b.entryName.equals("test.rdc"), "Streaming ZIP member");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        RdcLiteParser.export(new ByteArrayInputStream(inputZip), "test.zip", out);
        ZipInputStream exported = new ZipInputStream(new ByteArrayInputStream(out.toByteArray()));
        int entries = 0;
        ZipEntry e;
        while((e = exported.getNextEntry()) != null) {
            entries++;
            check(!e.getName().endsWith(".rdc"), "Output must not duplicate RDC payload");
        }
        check(entries == 3, "Expected metadata, preview, limitations");
        mustFail(new byte[]{0,1,2,3}, "wrong magic");
        mustFail(java.util.Arrays.copyOf(rdc, 25), "truncated RDC");
        byte[] corrupt = rdc.clone();
        corrupt[36] = (byte)255; corrupt[37] = (byte)255;
        corrupt[38] = (byte)255; corrupt[39] = (byte)127;
        mustFail(corrupt, "invalid thumbnail length");
        System.out.println("PASS: RDC and ZIP streaming, bounds, replay-free export");
    }
}
