package org.huxerui;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.AlarmManager;
import android.app.AppOpsManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;

import java.util.Collections;

final class HuxerUILocalNotification {
    static final String CHANNEL_ID_METADATA = "org.huxerui.local_notification.channel_id";
    static final String SMALL_ICON_METADATA = "org.huxerui.local_notification.small_icon";

    static final String ALARM_ACTION = "org.huxerui.action.LOCAL_NOTIFICATION_ALARM";
    static final String ACTIVATION_ACTION = "org.huxerui.action.LOCAL_NOTIFICATION";
    static final String IDENTIFIER_EXTRA = "org.huxerui.extra.LOCAL_NOTIFICATION_IDENTIFIER";
    static final String DATA_EXTRA = "org.huxerui.extra.LOCAL_NOTIFICATION_DATA";
    private static final int MAX_DATA_BYTES = 64 * 1024;

    private static final String TITLE_EXTRA = "org.huxerui.extra.LOCAL_NOTIFICATION_TITLE";
    private static final String BODY_EXTRA = "org.huxerui.extra.LOCAL_NOTIFICATION_BODY";
    private static final String TEMPLATE_EXTRA = "org.huxerui.extra.LOCAL_NOTIFICATION_TEMPLATE";

    private static final int GRANTED = 1;
    private static final int DENIED = 2;
    private static final int UNAVAILABLE_AUTHORIZATION = 5;

    private static final int ACCEPTED = 0;
    private static final int UNAUTHORIZED = 1;
    private static final int UNAVAILABLE_OPERATION = 2;
    private static final int FAILED = 3;

    private static final int CAN_SHOW = 1;
    private static final int CAN_SCHEDULE = 1 << 1;
    private static final int CAN_CANCEL = 1 << 2;
    private static final int CAN_ACTIVATE = 1 << 3;
    private static final int CAN_USE_TEMPLATES = 1 << 4;

    private static final int REQUEST_CODE = 0x4852;
    private static final int NOTIFICATION_ID = 0x4855;

    private final HuxerUIView view;
    private final Context context;
    private final Configuration configuration;
    private long activeNativeHandle;

    HuxerUILocalNotification(HuxerUIView view) {
        this.view = view;
        context = view.getContext().getApplicationContext();
        configuration = Configuration.load(context);
    }

    int capabilities() {
        return capabilityBits(configuration);
    }

    static int configuredCapabilities(Context context) {
        return capabilityBits(Configuration.load(context.getApplicationContext()));
    }

    private static int capabilityBits(Configuration configuration) {
        int result = 0;
        if (configuration.canCancel()) {
            result |= CAN_CANCEL;
        }
        if (configuration.canShow()) {
            result |= CAN_SHOW;
            if (configuration.canActivate()) {
                result |= CAN_ACTIVATE;
            }
            if (configuration.canSchedule()) {
                result |= CAN_SCHEDULE;
            }
            if (configuration.canUseTemplates()) {
                result |= CAN_USE_TEMPLATES;
            }
        }
        return result;
    }

    int checkAuthorization() {
        return checkAuthorization(context, configuration);
    }

    void requestAuthorization(long nativeHandle) {
        int status = checkAuthorization();
        if (status == GRANTED || status == UNAVAILABLE_AUTHORIZATION || Build.VERSION.SDK_INT < 33) {
            nativeCompleteAuthorization(nativeHandle, status);
            return;
        }
        if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
            nativeCompleteAuthorization(nativeHandle, status);
            return;
        }
        if (view.permissionLauncher() == null || activeNativeHandle != 0L) {
            nativeCompleteAuthorization(nativeHandle, UNAVAILABLE_AUTHORIZATION);
            return;
        }
        activeNativeHandle = nativeHandle;
        try {
            view.permissionLauncher().request(Manifest.permission.POST_NOTIFICATIONS, REQUEST_CODE);
        } catch (RuntimeException exception) {
            completeAuthorization(UNAVAILABLE_AUTHORIZATION);
        }
    }

    boolean dispatchPermissionResult(int requestCode) {
        if (requestCode != REQUEST_CODE || activeNativeHandle == 0L) {
            return false;
        }
        completeAuthorization(checkAuthorization());
        return true;
    }

    void launcherChanged() {
        if (view.permissionLauncher() == null && activeNativeHandle != 0L) {
            completeAuthorization(UNAVAILABLE_AUTHORIZATION);
        }
    }

    int show(String identifier, String title, String body, String templateIdentifier, byte[] data) {
        int presentation = presentationStatus(configuration, templateIdentifier);
        if (presentation != ACCEPTED) {
            return presentation;
        }
        int submission = submissionStatus();
        if (submission != ACCEPTED) {
            return submission;
        }
        try {
            cancelScheduled(context, configuration, identifier);
            return post(context, configuration, identifier, title, body, templateIdentifier, data);
        } catch (SecurityException exception) {
            return UNAUTHORIZED;
        } catch (RuntimeException exception) {
            return FAILED;
        }
    }

    int schedule(String identifier, String title, String body, String templateIdentifier, byte[] data,
                 long deliveryTimeMillis) {
        if (!configuration.canSchedule()) {
            return UNAVAILABLE_OPERATION;
        }
        int presentation = presentationStatus(configuration, templateIdentifier);
        if (presentation != ACCEPTED) {
            return presentation;
        }
        int submission = submissionStatus();
        if (submission != ACCEPTED) {
            return submission;
        }
        try {
            decodeData(data);
            PendingIntent pendingIntent = alarmPendingIntent(context, identifier, title, body, templateIdentifier,
                                                             data, PendingIntent.FLAG_UPDATE_CURRENT);
            if (pendingIntent == null) {
                return FAILED;
            }
            configuration.alarmManager.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, deliveryTimeMillis, pendingIntent);
            return ACCEPTED;
        } catch (SecurityException exception) {
            return FAILED;
        } catch (RuntimeException exception) {
            return FAILED;
        }
    }

    int cancel(String identifier) {
        if (!configuration.canCancel()) {
            return UNAVAILABLE_OPERATION;
        }
        try {
            cancelScheduled(context, configuration, identifier);
            configuration.notificationManager.cancel(identifier, NOTIFICATION_ID);
            return ACCEPTED;
        } catch (RuntimeException exception) {
            return FAILED;
        }
    }

    static void deliverScheduled(Context receiverContext, Intent intent) {
        if (intent == null || !ALARM_ACTION.equals(intent.getAction())) {
            return;
        }
        String identifier = intent.getStringExtra(IDENTIFIER_EXTRA);
        String title = intent.getStringExtra(TITLE_EXTRA);
        String body = intent.getStringExtra(BODY_EXTRA);
        String templateIdentifier = intent.getStringExtra(TEMPLATE_EXTRA);
        if (identifier == null || identifier.isEmpty() || (isEmpty(title) && isEmpty(body))) {
            return;
        }
        Context context = receiverContext.getApplicationContext();
        Configuration configuration = Configuration.load(context);
        if (presentationStatus(configuration, templateIdentifier) != ACCEPTED
            || checkAuthorization(context, configuration) != GRANTED) {
            return;
        }
        try {
            post(context, configuration, identifier, title, body, templateIdentifier, readData(intent));
        } catch (RuntimeException ignored) {
        }
    }

    static Uri activationUri(Context context, String identifier) {
        return identityUri(context, "activation", identifier);
    }

    static byte[] readData(Intent intent) {
        Bundle extras = intent.getExtras();
        Object value = extras == null ? null : extras.get(DATA_EXTRA);
        if (value != null && !(value instanceof byte[])) {
            throw new IllegalArgumentException("HuxerUI local notification data must contain an encoded byte value");
        }
        return (byte[]) value;
    }

    static PlatformPayload decodeData(byte[] data) {
        if (data == null) {
            return PlatformPayload.nullValue();
        }
        if (data.length > MAX_DATA_BYTES) {
            throw new IllegalArgumentException("HuxerUI local notification data exceeds the 64 KiB encoded size limit");
        }
        // No retained resource tables exist after a scheduled notification outlives its process.
        return PlatformPayload.decodeEnvelope(
                data, Collections.emptyList(), Collections.emptyList(), Collections.emptyList());
    }

    static Uri alarmUri(Context context, String identifier) {
        return identityUri(context, "alarm", identifier);
    }

    static boolean isActivationIntent(Context context, Intent intent, String identifier) {
        return ACTIVATION_ACTION.equals(intent.getAction())
                && activationUri(context, identifier).equals(intent.getData());
    }

    private int submissionStatus() {
        int authorization = checkAuthorization();
        if (authorization == UNAVAILABLE_AUTHORIZATION) {
            return UNAVAILABLE_OPERATION;
        }
        return authorization == GRANTED ? ACCEPTED : UNAUTHORIZED;
    }

    private void completeAuthorization(int status) {
        long nativeHandle = activeNativeHandle;
        activeNativeHandle = 0L;
        nativeCompleteAuthorization(nativeHandle, status);
    }

    private static int checkAuthorization(Context context, Configuration configuration) {
        if (!configuration.canShow()) {
            return UNAVAILABLE_AUTHORIZATION;
        }
        if (Build.VERSION.SDK_INT >= 33
            && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
            return DENIED;
        }
        try {
            if (Build.VERSION.SDK_INT >= 24) {
                if (!Api24.areNotificationsEnabled(configuration.notificationManager)) {
                    return DENIED;
                }
            } else if (!Api23.areNotificationsEnabled(context)) {
                return DENIED;
            }
            if (Build.VERSION.SDK_INT >= 26) {
                int importance = Api26.channelImportance(configuration.notificationManager, configuration.channelId);
                if (importance == Api26.MISSING_CHANNEL) {
                    return UNAVAILABLE_AUTHORIZATION;
                }
                if (importance == NotificationManager.IMPORTANCE_NONE) {
                    return DENIED;
                }
            }
        } catch (RuntimeException exception) {
            return UNAVAILABLE_AUTHORIZATION;
        }
        return GRANTED;
    }

    private static int presentationStatus(Configuration configuration, String templateIdentifier) {
        if (templateIdentifier == null) {
            return ACCEPTED;
        }
        if (templateIdentifier.isEmpty() || !configuration.canUseTemplates()) {
            return UNAVAILABLE_OPERATION;
        }
        try {
            return configuration.layoutProvider.supportsLocalNotificationTemplate(templateIdentifier)
                    ? ACCEPTED
                    : UNAVAILABLE_OPERATION;
        } catch (RuntimeException exception) {
            return FAILED;
        }
    }

    private static int post(Context context, Configuration configuration, String identifier, String title, String body,
                            String templateIdentifier, byte[] data) {
        PlatformPayload payload = decodeData(data);
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            builder = Api26.builder(context, configuration.channelId);
        } else {
            builder = new Notification.Builder(context).setPriority(Notification.PRIORITY_DEFAULT);
        }
        builder.setSmallIcon(configuration.smallIcon).setAutoCancel(true);
        if (!isEmpty(title)) {
            builder.setContentTitle(title);
        }
        if (!isEmpty(body)) {
            builder.setContentText(body);
        }
        if (templateIdentifier != null) {
            HuxerUILocalNotificationLayout layout = configuration.layoutProvider.createLocalNotificationLayout(
                    new HuxerUILocalNotificationContent(identifier, title, body, templateIdentifier, payload));
            if (layout == null || !layout.hasViews()) {
                return FAILED;
            }
            Api24.applyLayout(builder, layout);
        }
        // Construct the template before replacing the data attached to an existing activation PendingIntent.
        PendingIntent activation = activationPendingIntent(context, identifier, data);
        if (activation != null) {
            builder.setContentIntent(activation);
        }
        configuration.notificationManager.notify(identifier, NOTIFICATION_ID, builder.build());
        return ACCEPTED;
    }

    private static PendingIntent activationPendingIntent(Context context, String identifier, byte[] data) {
        Intent intent = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        if (intent == null) {
            return null;
        }
        intent.setAction(ACTIVATION_ACTION);
        intent.setData(activationUri(context, identifier));
        intent.putExtra(IDENTIFIER_EXTRA, identifier);
        intent.putExtra(DATA_EXTRA, data);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(context, 0, intent, pendingIntentFlags(PendingIntent.FLAG_UPDATE_CURRENT));
    }

    private static PendingIntent alarmPendingIntent(Context context, String identifier, String title, String body,
                                                    String templateIdentifier, byte[] data, int flags) {
        Intent intent = new Intent(context, HuxerUILocalNotificationReceiver.class);
        intent.setAction(ALARM_ACTION);
        intent.setData(alarmUri(context, identifier));
        if ((flags & PendingIntent.FLAG_NO_CREATE) == 0) {
            intent.putExtra(IDENTIFIER_EXTRA, identifier);
            intent.putExtra(TITLE_EXTRA, title);
            intent.putExtra(BODY_EXTRA, body);
            intent.putExtra(TEMPLATE_EXTRA, templateIdentifier);
            intent.putExtra(DATA_EXTRA, data);
        }
        return PendingIntent.getBroadcast(context, 0, intent, pendingIntentFlags(flags));
    }

    private static void cancelScheduled(Context context, Configuration configuration, String identifier) {
        if (configuration.alarmManager == null) {
            return;
        }
        PendingIntent pendingIntent =
                alarmPendingIntent(context, identifier, null, null, null, null, PendingIntent.FLAG_NO_CREATE);
        if (pendingIntent != null) {
            configuration.alarmManager.cancel(pendingIntent);
            pendingIntent.cancel();
        }
    }

    private static int pendingIntentFlags(int flags) {
        return flags | PendingIntent.FLAG_IMMUTABLE;
    }

    private static Uri identityUri(Context context, String purpose, String identifier) {
        // Extras do not participate in PendingIntent identity; keep the full identifier in its data URI.
        // Separate alarm and activation URIs avoid collisions without relying on a lossy string hash.
        return new Uri.Builder()
                .scheme("huxerui")
                .authority(context.getPackageName())
                .appendPath("local-notification")
                .appendPath(purpose)
                .appendPath(identifier)
                .build();
    }

    private static boolean isEmpty(String value) {
        return value == null || value.isEmpty();
    }

    private static boolean hasPermissionDeclaration(Context context, String permission) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(),
                                                                          PackageManager.GET_PERMISSIONS);
            if (info.requestedPermissions == null) {
                return false;
            }
            for (String declared : info.requestedPermissions) {
                if (permission.equals(declared)) {
                    return true;
                }
            }
        } catch (PackageManager.NameNotFoundException | RuntimeException exception) {
            return false;
        }
        return false;
    }

    private static boolean hasReceiverDeclaration(Context context) {
        try {
            ComponentName receiver = new ComponentName(context, HuxerUILocalNotificationReceiver.class);
            ActivityInfo info = context.getPackageManager().getReceiverInfo(receiver, 0);
            return info.enabled && !info.exported;
        } catch (PackageManager.NameNotFoundException | RuntimeException exception) {
            return false;
        }
    }

    private static boolean isResource(Context context, int identifier) {
        if (identifier == 0) {
            return false;
        }
        try {
            context.getResources().getResourceTypeName(identifier);
            return true;
        } catch (Resources.NotFoundException exception) {
            return false;
        }
    }

    private static final class Configuration {
        final Context context;
        final NotificationManager notificationManager;
        final AlarmManager alarmManager;
        final String channelId;
        final int smallIcon;
        final boolean channelAvailable;
        final boolean permissionDeclared;
        final boolean receiverDeclared;
        final boolean activationAvailable;
        final HuxerUILocalNotificationLayoutProvider layoutProvider;

        private Configuration(Context context, NotificationManager notificationManager, AlarmManager alarmManager,
                              String channelId, int smallIcon, boolean channelAvailable, boolean permissionDeclared,
                              boolean receiverDeclared, boolean activationAvailable,
                              HuxerUILocalNotificationLayoutProvider layoutProvider) {
            this.context = context;
            this.notificationManager = notificationManager;
            this.alarmManager = alarmManager;
            this.channelId = channelId;
            this.smallIcon = smallIcon;
            this.channelAvailable = channelAvailable;
            this.permissionDeclared = permissionDeclared;
            this.receiverDeclared = receiverDeclared;
            this.activationAvailable = activationAvailable;
            this.layoutProvider = layoutProvider;
        }

        static Configuration load(Context context) {
            NotificationManager notificationManager =
                    (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
            AlarmManager alarmManager = (AlarmManager) context.getSystemService(Context.ALARM_SERVICE);
            String channelId = null;
            int smallIcon = 0;
            try {
                ApplicationInfo application = context.getPackageManager().getApplicationInfo(
                        context.getPackageName(), PackageManager.GET_META_DATA);
                Bundle metadata = application.metaData;
                if (metadata != null) {
                    channelId = metadata.getString(CHANNEL_ID_METADATA);
                    smallIcon = metadata.getInt(SMALL_ICON_METADATA, 0);
                }
            } catch (PackageManager.NameNotFoundException | RuntimeException exception) {
                channelId = null;
                smallIcon = 0;
            }
            boolean channelAvailable = Build.VERSION.SDK_INT < 26;
            if (Build.VERSION.SDK_INT >= 26 && notificationManager != null && !isEmpty(channelId)) {
                try {
                    channelAvailable = Api26.channelAvailable(notificationManager, channelId);
                } catch (RuntimeException exception) {
                    channelAvailable = false;
                }
            }
            boolean permissionDeclared = Build.VERSION.SDK_INT < 33
                    || hasPermissionDeclaration(context, Manifest.permission.POST_NOTIFICATIONS);
            boolean activationAvailable = false;
            try {
                activationAvailable =
                        context.getPackageManager().getLaunchIntentForPackage(context.getPackageName()) != null;
            } catch (RuntimeException ignored) {
            }
            HuxerUILocalNotificationLayoutProvider layoutProvider =
                    context instanceof HuxerUILocalNotificationLayoutProvider
                    ? (HuxerUILocalNotificationLayoutProvider) context
                    : null;
            return new Configuration(context, notificationManager, alarmManager, channelId, smallIcon, channelAvailable,
                                     permissionDeclared, hasReceiverDeclaration(context), activationAvailable,
                                     layoutProvider);
        }

        boolean canShow() {
            return notificationManager != null && isResource(context, smallIcon) && permissionDeclared
                    && channelAvailable;
        }

        boolean canSchedule() {
            return canShow() && alarmManager != null && receiverDeclared;
        }

        boolean canCancel() {
            return notificationManager != null && alarmManager != null;
        }

        boolean canActivate() {
            return canShow() && activationAvailable;
        }

        boolean canUseTemplates() {
            return canShow() && Build.VERSION.SDK_INT >= 24 && layoutProvider != null;
        }
    }

    private static final class Api23 {
        private static final String POST_NOTIFICATION_OPERATION = "android:post_notification";

        private Api23() {}

        static boolean areNotificationsEnabled(Context context) {
            AppOpsManager manager = (AppOpsManager) context.getSystemService(Context.APP_OPS_SERVICE);
            if (manager == null) {
                return false;
            }
            int mode = manager.checkOpNoThrow(POST_NOTIFICATION_OPERATION, Process.myUid(), context.getPackageName());
            return mode == AppOpsManager.MODE_ALLOWED || mode == AppOpsManager.MODE_DEFAULT;
        }
    }

    @SuppressLint("NewApi")
    private static final class Api24 {
        private Api24() {}

        static boolean areNotificationsEnabled(NotificationManager manager) {
            return manager.areNotificationsEnabled();
        }

        static void applyLayout(Notification.Builder builder, HuxerUILocalNotificationLayout layout) {
            builder.setStyle(new Notification.DecoratedCustomViewStyle());
            if (layout.getContentView() != null) {
                builder.setCustomContentView(layout.getContentView());
            }
            if (layout.getExpandedContentView() != null) {
                builder.setCustomBigContentView(layout.getExpandedContentView());
            }
            if (layout.getHeadsUpContentView() != null) {
                builder.setCustomHeadsUpContentView(layout.getHeadsUpContentView());
            }
        }
    }

    @SuppressLint("NewApi")
    private static final class Api26 {
        static final int MISSING_CHANNEL = Integer.MIN_VALUE;

        private Api26() {}

        static Notification.Builder builder(Context context, String channelId) {
            return new Notification.Builder(context, channelId);
        }

        static boolean channelAvailable(NotificationManager manager, String channelId) {
            return manager.getNotificationChannel(channelId) != null;
        }

        static int channelImportance(NotificationManager manager, String channelId) {
            NotificationChannel channel = manager.getNotificationChannel(channelId);
            return channel == null ? MISSING_CHANNEL : channel.getImportance();
        }
    }

    private static native void nativeCompleteAuthorization(long nativeHandle, int status);
}
