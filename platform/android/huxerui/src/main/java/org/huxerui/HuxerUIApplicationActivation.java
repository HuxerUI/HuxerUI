package org.huxerui;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;

final class HuxerUIApplicationActivation {
    static final int URL = 1;
    static final int FILE = 2;

    final int kind;
    final String value;
    final String name;
    final long size;
    final String contentType;
    final boolean writable;

    private HuxerUIApplicationActivation(
            int kind, String value, String name, long size, String contentType, boolean writable) {
        this.kind = kind;
        this.value = value;
        this.name = name;
        this.size = size;
        this.contentType = contentType;
        this.writable = writable;
    }

    static HuxerUIApplicationActivation fromIntent(Context context, Intent intent) {
        if (intent == null) {
            return null;
        }
        String action = intent.getAction();
        if (!Intent.ACTION_VIEW.equals(action) && !Intent.ACTION_EDIT.equals(action)) {
            return null;
        }
        Uri uri = intent.getData();
        if (uri == null) {
            return null;
        }
        String value = uri.toString();
        if (value.isEmpty()) {
            return null;
        }
        String scheme = uri.getScheme();
        if (!"content".equalsIgnoreCase(scheme) && !"file".equalsIgnoreCase(scheme)) {
            return new HuxerUIApplicationActivation(URL, value, null, -1L, null, false);
        }

        Context applicationContext = context.getApplicationContext();
        ContentResolver resolver = applicationContext.getContentResolver();
        int grantFlags =
                intent.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        HuxerUIFileReference.Metadata metadata = HuxerUIFileReference.describe(resolver, uri, grantFlags);
        String contentType = metadata.contentType;
        if (contentType == null) {
            contentType = HuxerUIFileReference.sanitizeContentType(intent.getType());
        }
        return new HuxerUIApplicationActivation(
                FILE, value, metadata.name, metadata.size, contentType, metadata.writable);
    }
}
