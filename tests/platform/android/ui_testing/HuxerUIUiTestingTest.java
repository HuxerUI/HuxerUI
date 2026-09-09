package org.huxerui;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.res.AssetManager;
import android.os.Bundle;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** Runs the native windowless smoke with resources owned by the test APK. */
public final class HuxerUIUiTestingTest extends Instrumentation {
    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override
    public void onStart() {
        Bundle result = new Bundle();
        result.putString("class", getClass().getName());
        result.putString("test", "testWindowlessRuntime");
        result.putInt("numtests", 1);
        result.putInt("current", 1);
        sendStatus(1, result);
        File packageRoot = null;
        try {
            packageRoot = File.createTempFile("ui-testing-", "", getContext().getCacheDir());
            if (!packageRoot.delete() || !packageRoot.mkdir()) {
                throw new IOException("HuxerUI test package directory cannot be created");
            }
            copyAssets(getContext().getAssets(), "package", packageRoot);
            System.loadLibrary("huxerui_ui_testing_smoke");
            String packagePath = packageRoot.getAbsolutePath();
            runOnMainSync(() -> result.putString("error", runNative(packagePath)));
            String error = result.getString("error");
            boolean passed = error != null && error.isEmpty();
            result.putString("stream", passed ? "HuxerUI UI testing smoke: PASS\n" : "FAIL: " + error);
            if (!passed) result.putString("stack", error == null ? "HuxerUI native result is missing" : error);
            sendStatus(passed ? 0 : -2, result);
            finish(passed ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
        } catch (Throwable error) {
            result.putString("stack", android.util.Log.getStackTraceString(error));
            result.putString("stream", android.util.Log.getStackTraceString(error));
            sendStatus(-2, result);
            finish(Activity.RESULT_CANCELED, result);
        } finally {
            if (packageRoot != null) deletePackage(packageRoot);
        }
    }

    private static void deletePackage(File path) {
        File[] children = path.listFiles();
        if (children != null) {
            for (File child : children) deletePackage(child);
        }
        if (!path.delete()) android.util.Log.w("HuxerUI", "Cannot remove test package entry: " + path);
    }

    private static void copyAssets(AssetManager assets, String source, File destination) throws IOException {
        String[] children = assets.list(source);
        if (children == null) throw new IOException("HuxerUI test assets are unavailable: " + source);
        if (children.length > 0) {
            if (!destination.isDirectory() && !destination.mkdirs()) {
                throw new IOException("HuxerUI test asset directory cannot be created: " + destination);
            }
            for (String child : children) copyAssets(assets, source + "/" + child, new File(destination, child));
        } else {
            try (InputStream input = assets.open(source); FileOutputStream output = new FileOutputStream(destination)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            }
        }
    }

    private static native String runNative(String packagePath);
}
