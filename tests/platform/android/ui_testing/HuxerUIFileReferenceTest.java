package org.huxerui;

import android.app.Instrumentation;
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

final class HuxerUIFileReferenceTest {
    private static final Uri URI = Uri.parse("content://org.huxerui.test/opaque-provider-id");

    static void verify(Instrumentation instrumentation) throws Exception {
        if (Build.VERSION.SDK_INT < 29) {
            android.util.Log.w("HuxerUI", "Skipping file reference provider tests: ContentResolver.wrap requires API 29+");
            return;
        }
        File source = File.createTempFile("huxerui-reference-", ".txt", instrumentation.getContext().getCacheDir());
        File destination = new File(source.getPath() + ".imported");
        try {
            try (FileOutputStream output = new FileOutputStream(source)) {
                output.write("content".getBytes(StandardCharsets.UTF_8));
            }
            Provider provider = new Provider(source);
            provider.attachInfo(instrumentation.getContext(), new ProviderInfo());
            ContentResolver resolver = ContentResolver.wrap(provider);
            Context context = new ContextWrapper(instrumentation.getContext()) {
                @Override public Context getApplicationContext() { return this; }
                @Override public ContentResolver getContentResolver() { return resolver; }
            };
            for (int scenario = 0; scenario < 3; ++scenario) {
                provider.unknown = scenario == 1;
                provider.revoked = false;
                final boolean revoke = scenario == 2;
                try (HuxerUIFileReference first = HuxerUIFileReference.fromUri(context, URI);
                        HuxerUIFileReference second = HuxerUIFileReference.fromUri(context, URI)) {
                    PlatformPayload payload = PlatformPayload.fileReference(first);
                    PlatformPayload.Envelope envelope = PlatformPayload.encodeEnvelope(payload);
                    check(PlatformPayload.decodeEnvelope(envelope.bytes, envelope.externalTextures,
                            envelope.fileReferences, envelope.bufferReferences).equals(payload),
                            "A factory reference must be accepted by the payload bridge");
                    String[] error = new String[1];
                    instrumentation.runOnMainSync(() -> error[0] = verifyNative(payload, () -> {
                        first.close();
                        check(second.uri().equals(URI), "Closing one wrapper must preserve another selection");
                        second.close();
                        provider.revoked = revoke;
                    }, destination.getPath(), provider.unknown, revoke));
                    check("".equals(error[0]), "Native reference regression: " + error[0]);
                }
            }
            provider.revoked = true;
            try {
                HuxerUIFileReference.fromUri(context, URI).close();
                throw new AssertionError("Denied access must fail creation");
            } catch (SecurityException expected) {}
            provider.revoked = false;
            provider.directory = true;
            try {
                HuxerUIFileReference.fromUri(context, URI).close();
                throw new AssertionError("A directory must fail creation");
            } catch (IOException expected) {}
            provider.directory = false;
            try {
                HuxerUIFileReference.fromUri(context, Uri.fromFile(source)).close();
                throw new AssertionError("A local path must not be accepted as a content URI");
            } catch (IllegalArgumentException expected) {}
            check(source.delete(), "The provider source must be removable");
            try {
                HuxerUIFileReference.fromUri(context, URI).close();
                throw new AssertionError("A missing file must fail creation");
            } catch (IOException expected) {}
        } finally {
            source.delete();
            destination.delete();
        }
    }

    private static void check(boolean value, String message) {
        if (!value) { throw new AssertionError(message); }
    }

    private static final class Provider extends ContentProvider {
        final File source;
        volatile boolean unknown;
        volatile boolean revoked;
        boolean directory;

        Provider(File source) { this.source = source; }
        @Override public boolean onCreate() { return true; }
        @Override public String getType(Uri uri) {
            return directory ? DocumentsContract.Document.MIME_TYPE_DIR : unknown ? null : "text/plain";
        }
        @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
            MatrixCursor cursor = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
            cursor.addRow(unknown ? new Object[] {null, null} : new Object[] {"测试.txt", 7L});
            return cursor;
        }
        @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
            if (revoked) { throw new SecurityException("Test provider access revoked"); }
            check("r".equals(mode), "A read-only factory must not request write access");
            return ParcelFileDescriptor.open(source, ParcelFileDescriptor.MODE_READ_ONLY);
        }
        @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
        @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
            throw new UnsupportedOperationException();
        }
        @Override public int delete(Uri uri, String selection, String[] args) {
            throw new UnsupportedOperationException();
        }
    }

    private static native String verifyNative(PlatformPayload payload, Runnable afterCapture,
            String destination, boolean unknown, boolean revoked);
}
