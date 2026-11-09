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
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

final class HuxerUIFileReferenceTest {
    private static final Uri URI = Uri.parse("content://org.huxerui.test/opaque-provider-id");
    private static final Uri ACCESS_CHECK_URI = URI.buildUpon().appendPath("access-check").build();

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
            for (int scenario = 0; scenario < 6; ++scenario) {
                provider.unknown = scenario == 1;
                provider.revoked = false;
                provider.sequential = false;
                provider.reliable = false;
                provider.readError = false;
                final boolean revoke = scenario == 2;
                final boolean sequential = scenario >= 3;
                final boolean reliable = scenario >= 4;
                final boolean readError = scenario == 5;
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
                        provider.sequential = sequential;
                        provider.reliable = reliable;
                        provider.readError = readError;
                    }, destination.getPath(), provider.unknown, revoke, sequential, readError));
                    check("".equals(error[0]), "Native reference regression: " + error[0]);
                }
            }
            String accessError = verifyDescriptorAccess(context, ACCESS_CHECK_URI.toString());
            check("".equals(accessError), "Native descriptor access regression: " + accessError);
            check("content".equals(new String(Files.readAllBytes(source.toPath()), StandardCharsets.UTF_8)),
                    "Rejecting an incompatible descriptor must preserve the file contents");
            for (boolean asynchronous : new boolean[] {false, true}) {
                verifyOutput(instrumentation, context, provider, asynchronous, true, false);
                verifyOutput(instrumentation, context, provider, asynchronous, false, false);
                verifyOutput(instrumentation, context, provider, asynchronous, false, true);
            }
            provider.sequential = false;
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

    private static void verifyOutput(Instrumentation instrumentation, Context context, Provider provider,
            boolean asynchronous, boolean finalize, boolean writeError) throws InterruptedException {
        OutputObserver observer = new OutputObserver(writeError);
        provider.output = observer;
        String[] error = new String[1];
        Runnable verify = () -> error[0] = verifyReliableOutput(context, URI.toString(), asynchronous, finalize,
                writeError);
        if (asynchronous) { instrumentation.runOnMainSync(verify); }
        else { verify.run(); }
        check("".equals(error[0]), "Native reliable output regression: " + error[0]);
        check(observer.closed.await(5, TimeUnit.SECONDS), "Provider must receive a close status");
        check(!(observer.error instanceof ParcelFileDescriptor.FileDescriptorDetachedException),
                "A reliable descriptor must preserve its provider's close channel");
        check((observer.error == null) == finalize, "Provider must distinguish finalized and incomplete writes");
        provider.output = null;
    }

    private static final class OutputObserver {
        final boolean writeError;
        final CountDownLatch closed = new CountDownLatch(1);
        IOException error;

        OutputObserver(boolean writeError) { this.writeError = writeError; }

        void onClose(IOException failure) {
            error = failure;
            closed.countDown();
        }
    }

    private static final class Provider extends ContentProvider {
        final File source;
        volatile boolean unknown;
        volatile boolean revoked;
        volatile boolean sequential;
        volatile boolean reliable;
        volatile boolean readError;
        volatile OutputObserver output;
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
            if (ACCESS_CHECK_URI.equals(uri)) {
                try {
                    // Linux access mode 3 checks permissions but returns an fd that cannot read or write.
                    int access = "r".equals(mode) ? OsConstants.O_RDWR : 3;
                    FileDescriptor descriptor = Os.open(source.getPath(), access, 0);
                    try {
                        return ParcelFileDescriptor.dup(descriptor);
                    } finally {
                        Os.close(descriptor);
                    }
                } catch (ErrnoException | IOException exception) {
                    FileNotFoundException failure = new FileNotFoundException("Test descriptor unavailable");
                    failure.initCause(exception);
                    throw failure;
                }
            }
            OutputObserver observer = output;
            if (observer != null && mode.contains("w")) {
                try {
                    int flags = observer.writeError ? ParcelFileDescriptor.MODE_READ_ONLY
                                                    : ParcelFileDescriptor.parseMode(mode);
                    return ParcelFileDescriptor.open(source, flags, new Handler(Looper.getMainLooper()),
                            observer::onClose);
                } catch (IOException exception) {
                    FileNotFoundException failure = new FileNotFoundException("Test provider output unavailable");
                    failure.initCause(exception);
                    throw failure;
                }
            }
            check("r".equals(mode), "A read-only factory must not request write access");
            if (sequential) {
                try {
                    ParcelFileDescriptor[] pipe = reliable ? ParcelFileDescriptor.createReliablePipe()
                                                           : ParcelFileDescriptor.createPipe();
                    try (ParcelFileDescriptor.AutoCloseOutputStream output =
                            new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1])) {
                        output.write("content".getBytes(StandardCharsets.UTF_8));
                        if (readError) { pipe[1].closeWithError("Test provider read failure"); }
                        return pipe[0];
                    } catch (IOException exception) {
                        pipe[0].close();
                        throw exception;
                    }
                } catch (IOException exception) {
                    FileNotFoundException failure = new FileNotFoundException("Test provider pipe unavailable");
                    failure.initCause(exception);
                    throw failure;
                }
            }
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
            String destination, boolean unknown, boolean revoked, boolean sequential, boolean readError);
    private static native String verifyDescriptorAccess(Context context, String uri);
    private static native String verifyReliableOutput(Context context, String uri, boolean asynchronous,
            boolean finalize, boolean writeError);
}
