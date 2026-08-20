package org.huxerui;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.system.ErrnoException;
import android.system.Os;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class HuxerUIFileReference {
    private static final int RESULT_BYTES = 0;
    private static final int RESULT_TRUE = 1;
    private static final int RESULT_FALSE = 2;
    private static final int RESULT_ERROR = 3;
    private static final int RESULT_CANCELED = 4;

    private static final int ERROR_NOT_FOUND = 0;
    private static final int ERROR_PERMISSION_DENIED = 1;
    private static final int ERROR_TOO_LARGE = 2;
    private static final int ERROR_IO = 3;

    private static final int WORKER_COUNT = 2;
    private static final int QUEUE_CAPACITY = 32;
    private static final int BUFFER_SIZE = 16 * 1024;
    private static final int MAX_BYTE_ARRAY_SIZE = Integer.MAX_VALUE - 8;
    private static final ThreadPoolExecutor executor = new ThreadPoolExecutor(
            WORKER_COUNT, WORKER_COUNT, 30L, TimeUnit.SECONDS, new LinkedBlockingQueue<>(QUEUE_CAPACITY));

    static {
        executor.allowCoreThreadTimeOut(true);
    }

    private final ContentResolver resolver;
    private final Uri uri;

    HuxerUIFileReference(Context context, String uri) {
        resolver = context.getApplicationContext().getContentResolver();
        this.uri = Uri.parse(uri);
    }

    Operation prepareRead(long nativeHandle) {
        return new Operation(nativeHandle, Operation.READ, null, false);
    }

    Operation prepareImport(long nativeHandle, String destination, boolean overwrite) {
        return new Operation(nativeHandle, Operation.IMPORT, destination, overwrite);
    }

    Operation prepareReplace(long nativeHandle, String source) {
        return new Operation(nativeHandle, Operation.REPLACE, source, false);
    }

    static Future<?> submit(Runnable operation) {
        return executor.submit(operation);
    }

    static boolean copyFileToUri(ContentResolver resolver, File source, Uri destination, CopyState state)
            throws IOException {
        if (!source.isFile()) {
            return false;
        }
        try (InputStream input = state.trackInput(new FileInputStream(source));
                OutputStream output = state.trackOutput(openOutput(resolver, destination))) {
            copy(input, output, state);
            output.flush();
            return !state.canceled.get();
        } finally {
            state.clearStreams();
        }
    }

    private static OutputStream openOutput(ContentResolver resolver, Uri destination) throws IOException {
        try {
            OutputStream output = resolver.openOutputStream(destination, "rwt");
            if (output != null) {
                return output;
            }
        } catch (FileNotFoundException ignored) {
        }
        OutputStream output = resolver.openOutputStream(destination, "w");
        if (output == null) {
            throw new FileNotFoundException("HuxerUI external file output is unavailable");
        }
        return output;
    }

    private static void copy(InputStream input, OutputStream output, CopyState state) throws IOException {
        byte[] buffer = new byte[BUFFER_SIZE];
        int count;
        while (!state.canceled.get() && (count = input.read(buffer)) != -1) {
            output.write(buffer, 0, count);
        }
        if (state.canceled.get()) {
            throw new IOException("HuxerUI file operation was canceled");
        }
    }

    static final class CopyState {
        final AtomicBoolean canceled = new AtomicBoolean();
        private Closeable input;
        private Closeable output;

        synchronized InputStream trackInput(InputStream stream) {
            input = stream;
            return stream;
        }

        synchronized OutputStream trackOutput(OutputStream stream) {
            output = stream;
            return stream;
        }

        synchronized void cancel() {
            canceled.set(true);
            close(input);
            close(output);
            input = null;
            output = null;
        }

        synchronized void clearStreams() {
            input = null;
            output = null;
        }

        private static void close(Closeable stream) {
            if (stream == null) {
                return;
            }
            try {
                stream.close();
            } catch (IOException ignored) {
            }
        }
    }

    final class Operation implements Runnable {
        static final int READ = 0;
        static final int IMPORT = 1;
        static final int REPLACE = 2;

        private final long nativeHandle;
        private final int kind;
        private final String path;
        private final boolean overwrite;
        private final AtomicBoolean finished = new AtomicBoolean();
        private final CopyState copyState = new CopyState();
        private volatile Future<?> worker;

        Operation(long nativeHandle, int kind, String path, boolean overwrite) {
            this.nativeHandle = nativeHandle;
            this.kind = kind;
            this.path = path;
            this.overwrite = overwrite;
        }

        void start() {
            try {
                worker = submit(this);
            } catch (RuntimeException exception) {
                finishError(ERROR_IO, failureMessage(exception));
            }
        }

        void cancel() {
            if (!finished.compareAndSet(false, true)) {
                return;
            }
            copyState.cancel();
            Future<?> activeWorker = worker;
            if (activeWorker != null) {
                activeWorker.cancel(true);
            }
            nativeComplete(nativeHandle, RESULT_CANCELED, ERROR_IO, null, null);
        }

        @Override
        public void run() {
            try {
                if (kind == READ) {
                    finishBytes(readBytes());
                } else if (kind == IMPORT) {
                    finishBoolean(importTo(new File(path), overwrite));
                } else {
                    finishBoolean(copyFileToUri(resolver, new File(path), uri, copyState));
                }
            } catch (FileNotFoundException exception) {
                finishError(ERROR_NOT_FOUND, failureMessage(exception));
            } catch (SecurityException exception) {
                finishError(ERROR_PERMISSION_DENIED, failureMessage(exception));
            } catch (FileTooLargeException exception) {
                finishError(ERROR_TOO_LARGE, failureMessage(exception));
            } catch (IOException | RuntimeException exception) {
                finishError(ERROR_IO, failureMessage(exception));
            }
        }

        private byte[] readBytes() throws IOException {
            try (InputStream input = copyState.trackInput(resolver.openInputStream(uri));
                    ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input == null) {
                    throw new FileNotFoundException("HuxerUI external file input is unavailable");
                }
                byte[] buffer = new byte[BUFFER_SIZE];
                int count;
                int total = 0;
                while (!copyState.canceled.get() && (count = input.read(buffer)) != -1) {
                    if (count > MAX_BYTE_ARRAY_SIZE - total) {
                        throw new FileTooLargeException();
                    }
                    output.write(buffer, 0, count);
                    total += count;
                }
                if (copyState.canceled.get()) {
                    throw new IOException("HuxerUI file operation was canceled");
                }
                return output.toByteArray();
            } finally {
                copyState.clearStreams();
            }
        }

        private boolean importTo(File destination, boolean overwrite) throws IOException {
            if (destination.isDirectory() || (destination.exists() && !overwrite)) {
                return false;
            }
            File parent = destination.getParentFile();
            if (parent == null || !parent.isDirectory()) {
                return false;
            }
            File temporary = File.createTempFile(".huxerui-", ".tmp", parent);
            try {
                try (InputStream input = copyState.trackInput(resolver.openInputStream(uri));
                        OutputStream output = copyState.trackOutput(new FileOutputStream(temporary))) {
                    if (input == null) {
                        throw new FileNotFoundException("HuxerUI external file input is unavailable");
                    }
                    copy(input, output, copyState);
                    output.flush();
                } finally {
                    copyState.clearStreams();
                }
                try {
                    Os.rename(temporary.getAbsolutePath(), destination.getAbsolutePath());
                    return true;
                } catch (ErrnoException exception) {
                    return false;
                }
            } finally {
                temporary.delete();
            }
        }

        private void finishBytes(byte[] bytes) {
            if (finished.compareAndSet(false, true)) {
                nativeComplete(nativeHandle, RESULT_BYTES, ERROR_IO, bytes, null);
            }
        }

        private void finishBoolean(boolean result) {
            if (finished.compareAndSet(false, true)) {
                nativeComplete(nativeHandle, result ? RESULT_TRUE : RESULT_FALSE, ERROR_IO, null, null);
            }
        }

        private void finishError(int code, String message) {
            if (finished.compareAndSet(false, true)) {
                nativeComplete(nativeHandle, RESULT_ERROR, code, null, message);
            }
        }
    }

    private static String failureMessage(Exception exception) {
        String detail = exception.getLocalizedMessage();
        return detail == null || detail.isEmpty() ? "HuxerUI external file operation failed"
                                                  : "HuxerUI external file operation failed: " + detail;
    }

    private static final class FileTooLargeException extends IOException {
        FileTooLargeException() {
            super("HuxerUI external file is too large to read into memory");
        }
    }

    private static native void nativeComplete(
            long nativeHandle, int result, int errorCode, byte[] bytes, String message);
}
