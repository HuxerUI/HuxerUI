package org.huxerui;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;
import org.json.JSONException;
import org.json.JSONObject;

final class HuxerUIFileAccessStore {
    private static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    private static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private static final int REMEMBER = 0;
    private static final int RESTORE = 1;
    private static final int FORGET = 2;

    private HuxerUIFileAccessStore() {}

    static synchronized void run(Context context, int operation, String key, String uri, int modes,
            HuxerUIFileReference.Metadata[] result) throws IOException, JSONException {
        ContentResolver resolver = context.getContentResolver();
        AtomicFile file = new AtomicFile(new File(context.getNoBackupFilesDir(), "huxerui-file-access.json"));
        JSONObject records = read(file);
        JSONObject entries = records.getJSONObject("entries");
        cleanup(file, records, resolver);

        switch (operation) {
            case REMEMBER:
                remember(context, resolver, file, records, key, Uri.parse(uri), modes);
                break;
            case RESTORE:
                JSONObject entry = entries.optJSONObject(key);
                if (entry == null) {
                    if (entries.has(key)) { throw new JSONException("Invalid file access record"); }
                    throw new FileNotFoundException();
                }
                result[0] = restore(resolver, Uri.parse(entry.getString("uri")), entry.getInt("modes"));
                break;
            case FORGET:
                if (entries.remove(key) != null) {
                    write(file, records);
                    cleanup(file, records, resolver);
                }
                break;
            default:
                throw new UnsupportedOperationException();
        }
    }

    private static void remember(Context context, ContentResolver resolver, AtomicFile file, JSONObject records,
            String key, Uri grant, int modes) throws IOException, JSONException {
        Uri uri = documentUri(grant);
        if ((modes & READ) == 0 || !DocumentsContract.isDocumentUri(context, uri)) {
            throw new UnsupportedOperationException();
        }
        validate(resolver, uri);
        String grantKey = grant.toString();
        UriPermission before = persistedPermission(resolver, grant);
        int needed = modes & ~permissionModes(before);
        if (needed != 0) {
            resolver.takePersistableUriPermission(grant, needed);
            UriPermission after = persistedPermission(resolver, grant);
            if ((permissionModes(after) & modes) != modes) {
                throw new SecurityException();
            }
            JSONObject owned = records.getJSONObject("owned");
            // Only a confirmed system grant can be released later. An interrupted save may leave an
            // untracked grant, which cannot safely be distinguished from access held by native code.
            int acquired = confirmedModes(owned.optJSONObject(grantKey), before) | needed;
            owned.put(grantKey, new JSONObject().put("modes", acquired).put("time", after.getPersistedTime()));
        }
        records.getJSONObject("entries").put(key, new JSONObject().put("uri", grantKey).put("modes", modes));
        write(file, records);
        cleanup(file, records, resolver);
    }

    private static HuxerUIFileReference.Metadata restore(ContentResolver resolver, Uri grant, int ceiling)
            throws IOException {
        int modes = permissionModes(persistedPermission(resolver, grant)) & ceiling;
        if ((modes & READ) == 0) {
            throw new SecurityException();
        }
        Uri uri = documentUri(grant);
        validate(resolver, uri);
        return HuxerUIFileReference.describe(resolver, uri, modes | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION, grant);
    }

    private static Uri documentUri(Uri grant) {
        List<String> segments = grant.getPathSegments();
        if (segments.size() == 2 && "tree".equals(segments.get(0))) {
            return DocumentsContract.buildDocumentUriUsingTree(grant, DocumentsContract.getTreeDocumentId(grant));
        }
        return grant;
    }

    private static void validate(ContentResolver resolver, Uri uri) throws IOException {
        String[] columns = {DocumentsContract.Document.COLUMN_FLAGS};
        try (Cursor cursor = resolver.query(uri, columns, null, null, null)) {
            if (cursor == null) { throw new IOException("Document provider returned no cursor"); }
            if (!cursor.moveToFirst()) { throw new FileNotFoundException(); }
            if ((cursor.getInt(0) & DocumentsContract.Document.FLAG_VIRTUAL_DOCUMENT) != 0) {
                throw new UnsupportedOperationException();
            }
        }
    }

    private static UriPermission persistedPermission(ContentResolver resolver, Uri grant) {
        for (UriPermission permission : resolver.getPersistedUriPermissions()) {
            if (grant.equals(permission.getUri())) {
                return permission;
            }
        }
        return null;
    }

    private static int permissionModes(UriPermission permission) {
        return permission == null ? 0
                : (permission.isReadPermission() ? READ : 0) | (permission.isWritePermission() ? WRITE : 0);
    }

    private static int confirmedModes(JSONObject ownership, UriPermission permission) {
        long time = ownership == null ? UriPermission.INVALID_TIME
                : ownership.optLong("time", UriPermission.INVALID_TIME);
        if (time == UriPermission.INVALID_TIME || permission == null || time != permission.getPersistedTime()) {
            return 0;
        }
        return ownership.optInt("modes") & permissionModes(permission);
    }

    private static void cleanup(AtomicFile file, JSONObject records, ContentResolver resolver) {
        try {
            JSONObject owned = records.getJSONObject("owned");
            if (owned.length() == 0) { return; }
            JSONObject entries = records.getJSONObject("entries");
            JSONObject inUse = new JSONObject();
            for (Iterator<String> keys = entries.keys(); keys.hasNext();) {
                JSONObject entry = entries.getJSONObject(keys.next());
                String uri = entry.getString("uri");
                inUse.put(uri, inUse.optInt(uri) | entry.getInt("modes"));
            }
            boolean changed = false;
            for (Iterator<String> uris = owned.keys(); uris.hasNext();) {
                String key = uris.next();
                Uri grant = Uri.parse(key);
                JSONObject ownership = owned.optJSONObject(key);
                // Old records had only permission bits. A released or re-taken grant can have the
                // same URI and flags, so discard ownership when the current grant cannot be matched.
                int confirmed = confirmedModes(ownership, persistedPermission(resolver, grant));
                int keeping = confirmed & inUse.optInt(key);
                int releasing = confirmed & ~keeping;
                if (releasing != 0) {
                    resolver.releasePersistableUriPermission(grant, releasing);
                }
                if (keeping == 0) {
                    uris.remove();
                } else if (keeping != ownership.optInt("modes")) {
                    ownership.put("modes", keeping);
                } else {
                    continue;
                }
                changed = true;
            }
            if (changed) { write(file, records); }
        } catch (IOException | JSONException | RuntimeException ignored) {
            // The durable ownership map allows a later operation to retry an incomplete release.
        }
    }
    private static JSONObject read(AtomicFile file) throws IOException, JSONException {
        try {
            return new JSONObject(new String(file.readFully(), StandardCharsets.UTF_8));
        } catch (FileNotFoundException exception) {
            if (file.getBaseFile().exists()) { throw exception; }
            return new JSONObject().put("entries", new JSONObject()).put("owned", new JSONObject());
        }
    }

    private static void write(AtomicFile file, JSONObject records) throws IOException {
        byte[] bytes = records.toString().getBytes(StandardCharsets.UTF_8);
        FileOutputStream output = file.startWrite();
        try {
            output.write(bytes);
            file.finishWrite(output);
        } catch (IOException | RuntimeException exception) {
            file.failWrite(output);
            throw exception;
        }
        // AtomicFile logs synchronization and rename failures instead of throwing them.
        if (!Arrays.equals(file.readFully(), bytes)) {
            throw new IOException("File access record did not reach storage");
        }
    }

}
