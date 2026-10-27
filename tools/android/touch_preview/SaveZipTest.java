// Session 71: checks android/java/.../SaveZip.java on the PC with a made-up save folder (no game data): export leaves
// cache/ out, import replaces the save and keeps the old one as <save>.before_import, a bad .zip leaves the save alone.
//   java SaveZipTest WORK_DIR   (tools/android/touch_preview.ps1 -SaveZip runs it)
import io.github.jarrodfeng.wwe2k14.SaveZip;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public class SaveZipTest {
    static int failures = 0;
    static void check(String what, boolean ok) {
        System.out.println((ok ? "ok    " : "FAIL  ") + what);
        if (!ok) failures++;
    }
    static void write(File f, String text) throws IOException {
        f.getParentFile().mkdirs();
        try (FileOutputStream o = new FileOutputStream(f)) { o.write(text.getBytes(StandardCharsets.UTF_8)); }
    }
    static String read(File f) throws IOException { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }
    static void wipe(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) wipe(c);
        f.delete();
    }

    public static void main(String[] args) throws Exception {
        File work = new File(args[0]);
        wipe(work);
        File save = new File(work, "wwe2k14");
        write(new File(save, "profile/save.dat"), "old save");
        write(new File(save, "cache/pipelines.bin"), "cache");
        ByteArrayOutputStream zip = new ByteArrayOutputStream();
        int n = SaveZip.export(save, zip);
        check("export: 1 file (cache left out)", n == 1);

        write(new File(save, "profile/save.dat"), "newer save");
        int m = SaveZip.importTo(new ByteArrayInputStream(zip.toByteArray()), save);
        check("import: 1 file", m == 1);
        check("import: the exported save is back", read(new File(save, "profile/save.dat")).equals("old save"));
        check("import: the replaced save is kept", read(new File(work, "wwe2k14.before_import/profile/save.dat")).equals("newer save"));
        check("import: the cache moves back with the game", new File(save, "cache/pipelines.bin").isFile());

        ByteArrayOutputStream bad = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(bad)) {
            z.putNextEntry(new ZipEntry("../escape.txt"));
            z.write(1);
            z.closeEntry();
        }
        boolean threw = false;
        try { SaveZip.importTo(new ByteArrayInputStream(bad.toByteArray()), save); } catch (IOException e) { threw = true; }
        check("a .zip with ../ is refused", threw && !new File(work, "escape.txt").exists());
        check("... and the save is untouched", read(new File(save, "profile/save.dat")).equals("old save"));
        threw = false;
        try { SaveZip.importTo(new ByteArrayInputStream(new byte[] {1, 2, 3}), save); } catch (IOException e) { threw = true; }
        check("a file that is not a .zip is refused", threw && read(new File(save, "profile/save.dat")).equals("old save"));
        System.out.println(failures == 0 ? "all checks passed" : failures + " check(s) failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
