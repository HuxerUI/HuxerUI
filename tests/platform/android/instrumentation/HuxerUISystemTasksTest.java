package org.huxerui;

import android.app.Activity;
import android.app.Instrumentation;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Exercises foreground starts whose original Runtime and request have already retired. */
public final class HuxerUISystemTasksTest {
    private static final String CHANNEL = "huxerui.test.system_tasks";
    private static int assertions;

    private HuxerUISystemTasksTest() {}

    static int verify(Instrumentation instrumentation) throws InterruptedException {
        assertions = 0;
        check(new HuxerUIForegroundTaskService().onBind(null) == null,
                "The SDK task service must be directly instantiable as a start-only service");
        Context context = instrumentation.getTargetContext();
        NotificationManager notifications = context.getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26) {
            notifications.createNotificationChannel(new NotificationChannel(CHANNEL, "System task tests",
                    NotificationManager.IMPORTANCE_LOW));
        }
        Activity activity = instrumentation.startActivitySync(new Intent(context, StartupActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        StartupService.stopped = new CountDownLatch(1);
        StartupService.promotions.set(0);
        Throwable[] failure = new Throwable[1];
        try {
            instrumentation.runOnMainSync(() -> {
                try {
                    check(HuxerUIApplication.current() == null, "The retired-start test requires no live Runtime");
                    // Both starts are queued before the service can receive either command.
                    start(context, 0x4855F001);
                    start(context, 0x4855F002);
                } catch (Throwable error) {
                    failure[0] = error;
                }
            });
            if (failure[0] != null) throw new AssertionError("Foreground task startup failed", failure[0]);
            check(StartupService.stopped.await(10, TimeUnit.SECONDS),
                    "Retired starts must stop their platform service");
            check(StartupService.promotions.get() == 2, "Each retired start must promote before removing its notification");
        } finally {
            context.stopService(new Intent(context, StartupService.class));
            instrumentation.runOnMainSync(activity::finish);
            if (Build.VERSION.SDK_INT >= 26) notifications.deleteNotificationChannel(CHANNEL);
        }
        return assertions;
    }

    @SuppressWarnings("deprecation")
    private static void start(Context context, int id) {
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(context, CHANNEL)
                : new Notification.Builder(context);
        Notification notification = builder.setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("Canceled system task").build();
        Intent intent = new Intent(context, StartupService.class)
                .putExtra(HuxerUISystemTasks.TOKEN, "retired-" + id)
                .putExtra(HuxerUISystemTasks.NOTIFICATION, notification)
                .putExtra(HuxerUISystemTasks.NOTIFICATION_ID, id);
        if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent);
        else context.startService(intent);
    }

    private static void check(boolean value, String message) {
        ++assertions;
        if (!value) throw new AssertionError(message);
    }

    /** Supplies foreground eligibility while the task service starts. */
    public static final class StartupActivity extends Activity {}

    /** Records actual promotion before the production service removes foreground presentation. */
    public static final class StartupService extends HuxerUIForegroundTaskService {
        static CountDownLatch stopped;
        static final AtomicInteger promotions = new AtomicInteger();

        @Override
        void removeForeground() {
            if (notificationId != 0) promotions.incrementAndGet();
            super.removeForeground();
        }

        @Override
        public void onDestroy() {
            try {
                super.onDestroy();
            } finally {
                stopped.countDown();
            }
        }
    }
}
