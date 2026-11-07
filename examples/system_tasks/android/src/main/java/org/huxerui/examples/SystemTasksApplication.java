package org.huxerui.examples;

import android.app.Application;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.Build;

/** Loads the app declaration for JobService cold activation and prepares platform task presentation. */
public final class SystemTasksApplication extends Application {
    static {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        if (Build.VERSION.SDK_INT >= 26) {
            getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel(
                    "huxerui.system_tasks", "System tasks", NotificationManager.IMPORTANCE_LOW));
        }
    }
}
