package org.huxerui;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.content.pm.ProviderInfo;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import android.system.OsConstants;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
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

/**
 * A retained Android document capability created from a URI or received through {@link PlatformPayload}.
 *
 * <p>{@link #uri()} exposes the provider URI for APIs that accept Android document resources. The C++
 * {@code FileReference} remains retained until this object is closed; repeated calls to {@link #close()} are safe.</p>
 */
public final class HuxerUIFileReference implements AutoCloseable {
    private static final int RESULT_BYTES = 0;
    private static final int RESULT_TRUE = 1;
    private static final int RESULT_FALSE = 2;
    private static final int RESULT_ERROR = 3;
    private static final int RESULT_CANCELED = 4;
    private static final int RESULT_DIRECTORY = 5;

    private static final int ERROR_NOT_FOUND = 0;
    private static final int ERROR_PERMISSION_DENIED = 1;
    private static final int ERROR_TOO_LARGE = 2;
    private static final int ERROR_IO = 3;
    private static final int ERROR_NOT_DIRECTORY = 4;
    private static final int ERROR_IS_DIRECTORY = 5;
    private static final int ERROR_UNSUPPORTED = 6;
    private static final int ERROR_ALREADY_EXISTS = 7;

    private static final int WORKER_COUNT = 2;
    private static final int QUEUE_CAPACITY = 32;
    private static final int BUFFER_SIZE = 16 * 1024;
    private static final int MAX_BYTE_ARRAY_SIZE = Integer.MAX_VALUE - 8;
    private static final ThreadPoolExecutor executor = new ThreadPoolExecutor(
            WORKER_COUNT, WORKER_COUNT, 30L, TimeUnit.SECONDS, new LinkedBlockingQueue<>(QUEUE_CAPACITY));

    static {
        executor.allowCoreThreadTimeOut(true);
    }

    private final Context context;
    private final boolean writeAllowed;
    private final ContentResolver resolver;
    private final Uri uri;
    private final long capabilityKey;
    private long nativeHandle;

    /**
     * Creates a read-only file capability using the caller's existing access to a content URI.
     *
     * <p>This synchronous operation queries metadata and opens a read descriptor to check access; call it off the UI
     * thread when the provider may block. Unknown metadata remains unknown. The SDK must already be loaded, but no
     * window is required. Copies and pending operations retain their own access state after this wrapper closes.</p>
     *
     * <p>This method neither takes nor releases persistent URI grants. The caller owns that policy, and Android may
     * revoke access later. A retained reference does not promise access across application restarts.</p>
     *
     * @param context a Context whose application Context provides the ContentResolver
     * @param uri the readable content URI; retaining its text alone does not retain an access grant
     * @return a payload-compatible wrapper; close it when this Java owner no longer needs the capability
     * @throws IllegalArgumentException if context or uri is null, or uri is not a content URI
     * @throws SecurityException if the provider denies access
     * @throws IOException if the URI is not a readable file or its capability cannot be created
     */
    public static HuxerUIFileReference fromUri(Context context, Uri uri) throws IOException {
        if (context == null || uri == null || !"content".equals(uri.getScheme())) {
            throw new IllegalArgumentException("HuxerUI file reference requires a Context and content URI");
        }
        Context applicationContext = context.getApplicationContext();
        if (applicationContext == null) {
            throw new IllegalArgumentException("HuxerUI file reference requires an application Context");
        }
        ContentResolver resolver = applicationContext.getContentResolver();
        Metadata metadata = describe(resolver, uri, 0);
        if (DocumentsContract.Document.MIME_TYPE_DIR.equals(metadata.contentType)) {
            throw new IOException("HuxerUI file reference requires a file, not a directory");
        }
        try (ParcelFileDescriptor descriptor = resolver.openFileDescriptor(uri, "r")) {
            if (descriptor == null) {
                throw new IOException("HuxerUI external file descriptor is unavailable");
            }
            if (OsConstants.S_ISDIR(Os.fstat(descriptor.getFileDescriptor()).st_mode)) {
                throw new IOException("HuxerUI file reference requires a file, not a directory");
            }
        } catch (ErrnoException exception) {
            throw new IOException("HuxerUI external file descriptor could not be inspected", exception);
        } catch (SecurityException exception) {
            throw exception;
        } catch (RuntimeException exception) {
            throw new IOException("HuxerUI external file could not be opened", exception);
        }
        return create(applicationContext, uri.toString(), metadata.name, metadata.size, metadata.contentType);
    }

    // Provider-backed instances service C++ file operations; payload wrappers instead retain a native capability.
    HuxerUIFileReference(Context context, String uri, boolean writeAllowed) {
        this.context = context.getApplicationContext();
        this.writeAllowed = writeAllowed;
        resolver = this.context.getContentResolver();
        this.uri = Uri.parse(uri);
        capabilityKey = 0L;
        nativeHandle = 0L;
    }

    HuxerUIFileReference(long nativeHandle, long capabilityKey, String uri) {
        if (nativeHandle == 0L || capabilityKey == 0L) {
            throw new IllegalArgumentException("HuxerUI file reference handles must not be zero");
        }
        context = null;
        writeAllowed = false;
        resolver = null;
        this.uri = Uri.parse(uri);
        this.capabilityKey = capabilityKey;
        this.nativeHandle = nativeHandle;
    }

    /** Returns the Android document URI while retaining the underlying access capability. */
    public synchronized Uri uri() {
        requireOpen();
        return uri;
    }

    synchronized long capabilityKey() {
        if (capabilityKey == 0L) {
            throw new IllegalStateException("HuxerUI file reference is not a platform payload capability");
        }
        return capabilityKey;
    }

    synchronized long retainHandle() {
        requireOpen();
        return retain(nativeHandle);
    }

    /** Releases this Java reference to the shared file capability. */
    @Override
    public synchronized void close() {
        if (nativeHandle != 0L) {
            release(nativeHandle);
            nativeHandle = 0L;
        }
    }

    @Override
    protected void finalize() throws Throwable {
        try {
            close();
        } finally {
            super.finalize();
        }
    }

    synchronized void requireOpen() {
        if (nativeHandle == 0L) {
            throw new IllegalStateException("HuxerUI file reference is closed or is not a platform payload capability");
        }
    }

    private static native void release(long handle);

    private static native long retain(long handle);

    private static native HuxerUIFileReference create(
            Context context, String uri, String name, long size, String contentType) throws IOException;

    String entryKey() {
        try { return uri.getAuthority() + ":" + DocumentsContract.getDocumentId(uri); }
        catch (IllegalArgumentException exception) { return uri.toString(); }
    }

    Operation prepareDirectory(long nativeHandle, int kind, String path, String sourceUri, String name,
            String existingUri, boolean overwrite) {
        Operation operation = new Operation(nativeHandle, kind, path, overwrite);
        operation.sourceUri = sourceUri == null ? null : Uri.parse(sourceUri);
        operation.name = name;
        operation.existingUri = existingUri == null ? null : Uri.parse(existingUri);
        return operation;
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

    Operation prepareOpenRead(long nativeHandle) {
        return new Operation(nativeHandle, Operation.OPEN_READ, null, false);
    }

    Operation prepareOpenWrite(long nativeHandle) {
        return new Operation(nativeHandle, Operation.OPEN_WRITE, null, false);
    }

    Operation prepareOpenRandomAccess(long nativeHandle, boolean writing) {
        int kind = writing ? Operation.OPEN_RANDOM_WRITE : Operation.OPEN_RANDOM_READ;
        return new Operation(nativeHandle, kind, null, false);
    }

    static Future<?> submit(Runnable operation) {
        return executor.submit(operation);
    }

    static Metadata describe(ContentResolver resolver, Uri uri, int grantFlags) {
        // Snapshot display metadata without reading contents. Name/size, MIME, and capability flags
        // are separate provider calls; advertised writability remains subject to the effective grant.
        String name = null;
        long size = -1L;
        try (Cursor cursor = resolver.query(
                     uri, new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int nameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                int sizeColumn = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (nameColumn >= 0 && !cursor.isNull(nameColumn)) {
                    name = cursor.getString(nameColumn);
                }
                if (sizeColumn >= 0 && !cursor.isNull(sizeColumn)) {
                    size = cursor.getLong(sizeColumn);
                }
            }
        } catch (RuntimeException ignored) {
        }
        if (name == null) { name = ""; }
        String contentType = null;
        try {
            contentType = resolver.getType(uri);
        } catch (RuntimeException ignored) {
        }
        contentType = sanitizeContentType(contentType);
        boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(contentType);
        boolean writeAllowed = (grantFlags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0;
        boolean writable = writeAllowed && supportsWrite(resolver, uri, directory);
        if (directory) { size = -1; }
        return new Metadata(uri, name, Math.max(-1L, size), contentType, writable, writeAllowed);
    }

    static String sanitizeContentType(String contentType) {
        if (contentType != null) {
            int parameters = contentType.indexOf(';');
            if (parameters >= 0) {
                contentType = contentType.substring(0, parameters).trim();
            }
            if (contentType.isEmpty() || contentType.indexOf('*') >= 0) {
                contentType = null;
            }
        }
        return contentType;
    }

    private static boolean supportsWrite(ContentResolver resolver, Uri uri, boolean directory) {
        try (Cursor cursor =
                        resolver.query(uri, new String[] {DocumentsContract.Document.COLUMN_FLAGS}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst()) {
                return !directory;
            }
            int column = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_FLAGS);
            if (column < 0 || cursor.isNull(column)) { return !directory; }
            int flag = directory ? DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE : DocumentsContract.Document.FLAG_SUPPORTS_WRITE;
            return (cursor.getInt(column) & flag) != 0;
        } catch (RuntimeException ignored) {
            return !directory;
        }
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
        OutputStream output = resolver.openOutputStream(destination, "wt");
        if (output == null) {
            throw new FileNotFoundException("HuxerUI external file output is unavailable");
        }
        return output;
    }

    private static long copy(InputStream input, OutputStream output, CopyState state) throws IOException {
        // Imports, write-back, and directory copies share this bounded Java-side transfer. Only the
        // final byte count crosses JNI; public ReadBytes instead materializes the entire payload.
        byte[] buffer = new byte[BUFFER_SIZE];
        long bytes = 0;
        int count;
        while (!state.canceled.get() && (count = input.read(buffer)) != -1) {
            output.write(buffer, 0, count);
            bytes += count;
        }
        if (state.canceled.get()) {
            throw new IOException("HuxerUI file operation was canceled");
        }
        return bytes;
    }

    static final class CopyState {
        // Closing tracked streams complements Future.cancel(): provider I/O may ignore thread
        // interruption. This is best effort and does not roll back bytes already accepted by a provider.
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

    static final class Metadata {
        final Uri uri;
        final String name;
        final long size;
        final String contentType;
        final boolean writable;
        // The tree grant is independent of this item's advertised capability. A directory that cannot
        // create children can still contain existing writable files; read-only grants restrict both.
        final boolean writeAllowed;

        Metadata(Uri uri, String name, long size, String contentType, boolean writable, boolean writeAllowed) {
            this.uri = uri;
            this.name = name;
            this.size = size;
            this.contentType = contentType;
            this.writable = writable;
            this.writeAllowed = writeAllowed;
        }
    }

    final class Operation implements Runnable {
        static final int READ = 0;
        static final int IMPORT = 1;
        static final int REPLACE = 2;
        static final int LIST_CHILDREN = 3;
        static final int FIND_CHILD = 4;
        static final int CREATE_DIRECTORY = 5;
        static final int COPY_FILE = 6;
        static final int CHECK_DESTINATION = 7;
        static final int OPEN_READ = 8;
        static final int OPEN_WRITE = 9;
        static final int OPEN_RANDOM_READ = 10;
        static final int OPEN_RANDOM_WRITE = 11;
        static final int CREATE_DIRECTORY_BY_NAME = 12;

        private final long nativeHandle;
        private final int kind;
        private final String path;
        private final boolean overwrite;
        private Uri sourceUri;
        private Uri existingUri;
        private String name;
        private long transferred;
        // Cancellation and worker completion share this gate because nativeComplete consumes a native
        // callback holder exactly once. A late worker must not call it again after cancellation.
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
            nativeComplete(nativeHandle, RESULT_CANCELED, ERROR_IO, null, null, null, 0, false, null);
        }

        @Override
        public void run() {
            // Synchronous JNI callers enter here directly and receive completion before this method returns.
            try {
                if (kind == READ) {
                    finishBytes(readBytes());
                } else if (kind == IMPORT) {
                    finishBoolean(importTo(new File(path), overwrite));
                } else if (kind == REPLACE) {
                    if (!writeAllowed) { throw new SecurityException(); }
                    finishBoolean(copyFileToUri(resolver, new File(path), uri, copyState));
                } else if (kind == OPEN_READ || kind == OPEN_WRITE ||
                        kind == OPEN_RANDOM_READ || kind == OPEN_RANDOM_WRITE) {
                    boolean writing = kind == OPEN_WRITE || kind == OPEN_RANDOM_WRITE;
                    boolean randomAccess = kind == OPEN_RANDOM_READ || kind == OPEN_RANDOM_WRITE;
                    if (writing && !writeAllowed) { throw new SecurityException(); }
                    openStream(writing, randomAccess);
                } else {
                    directoryOperation();
                }
            } catch (OperationFailure exception) {
                finishError(exception.code, "HuxerUI external directory operation failed");
            } catch (FileNotFoundException exception) {
                finishError(ERROR_NOT_FOUND, failureMessage(exception));
            } catch (SecurityException exception) {
                finishError(ERROR_PERMISSION_DENIED, failureMessage(exception));
            } catch (FileTooLargeException exception) {
                finishError(ERROR_TOO_LARGE, failureMessage(exception));
            } catch (UnsupportedOperationException exception) {
                finishError(ERROR_UNSUPPORTED, "HuxerUI provider does not support this operation");
            } catch (IOException | RuntimeException exception) {
                finishError(ERROR_IO, failureMessage(exception));
            }
        }

        private Metadata metadata(Uri item) throws IOException {
            Metadata result = describe(resolver, item,
                    writeAllowed ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : 0);
            if (result.name == null || result.name.isEmpty()) { throw new OperationFailure(ERROR_UNSUPPORTED); }
            return result;
        }

        private boolean isDirectory(Metadata item) {
            return DocumentsContract.Document.MIME_TYPE_DIR.equals(item.contentType);
        }

        private List<Metadata> children() throws IOException {
            // Use the tree URI for every child and obtain metadata from the enumeration cursor itself.
            // A loading or virtual-document result is not a complete ordinary-file listing; fail rather
            // than silently omitting entries from a directory copy. Hidden entries are not filtered.
            Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(uri, DocumentsContract.getDocumentId(uri));
            List<Metadata> result = new ArrayList<>();
            String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, OpenableColumns.DISPLAY_NAME,
                    OpenableColumns.SIZE, DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_FLAGS};
            try (Cursor cursor = resolver.query(childrenUri, columns, null, null, null)) {
                if (cursor == null) { throw new IOException(); }
                if (cursor.getExtras().getBoolean(DocumentsContract.EXTRA_LOADING, false)) { throw new OperationFailure(ERROR_UNSUPPORTED); }
                while (cursor.moveToNext()) {
                    if (copyState.canceled.get()) { throw new IOException(); }
                    String id = cursor.getString(0);
                    String childName = cursor.getString(1);
                    String type = cursor.getString(3);
                    if (id == null || childName == null || type == null) { throw new OperationFailure(ERROR_UNSUPPORTED); }
                    boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(type);
                    int flags = cursor.isNull(4) ? 0 : cursor.getInt(4);
                    if (Build.VERSION.SDK_INT >= 24 && (flags & DocumentsContract.Document.FLAG_VIRTUAL_DOCUMENT) != 0) {
                        throw new OperationFailure(ERROR_UNSUPPORTED);
                    }
                    int required = directory ? DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE
                                             : DocumentsContract.Document.FLAG_SUPPORTS_WRITE;
                    result.add(new Metadata(DocumentsContract.buildDocumentUriUsingTree(uri, id), childName,
                            directory || cursor.isNull(2) ? -1 : cursor.getLong(2), type,
                            writeAllowed && (flags & required) != 0, writeAllowed));
                }
            }
            return result;
        }

        private Metadata find(String childName) throws IOException {
            // Document IDs are not filenames. Name lookup scans the listing and rejects duplicate
            // display names instead of choosing an arbitrary document; writes reuse this result's URI.
            Metadata found = null;
            for (Metadata item : children()) {
                if (item.name.equals(childName)) {
                    if (found != null) { throw new OperationFailure(ERROR_ALREADY_EXISTS); }
                    found = item;
                }
            }
            return found;
        }

        private void checkName() throws IOException {
            if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                    || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf(0) >= 0) {
                throw new OperationFailure(ERROR_UNSUPPORTED);
            }
        }

        private boolean independent() throws IOException {
            // Cross-provider URI prefixes cannot prove containment. Accept the known separation of a
            // different-UID provider and app-private storage, or ask one provider about both tree roots.
            // Unknown relationships fail before copying, including tree checks below API 29.
            if (path != null) {
                File target = new File(path).getCanonicalFile();
                ProviderInfo provider = context.getPackageManager().resolveContentProvider(uri.getAuthority(), 0);
                if (provider == null || provider.applicationInfo.uid == android.os.Process.myUid()) {
                    throw new OperationFailure(ERROR_UNSUPPORTED);
                }
                File data = new File(context.getApplicationInfo().dataDir).getCanonicalFile();
                for (File ancestor = target; ancestor != null; ancestor = ancestor.getParentFile()) {
                    if (ancestor.equals(data)) { return true; }
                }
                throw new OperationFailure(ERROR_UNSUPPORTED);
            }
            if (sourceUri == null || !Objects.equals(uri.getAuthority(), sourceUri.getAuthority())
                    || Build.VERSION.SDK_INT < 29) { throw new OperationFailure(ERROR_UNSUPPORTED); }
            if (DocumentsContract.getDocumentId(uri).equals(DocumentsContract.getDocumentId(sourceUri))) { return false; }
            return !DocumentsContract.isChildDocument(resolver, uri, sourceUri)
                    && !DocumentsContract.isChildDocument(resolver, sourceUri, uri);
        }

        private void directoryOperation() throws IOException {
            Metadata parent = metadata(uri);
            if (!isDirectory(parent)) { throw new OperationFailure(ERROR_NOT_DIRECTORY); }
            if (kind == CHECK_DESTINATION) { finishBoolean(independent()); return; }
            if (kind == LIST_CHILDREN) { finishDirectory(children().toArray(new Metadata[0]), 0, false); return; }
            checkName();
            if (kind == FIND_CHILD) {
                Metadata existing = find(name);
                finishDirectory(existing == null ? new Metadata[0] : new Metadata[] {existing}, 0, false);
                return;
            }
            if (!parent.writable) { throw new SecurityException(); }
            boolean creatingDirectory = kind == CREATE_DIRECTORY || kind == CREATE_DIRECTORY_BY_NAME;
            Uri targetUri = existingUri;
            if (kind == CREATE_DIRECTORY_BY_NAME) {
                Metadata found = find(name);
                if (found != null) { targetUri = found.uri; }
            }
            // Revalidate the looked-up document directly instead of scanning all siblings again.
            // This does not reserve an absent name: createDocument may normalize or suffix it, so the
            // actual returned name is checked below and only a newly created wrong-name item is removed.
            Metadata existing = targetUri == null ? null : metadata(targetUri);
            if (existing != null && !name.equals(existing.name)) { throw new OperationFailure(ERROR_UNSUPPORTED); }
            if (existing != null && (isDirectory(existing) != creatingDirectory || (kind == COPY_FILE && !overwrite))) {
                throw new OperationFailure(ERROR_ALREADY_EXISTS);
            }
            if (copyState.canceled.get()) { throw new IOException(); }
            String type = creatingDirectory ? DocumentsContract.Document.MIME_TYPE_DIR : "application/octet-stream";
            if (kind == COPY_FILE && sourceUri != null) {
                String sourceType = resolver.getType(sourceUri);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(sourceType)) { throw new OperationFailure(ERROR_IS_DIRECTORY); }
                if (sourceType != null) { type = sourceType; }
                if (existing != null && Objects.equals(sourceUri.getAuthority(), existing.uri.getAuthority())
                        && DocumentsContract.getDocumentId(sourceUri).equals(DocumentsContract.getDocumentId(existing.uri))) {
                    throw new OperationFailure(ERROR_UNSUPPORTED);
                }
            }
            if (kind == COPY_FILE && sourceUri == null) {
                try {
                    if (!OsConstants.S_ISREG(Os.lstat(path).st_mode)) { throw new OperationFailure(ERROR_UNSUPPORTED); }
                } catch (ErrnoException exception) { throw new IOException(exception); }
            }
            Uri outputUri = existing == null ? DocumentsContract.createDocument(resolver, uri, type, name) : existing.uri;
            if (outputUri == null) { throw new IOException(); }
            Metadata outputMetadata = existing == null ? metadata(outputUri) : existing;
            if (!name.equals(outputMetadata.name)) {
                if (existing == null) { DocumentsContract.deleteDocument(resolver, outputUri); }
                throw new OperationFailure(ERROR_UNSUPPORTED);
            }
            long bytes = 0;
            if (kind == COPY_FILE) {
                if (!outputMetadata.writable) { throw new SecurityException(); }
                try (InputStream input = copyState.trackInput(sourceUri == null
                             ? new FileInputStream(path) : resolver.openInputStream(sourceUri));
                        OutputStream output = copyState.trackOutput(openOutput(resolver, outputUri))) {
                    if (input == null || output == null) { throw new FileNotFoundException(); }
                    bytes = copy(input, output, copyState);
                    output.flush();
                } finally { copyState.clearStreams(); }
                outputMetadata = metadata(outputUri);
            }
            finishDirectory(new Metadata[] {outputMetadata}, bytes, existing == null);
        }

        private void finishDirectory(Metadata[] references, long bytes, boolean created) {
            if (finished.compareAndSet(false, true)) {
                nativeComplete(nativeHandle, RESULT_DIRECTORY, ERROR_IO, null, null, references, bytes, created, null);
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

        private void openStream(boolean writing, boolean randomAccess) throws IOException {
            ParcelFileDescriptor descriptor;
            if (writing && randomAccess) {
                // Native code validates seekability and access before applying any requested truncation.
                descriptor = resolver.openFileDescriptor(uri, "rw");
            } else if (writing) {
                try {
                    descriptor = resolver.openFileDescriptor(uri, "rwt");
                } catch (FileNotFoundException ignored) {
                    descriptor = resolver.openFileDescriptor(uri, "wt");
                }
            } else {
                descriptor = resolver.openFileDescriptor(uri, "r");
            }
            if (descriptor == null) {
                throw new FileNotFoundException("HuxerUI external file descriptor is unavailable");
            }
            boolean retained = false;
            boolean reliable = false;
            try {
                reliable = descriptor.canDetectErrors();
                if (reliable) {
                    if (randomAccess) { throw new UnsupportedOperationException(); }
                    // Keep the Java owner alive so the provider can exchange close and error status.
                    retained = finishDescriptor(descriptor.getFd(), descriptor);
                } else {
                    int fd = descriptor.detachFd();
                    try {
                        retained = finishDescriptor(fd, null);
                    } finally {
                        if (!retained) { ParcelFileDescriptor.adoptFd(fd).close(); }
                    }
                }
            } finally {
                if (!retained) {
                    if (reliable && writing && !randomAccess) {
                        descriptor.closeWithError("HuxerUI external file output was not finalized");
                    } else {
                        descriptor.close();
                    }
                }
            }
        }

        private boolean importTo(File destination, boolean overwrite) throws IOException {
            // Stage beside the local destination so rename happens only after the streams close.
            // The existence check is not an atomic no-overwrite reservation against external writers.
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
                    transferred = copy(input, output, copyState);
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
                nativeComplete(nativeHandle, RESULT_BYTES, ERROR_IO, bytes, null, null, 0, false, null);
            }
        }

        private void finishBoolean(boolean result) {
            if (finished.compareAndSet(false, true)) {
                nativeComplete(nativeHandle, result ? RESULT_TRUE : RESULT_FALSE, ERROR_IO,
                        null, null, null, transferred, false, null);
            }
        }

        private boolean finishDescriptor(int fd, ParcelFileDescriptor descriptor) {
            return finished.compareAndSet(false, true)
                    && nativeComplete(nativeHandle, RESULT_TRUE, ERROR_IO, null, null, null, fd, false, descriptor);
        }

        private void finishError(int code, String message) {
            if (finished.compareAndSet(false, true)) {
                nativeComplete(nativeHandle, RESULT_ERROR, code, null, message, null, 0, false, null);
            }
        }
    }

    private static String failureMessage(Exception exception) {
        String detail = exception.getLocalizedMessage();
        return detail == null || detail.isEmpty() ? "HuxerUI external file operation failed"
                                                  : "HuxerUI external file operation failed: " + detail;
    }

    private static final class OperationFailure extends IOException {
        final int code;
        OperationFailure(int code) { this.code = code; }
    }

    private static final class FileTooLargeException extends IOException {
        FileTooLargeException() {
            super("HuxerUI external file is too large to read into memory");
        }
    }

    private static native boolean nativeComplete(long nativeHandle, int result, int errorCode, byte[] bytes,
                                                 String message, Metadata[] references, long transferred,
                                                 boolean created, ParcelFileDescriptor descriptor);
}
