// wwe2k14 Android port (Session 71): read-only access to files in the app's own folder for the share sheet ("Share the
// last game log"). A minimal stand-in for androidx's FileProvider (the APK is built without Gradle / androidx):
// content://io.github.jarrodfeng.wwe2k14.files/<path under Android/data/<package>/files>. Not exported; the share intent
// grants the receiving app read access to the files it names, nothing else.
package io.github.jarrodfeng.wwe2k14;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

public class FileShareProvider extends ContentProvider {
    public static final String AUTHORITY = "io.github.jarrodfeng.wwe2k14.files";

    public static Uri uriFor(File base, File f) throws IOException {
        String b = base.getCanonicalPath(), p = f.getCanonicalPath();
        if (!p.startsWith(b + File.separator)) throw new IOException("outside the app folder: " + p);
        return new Uri.Builder().scheme("content").authority(AUTHORITY).encodedPath(Uri.encode(p.substring(b.length()), "/")).build();
    }

    private File fileFor(Uri uri) throws FileNotFoundException {
        File base = getContext().getExternalFilesDir(null);
        if (base == null || uri.getPath() == null) throw new FileNotFoundException("no app folder");
        try {
            File f = new File(base, uri.getPath()).getCanonicalFile();
            if (!f.getPath().startsWith(base.getCanonicalPath() + File.separator) || !f.isFile())
                throw new FileNotFoundException(uri.toString());
            return f;
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read only");
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        try {
            File f = fileFor(uri);
            c.addRow(new Object[] {f.getName(), f.length()});
        } catch (FileNotFoundException ignored) {
        }
        return c;
    }

    // Session 73: the SHARE button sends one report zip.
    @Override public String getType(Uri uri) {
        return uri.getPath() != null && uri.getPath().endsWith(".zip") ? "application/zip" : "text/plain";
    }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
