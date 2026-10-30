// wwe2k14 Android port (Session 72 part 8; a player asked to add their own drivers from the phone's storage): a Vulkan
// driver the player picked, copied into the app's internal storage (getFilesDir()/drivers/<folder>/; the external
// Android/data folder is mounted noexec, a library there cannot load). It takes the driver packages Android emulators'
// driver managers use (a .zip with the driver's .so and a meta.json naming it: "libraryName", "name", "description",
// "driverVersion", "author") or a bare .so. The library must be a 64-bit ARM shared library. The launcher lists the
// folder by our own wwe2k14_driver.json; the game loads it through libadrenotools (src/android_driver.h,
// --wwe2k14_android_driver_dir). Plain Java with no Android classes, so it can be checked on a PC.
package io.github.jarrodfeng.wwe2k14;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class DriverImport {
    static final String INFO = "wwe2k14_driver.json";
    static final long MAX_BYTES = 256L << 20;  // a driver package is ~10-40 MB

    static final class Result {
        boolean ok;
        String message;  // shown to the player
        String folder;   // the new folder's name (ok only)
    }

    private static Result fail(String message) {
        Result r = new Result();
        r.message = message;
        return r;
    }

    /** Imports the file `displayName` read from `in` into a new folder under `driversDir`. */
    static Result run(InputStream source, String displayName, File driversDir) {
        if (!driversDir.isDirectory() && !driversDir.mkdirs()) return fail("Could not make the drivers folder");
        File staging = new File(driversDir, ".incoming");
        deleteTree(staging);
        if (!staging.mkdirs()) return fail("Could not make the drivers folder");
        try {
            BufferedInputStream in = new BufferedInputStream(source);
            in.mark(8);
            byte[] magic = new byte[4];
            int got = readFully(in, magic);
            in.reset();
            String library, name, description = "", version = "", author = "";
            String file = baseName(displayName);
            if (got == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
                library = safeName(file.toLowerCase(Locale.ROOT).endsWith(".so") ? file : file + ".so");
                copy(in, new File(staging, library), MAX_BYTES);
                name = stem(file);
            } else if (got == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4) {
                String meta = null;
                List<String> libs = new ArrayList<>();
                long total = 0;
                try (ZipInputStream zip = new ZipInputStream(in)) {
                    for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                        if (e.isDirectory()) continue;
                        String n = baseName(e.getName());  // the files only, never a path (no way out of the folder)
                        if (n.isEmpty() || n.startsWith(".") || n.equals(INFO)) continue;
                        n = safeName(n);
                        File out = new File(staging, n);
                        total += copy(zip, out, MAX_BYTES - total);
                        if (n.equals("meta.json")) meta = new String(readAll(out), StandardCharsets.UTF_8);
                        if (n.toLowerCase(Locale.ROOT).endsWith(".so") && !libs.contains(n)) libs.add(n);
                    }
                }
                library = meta != null ? field(meta, "libraryName") : "";
                if (library.isEmpty()) {
                    if (libs.size() != 1)
                        return fail(libs.isEmpty() ? "No driver (.so) in " + file
                                                   : "Several .so files in " + file + " and no meta.json saying which is the driver");
                    library = libs.get(0);
                }
                library = safeName(baseName(library));
                if (!new File(staging, library).isFile()) return fail(file + ": its meta.json names " + library + ", which is not in it");
                name = meta != null ? field(meta, "name") : "";
                if (name.isEmpty()) name = stem(file);
                if (meta != null) {
                    description = field(meta, "description");
                    version = field(meta, "driverVersion");
                    author = field(meta, "author");
                }
            } else {
                return fail(file + " is not a driver: pick a driver .zip or a .so");
            }
            String why = checkLibrary(new File(staging, library));
            if (why != null) return fail(library + " " + why);

            // The folder: the driver's name, made safe, plus a number when it is taken.
            String base = safeName(name.toLowerCase(Locale.ROOT).replace(' ', '_'));
            if (base.length() > 40) base = base.substring(0, 40);
            if (base.isEmpty() || base.startsWith(".")) base = "driver";
            File dest = new File(driversDir, base);
            for (int k = 2; dest.exists(); k++) dest = new File(driversDir, base + "_" + k);
            String info = "{\n  \"name\": " + quote(name) + ",\n  \"library\": " + quote(library) + ",\n  \"description\": "
                    + quote(description) + ",\n  \"version\": " + quote(version) + ",\n  \"author\": " + quote(author)
                    + ",\n  \"source\": " + quote(file) + "\n}\n";
            try (OutputStream o = new FileOutputStream(new File(staging, INFO))) {
                o.write(info.getBytes(StandardCharsets.UTF_8));
            }
            // Libraries read-only (Android 14+ wants code it loads from app storage not writable).
            File[] files = staging.listFiles();
            if (files != null)
                for (File f : files)
                    if (f.getName().toLowerCase(Locale.ROOT).endsWith(".so")) f.setWritable(false, false);
            if (!staging.renameTo(dest)) return fail("Could not store the driver");
            Result r = new Result();
            r.ok = true;
            r.folder = dest.getName();
            r.message = "Driver added and chosen: " + name;
            return r;
        } catch (IOException e) {
            return fail("Could not read " + baseName(displayName) + ": " + e.getMessage());
        } finally {
            deleteTree(staging);  // left only after a failure (renamed away on success)
        }
    }

    /** null when `f` is a 64-bit little-endian ARM shared library (ELF ET_DYN, EM_AARCH64), else why not. */
    static String checkLibrary(File f) throws IOException {
        byte[] h = new byte[20];
        try (InputStream in = new FileInputStream(f)) {
            if (readFully(in, h) < h.length) return "is too short to be a library";
        }
        if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return "is not a library (no ELF header)";
        if (h[4] != 2 || h[5] != 1) return "is not a 64-bit library";
        int type = (h[16] & 0xff) | (h[17] & 0xff) << 8, machine = (h[18] & 0xff) | (h[19] & 0xff) << 8;
        if (machine != 183) return "is not for 64-bit ARM phones";
        if (type != 3) return "is not a shared library";
        return null;
    }

    /** A string field of a flat JSON object ("" when missing). Enough for meta.json; no JSON library on a plain JVM. */
    static String field(String json, String key) {
        Matcher m = Pattern.compile("\"" + Pattern.quote(key) + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(json);
        if (!m.find()) return "";
        String s = m.group(1);
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c != '\\' || i + 1 >= s.length()) { b.append(c); continue; }
            char n = s.charAt(++i);
            if (n == 'n') b.append(' ');
            else if (n == 't') b.append(' ');
            else if (n == 'u' && i + 4 < s.length()) {
                try { b.append((char) Integer.parseInt(s.substring(i + 1, i + 5), 16)); } catch (NumberFormatException e) { b.append('?'); }
                i += 4;
            } else b.append(n);  // \" \\ \/
        }
        return b.toString().trim();
    }

    static String quote(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c < 0x20) b.append(String.format(Locale.ROOT, "\\u%04x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }

    private static String baseName(String path) {
        if (path == null) return "";
        String s = path.replace('\\', '/');
        return s.substring(s.lastIndexOf('/') + 1).trim();
    }

    private static String stem(String file) {
        int dot = file.lastIndexOf('.');
        return dot > 0 ? file.substring(0, dot) : file;
    }

    private static String safeName(String n) {
        return n.replaceAll("[^A-Za-z0-9._+-]", "_");
    }

    private static int readFully(InputStream in, byte[] b) throws IOException {
        int n = 0;
        for (int r; n < b.length && (r = in.read(b, n, b.length - n)) > 0; ) n += r;
        return n;
    }

    private static long copy(InputStream in, File out, long limit) throws IOException {
        long n = 0;
        try (OutputStream o = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            for (int r; (r = in.read(buf)) > 0; ) {
                n += r;
                if (n > limit) throw new IOException("bigger than " + (MAX_BYTES >> 20) + " MB");
                o.write(buf, 0, r);
            }
        }
        return n;
    }

    private static byte[] readAll(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int r; (r = in.read(buf)) > 0; ) b.write(buf, 0, r);
            return b.toByteArray();
        }
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
