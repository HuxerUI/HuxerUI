package org.huxerui;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class HuxerUIFilePicker {
    private static final int FIRST_REQUEST_CODE = 0x4800;
    private static final int LAST_REQUEST_CODE = 0x7fff;
    private static final AtomicInteger nextRequestCode = new AtomicInteger(FIRST_REQUEST_CODE);

    private final HuxerUIView view;
    private final ContentResolver resolver;
    private volatile Operation active;

    HuxerUIFilePicker(HuxerUIView view) {
        this.view = view;
        Context applicationContext = view.getContext().getApplicationContext();
        resolver = applicationContext.getContentResolver();
    }

    Operation prepareOpen(long nativeHandle, String[] extensions, String[] contentTypes, boolean multiple) {
        return new Operation(nativeHandle, null, null, extensions, contentTypes, multiple);
    }

    Operation prepareSave(
            long nativeHandle, String sourcePath, String suggestedName, String[] extensions, String[] contentTypes) {
        return new Operation(nativeHandle, sourcePath, suggestedName, extensions, contentTypes, false);
    }

    boolean dispatchResult(int requestCode, int resultCode, Intent data) {
        Operation operation = active;
        return operation != null && operation.dispatchResult(requestCode, resultCode, data);
    }

    void cancelActive() {
        Operation operation = active;
        if (operation != null) {
            operation.cancel();
        }
    }

    private void clear(Operation operation) {
        if (active == operation) {
            active = null;
        }
    }

    private static int requestCode() {
        while (true) {
            int current = nextRequestCode.get();
            int following = current == LAST_REQUEST_CODE ? FIRST_REQUEST_CODE : current + 1;
            if (nextRequestCode.compareAndSet(current, following)) {
                return current;
            }
        }
    }

    final class Operation {
        private final long nativeHandle;
        private final String sourcePath;
        private final String suggestedName;
        private final String[] extensions;
        private final String[] contentTypes;
        private final boolean multiple;
        private final int requestCode = requestCode();
        private final AtomicBoolean finished = new AtomicBoolean();
        private final HuxerUIFileReference.CopyState copyState = new HuxerUIFileReference.CopyState();
        private volatile Future<?> worker;

        Operation(long nativeHandle, String sourcePath, String suggestedName, String[] extensions,
                String[] contentTypes, boolean multiple) {
            this.nativeHandle = nativeHandle;
            this.sourcePath = sourcePath;
            this.suggestedName = suggestedName;
            this.extensions = extensions;
            this.contentTypes = contentTypes;
            this.multiple = multiple;
        }

        void start() {
            HuxerUIView.FilePickerLauncher launcher = view.filePickerLauncher();
            if (launcher == null || active != null) {
                complete(false, null);
                return;
            }
            active = this;
            try {
                launcher.launch(sourcePath == null ? openIntent() : saveIntent(), requestCode);
            } catch (RuntimeException exception) {
                complete(false, null);
            }
        }

        void cancel() {
            if (!finished.compareAndSet(false, true)) {
                return;
            }
            clear(this);
            copyState.cancel();
            Future<?> activeWorker = worker;
            if (activeWorker != null) {
                activeWorker.cancel(true);
            }
            HuxerUIView.FilePickerLauncher launcher = view.filePickerLauncher();
            if (launcher != null) {
                try {
                    launcher.cancel(requestCode);
                } catch (RuntimeException ignored) {
                }
            }
            nativeComplete(nativeHandle, false, null, null, null, null, null);
        }

        boolean dispatchResult(int resultRequestCode, int resultCode, Intent data) {
            if (resultRequestCode != requestCode || finished.get()) {
                return resultRequestCode == requestCode;
            }
            if (resultCode != Activity.RESULT_OK || data == null) {
                complete(false, null);
                return true;
            }
            if (sourcePath == null) {
                List<Uri> uris = selectedUris(data, multiple);
                if (uris.isEmpty()) {
                    complete(false, null);
                    return true;
                }
                int grantFlags = data.getFlags();
                try {
                    worker = HuxerUIFileReference.submit(() -> {
                        ReferenceInfo[] selected = null;
                        try {
                            selected = references(uris, grantFlags);
                        } catch (RuntimeException ignored) {
                        }
                        complete(false, selected);
                    });
                } catch (RuntimeException exception) {
                    complete(false, null);
                }
            } else {
                Uri destination = data.getData();
                if (destination == null) {
                    complete(false, null);
                    return true;
                }
                try {
                    worker = HuxerUIFileReference.submit(() -> {
                        boolean saved = false;
                        try {
                            saved = HuxerUIFileReference.copyFileToUri(
                                    resolver, new File(sourcePath), destination, copyState);
                        } catch (Exception ignored) {
                        }
                        complete(saved, null);
                    });
                } catch (RuntimeException exception) {
                    complete(false, null);
                }
            }
            return true;
        }

        private Intent openIntent() {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            configureMimeTypes(intent, resolvedMimeTypes(extensions, contentTypes));
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            return intent;
        }

        private Intent saveIntent() {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType(saveMimeType(extensions, contentTypes, suggestedName));
            String name = suggestedName;
            if (name == null || name.isEmpty()) {
                name = new File(sourcePath).getName();
            }
            if (!name.isEmpty()) {
                intent.putExtra(Intent.EXTRA_TITLE, name);
            }
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            return intent;
        }

        private void complete(boolean saved, ReferenceInfo[] references) {
            if (!finished.compareAndSet(false, true)) {
                return;
            }
            clear(this);
            if (references == null) {
                nativeComplete(nativeHandle, saved, null, null, null, null, null);
                return;
            }
            String[] uris = new String[references.length];
            String[] names = new String[references.length];
            long[] sizes = new long[references.length];
            String[] types = new String[references.length];
            boolean[] writable = new boolean[references.length];
            for (int index = 0; index < references.length; ++index) {
                ReferenceInfo reference = references[index];
                uris[index] = reference.uri.toString();
                names[index] = reference.name;
                sizes[index] = reference.size;
                types[index] = reference.contentType;
                writable[index] = reference.writable;
            }
            nativeComplete(nativeHandle, false, uris, names, sizes, types, writable);
        }
    }

    private ReferenceInfo[] references(List<Uri> uris, int grantFlags) {
        List<ReferenceInfo> references = new ArrayList<>();
        for (Uri uri : uris) {
            ReferenceInfo reference = reference(uri, grantFlags);
            if (reference != null) {
                references.add(reference);
            }
        }
        return references.toArray(new ReferenceInfo[0]);
    }

    private ReferenceInfo reference(Uri uri, int grantFlags) {
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
        if (name == null || name.isEmpty()) {
            name = uri.getLastPathSegment();
        }
        if (name == null || name.isEmpty()) {
            name = "document";
        }
        String contentType = null;
        try {
            contentType = resolver.getType(uri);
        } catch (RuntimeException ignored) {
        }
        if (contentType != null) {
            int parameters = contentType.indexOf(';');
            if (parameters >= 0) {
                contentType = contentType.substring(0, parameters).trim();
            }
            if (contentType.isEmpty() || contentType.indexOf('*') >= 0) {
                contentType = null;
            }
        }
        boolean writable = (grantFlags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0 && supportsWrite(uri);
        return new ReferenceInfo(uri, name, Math.max(-1L, size), contentType, writable);
    }

    private boolean supportsWrite(Uri uri) {
        try (Cursor cursor =
                        resolver.query(uri, new String[] {DocumentsContract.Document.COLUMN_FLAGS}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst()) {
                return true;
            }
            int column = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_FLAGS);
            return column < 0 || cursor.isNull(column)
                    || (cursor.getInt(column) & DocumentsContract.Document.FLAG_SUPPORTS_WRITE) != 0;
        } catch (RuntimeException ignored) {
            return true;
        }
    }

    private static List<Uri> selectedUris(Intent data, boolean multiple) {
        LinkedHashSet<Uri> result = new LinkedHashSet<>();
        ClipData clipData = data.getClipData();
        if (clipData != null) {
            int count = multiple ? clipData.getItemCount() : Math.min(1, clipData.getItemCount());
            for (int index = 0; index < count; ++index) {
                Uri uri = clipData.getItemAt(index).getUri();
                if (uri != null) {
                    result.add(uri);
                }
            }
        }
        if (result.isEmpty() && data.getData() != null) {
            result.add(data.getData());
        }
        return new ArrayList<>(result);
    }

    private static Set<String> resolvedMimeTypes(String[] extensions, String[] contentTypes) {
        LinkedHashSet<String> result = new LinkedHashSet<>();
        for (String contentType : contentTypes) {
            if ("*/*".equals(contentType)) {
                result.clear();
                result.add("*/*");
                return result;
            }
            result.add(contentType);
        }
        MimeTypeMap map = MimeTypeMap.getSingleton();
        for (String extension : extensions) {
            String contentType = map.getMimeTypeFromExtension(extension.toLowerCase(Locale.ROOT));
            if (contentType == null) {
                result.clear();
                result.add("*/*");
                return result;
            }
            result.add(contentType);
        }
        if (result.isEmpty()) {
            result.add("*/*");
        }
        return result;
    }

    private static void configureMimeTypes(Intent intent, Set<String> types) {
        if (types.size() == 1) {
            intent.setType(types.iterator().next());
            return;
        }
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, types.toArray(new String[0]));
    }

    private static String saveMimeType(String[] extensions, String[] contentTypes, String suggestedName) {
        if (suggestedName != null) {
            int separator = suggestedName.lastIndexOf('.');
            if (separator >= 0 && separator + 1 < suggestedName.length()) {
                String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                        suggestedName.substring(separator + 1).toLowerCase(Locale.ROOT));
                if (type != null) {
                    return type;
                }
            }
        }
        Set<String> types = resolvedMimeTypes(extensions, contentTypes);
        for (String type : types) {
            if (!"*/*".equals(type)) {
                return type;
            }
        }
        return "application/octet-stream";
    }

    private static final class ReferenceInfo {
        final Uri uri;
        final String name;
        final long size;
        final String contentType;
        final boolean writable;

        ReferenceInfo(Uri uri, String name, long size, String contentType, boolean writable) {
            this.uri = uri;
            this.name = name;
            this.size = size;
            this.contentType = contentType;
            this.writable = writable;
        }
    }

    private static native void nativeComplete(long nativeHandle, boolean saved, String[] uris, String[] names,
            long[] sizes, String[] contentTypes, boolean[] writable);
}
