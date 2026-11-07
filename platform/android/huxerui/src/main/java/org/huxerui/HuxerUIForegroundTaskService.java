package org.huxerui;

import android.app.Service;
import android.app.Notification;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/**
 * Foreground task component declared directly in the application manifest with its service type, task identifiers,
 * notification channel, and small icon. Subclasses can provide independent platform component identities when needed.
 */
public class HuxerUIForegroundTaskService extends Service {
    int notificationId;

    @Override
    public final IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    @SuppressWarnings("deprecation")
    public final int onStartCommand(Intent intent, int flags, int startId) {
        HuxerUIApplication application = HuxerUIApplication.current();
        HuxerUISystemTasks tasks = application == null ? null : application.systemTasks();
        String token = intent == null ? null : intent.getStringExtra(HuxerUISystemTasks.TOKEN);
        try {
            if (intent == null) return START_NOT_STICKY;
            if (HuxerUISystemTasks.STOP.equals(intent.getAction())) {
                if (tasks != null) tasks.stopForegroundTask(this, token);
            } else {
                if (notificationId == 0) {
                    // A canceled request still owes Android its foreground promotion, even after Runtime retirement.
                    Notification notification = intent.getParcelableExtra(HuxerUISystemTasks.NOTIFICATION);
                    int id = intent.getIntExtra(HuxerUISystemTasks.NOTIFICATION_ID, 0);
                    if (notification == null || id == 0) {
                        throw new IllegalArgumentException("HuxerUI foreground task startup presentation is missing");
                    }
                    startForeground(id, notification);
                    notificationId = id;
                }
                if (tasks != null) tasks.beginForeground(this, token);
            }
        } catch (RuntimeException exception) {
            Log.e("HuxerUI", "HuxerUI could not start or dispatch a foreground task", exception);
            if (tasks != null) tasks.rejectForeground(token, exception);
        } finally {
            if (tasks == null || !tasks.hasForegroundTask(this)) {
                try {
                    removeForeground();
                } finally {
                    stopSelfResult(startId);
                }
            }
        }
        return START_NOT_STICKY;
    }

    @Override
    public final void onTimeout(int startId) {
        expire();
    }

    @Override
    public final void onTimeout(int startId, int foregroundServiceType) {
        expire();
    }

    private void expire() {
        try {
            HuxerUIApplication application = HuxerUIApplication.current();
            if (application != null) application.systemTasks().revokeForeground(this);
        } finally {
            try {
                removeForeground();
            } finally {
                stopSelf();
            }
        }
    }

    @SuppressWarnings("deprecation")
    void removeForeground() {
        stopForeground(true);
        notificationId = 0;
    }

    @Override
    public void onDestroy() {
        HuxerUIApplication application = HuxerUIApplication.current();
        if (application != null) application.systemTasks().revokeForeground(this);
        super.onDestroy();
    }
}
