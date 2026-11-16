package @PROJECT_ID@;

import android.app.Application;
import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

public final class MainApplication extends Application {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        if (BuildConfig.HUXERUI_WATCH) {
            File directory = new File(base.getFilesDir(), "huxerui-watch");
            File selection = new File(directory, "current");
            String revision = null;
            String apk = null;
            if (selection.isFile()) {
                try (BufferedReader reader = new BufferedReader(new FileReader(selection))) {
                    revision = reader.readLine();
                    apk = reader.readLine();
                } catch (IOException exception) {
                    Log.w("HuxerUI", "Could not read the selected development revision", exception);
                }
            }
            if (revision != null && revision.matches("[0-9]+") && base.getApplicationInfo().sourceDir.equals(apk)) {
                File version = new File(directory, revision);
                File library = new File(version, "app.so");
                File resources = new File(version, "resources");
                try {
                    if (!library.isFile() || library.canWrite() ||
                            !new File(resources, "huxerui/resources.bin").isFile()) {
                        throw new IOException("incomplete staged application");
                    }
                    System.load(library.getAbsolutePath());
                    System.setProperty("org.huxerui.watch.revision", revision);
                    return;
                } catch (IOException | UnsatisfiedLinkError exception) {
                    Log.w("HuxerUI", "Could not load the staged application", exception);
                }
            }
        }
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
