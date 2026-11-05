package org.huxerui;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;

final class HuxerUIFilePicker {
    private final HuxerUIActivityResultLauncher launcher;
    private final ContentResolver resolver;
    private volatile Operation active;

    HuxerUIFilePicker(HuxerUIView view, HuxerUIActivityResultLauncher launcher) {
        this.launcher = launcher;
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

    Operation prepareDirectory(long nativeHandle, boolean writable) {
        Operation operation = new Operation(nativeHandle, null, null, new String[0], new String[0], false);
        operation.directory = true;
        operation.writable = writable;
        return operation;
    }

    boolean available() { return launcher.available(); }

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

    final class Operation {
        // This request remains active through metadata loading or export, not just until Activity result.
        // The terminal callback releases the native holder and the shared picker's operation slot.
        private final long nativeHandle;
        private final String sourcePath;
        private final String suggestedName;
        private final String[] extensions;
        private final String[] contentTypes;
        private final boolean multiple;
        private boolean directory;
        private boolean writable;
        private HuxerUIPlatformChannel.Cancellation presentation;
        private boolean canceled;
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
            if (finished.get()) return;
            if (!launcher.available() || active != null) {
                complete(false, null);
                return;
            }
            active = this;
            try {
                presentation = launcher.enqueue(sourcePath == null ? openIntent() : saveIntent(),
                        new HuxerUIActivityResultLauncher.Callback() {
                            @Override public void onResult(int code, Intent data) {
                                presentation = null;
                                acceptResult(code, data);
                            }
                            @Override public void onFailure(RuntimeException error) {
                                presentation = null;
                                complete(false, null);
                            }
                        }, () -> {
                            presentation = null;
                            complete(false, null);
                        });
            } catch (RuntimeException exception) {
                complete(false, null);
            }
        }

        void cancel() {
            if (finished.get() || canceled) return;
            canceled = true;
            copyState.cancel();
            Future<?> activeWorker = worker;
            if (activeWorker != null) activeWorker.cancel(true);
            if (presentation != null) {
                presentation.cancel();
            } else {
                complete(false, null);
            }
        }

        private void acceptResult(int resultCode, Intent data) {
            if (finished.get()) return;
            if (resultCode != Activity.RESULT_OK || data == null) {
                complete(false, null);
                return;
            }
            if (sourcePath == null) {
                List<Uri> uris = selectedUris(data, multiple);
                if (uris.isEmpty()) {
                    complete(false, null);
                    return;
                }
                int grantFlags = data.getFlags();
                // A read-only selection must not inherit broader OS write rights. Derived references
                // carry this effective restriction even if the same tree is later selected writable.
                if (directory && !writable) { grantFlags &= ~Intent.FLAG_GRANT_WRITE_URI_PERMISSION; }
                final int effectiveFlags = grantFlags;
                try {
                    worker = HuxerUIFileReference.submit(() -> {
                        HuxerUIFileReference.Metadata[] selected = null;
                        try {
                            if (directory) {
                                if (uris.size() != 1) { throw new IllegalArgumentException(); }
                                Uri tree = uris.get(0);
                                // Keep the tree grant while addressing its root document. File operations
                                // must use document URIs under that grant, not filesystem paths guessed from IDs.
                                Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
                                HuxerUIFileReference.Metadata item = HuxerUIFileReference.describe(resolver, document, effectiveFlags);
                                if (!DocumentsContract.Document.MIME_TYPE_DIR.equals(item.contentType)
                                        || (writable && !item.writable)) { throw new SecurityException(); }
                                selected = new HuxerUIFileReference.Metadata[] {item};
                            } else {
                                selected = references(uris, effectiveFlags);
                            }
                        } catch (RuntimeException ignored) {
                        }
                        complete(false, selected);
                    });
                } catch (RuntimeException exception) {
                    complete(false, null);
                }
            } else {
                Uri destination = data.getData();
                // Save exports the existing local source into the chosen document. Merely receiving a
                // destination URI is not success; completion waits for streaming output to close.
                if (destination == null) {
                    complete(false, null);
                    return;
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
        }

        private Intent openIntent() {
            if (directory) {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                if (writable) { intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION); }
                return intent;
            }
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

        private void complete(boolean saved, HuxerUIFileReference.Metadata[] references) {
            if (!finished.compareAndSet(false, true)) {
                return;
            }
            clear(this);
            nativeComplete(nativeHandle, saved, references);
        }
    }

    private HuxerUIFileReference.Metadata[] references(List<Uri> uris, int grantFlags) {
        HuxerUIFileReference.Metadata[] references = new HuxerUIFileReference.Metadata[uris.size()];
        for (int index = 0; index < uris.size(); ++index) {
            references[index] = HuxerUIFileReference.describe(resolver, uris.get(index), grantFlags);
        }
        return references;
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

    private static native void nativeComplete(long nativeHandle, boolean saved,
            HuxerUIFileReference.Metadata[] references);
}
