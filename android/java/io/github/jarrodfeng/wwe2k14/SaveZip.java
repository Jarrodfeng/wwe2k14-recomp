// wwe2k14 Android port (Session 71, ANDROID_PLAN F7; Jarrod 2026-10-28: "App folder + export/import"): the save folder
// to and from a .zip. Plain Java (java.util.zip), so tools/android/touch_preview/SaveZipTest.java checks it on the PC.
// The folder's cache\ (pipeline caches, rebuilt by the game) is left out of exports. An import goes into a fresh folder
// first and replaces the save only when the whole .zip unpacked; the old save stays next to it as <save>.before_import.
package io.github.jarrodfeng.wwe2k14;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class SaveZip {
    private SaveZip() {}

    /** Writes `dir` (without cache/) as a .zip; returns the number of files. */
    public static int export(File dir, OutputStream out) throws IOException {
        int[] count = {0};
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            add(z, dir, "", count);
        }
        return count[0];
    }

    private static void add(ZipOutputStream z, File f, String rel, int[] count) throws IOException {
        if (f.isDirectory()) {
            if (rel.equals("cache/")) return;
            File[] kids = f.listFiles();
            if (kids == null) return;
            for (File k : kids) add(z, k, rel + k.getName() + (k.isDirectory() ? "/" : ""), count);
            return;
        }
        z.putNextEntry(new ZipEntry(rel));
        try (InputStream in = new FileInputStream(f)) {
            copy(in, z);
        }
        z.closeEntry();
        count[0]++;
    }

    /** Replaces `dir` with the .zip's contents; returns the number of files. Throws (leaving `dir` alone) when the
     *  .zip is unreadable, empty or names a path outside the folder. */
    public static int importTo(InputStream in, File dir) throws IOException {
        File parent = dir.getCanonicalFile().getParentFile();
        File tmp = new File(parent, dir.getName() + ".import_tmp");
        deleteTree(tmp);
        if (!tmp.mkdirs()) throw new IOException("cannot make " + tmp);
        String root = tmp.getCanonicalPath() + File.separator;
        int count = 0;
        try (ZipInputStream z = new ZipInputStream(in)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                File out = new File(tmp, e.getName()).getCanonicalFile();
                if (!out.getPath().startsWith(root)) throw new IOException("bad path in the .zip: " + e.getName());
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    copy(z, o);
                }
                count++;
            }
        } catch (IOException ex) {
            deleteTree(tmp);
            throw ex;
        }
        if (count == 0) {
            deleteTree(tmp);
            throw new IOException("the .zip holds no files");
        }
        File backup = new File(parent, dir.getName() + ".before_import");
        deleteTree(backup);
        if (dir.exists() && !dir.renameTo(backup)) { deleteTree(tmp); throw new IOException("cannot move the old save aside"); }
        if (!tmp.renameTo(dir)) {
            backup.renameTo(dir);
            throw new IOException("cannot put the imported save in place");
        }
        // The game's caches stay with the game, not the save: bring the old one back if the import had none.
        File oldCache = new File(backup, "cache"), newCache = new File(dir, "cache");
        if (oldCache.isDirectory() && !newCache.exists()) oldCache.renameTo(newCache);
        return count;
    }

    static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }

    static void deleteTree(File f) {
        if (!f.exists()) return;
        File[] kids = f.isDirectory() ? f.listFiles() : null;
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
