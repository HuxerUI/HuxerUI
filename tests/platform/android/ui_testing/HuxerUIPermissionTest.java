package org.huxerui;

import android.Manifest;
import android.app.Instrumentation;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.os.Build;
import android.provider.Settings;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

final class HuxerUIPermissionTest {
    private static final int LOCATION = 2;
    private static final int PRECISE_LOCATION = 3;
    private static final int CONTACTS_READ = 6;
    private static final int CALENDAR_ADD = 7;
    private static final int CALENDAR_READ_WRITE = 8;
    private static final int PHOTOS_READ = 9;
    private static final int LIMITED = 7;
    private static final int SYSTEM_DISABLED = 8;
    private static final int APPLICATION = 0;
    private static final int SYSTEM = 1;

    static void verify(Instrumentation instrumentation) {
        Context base = instrumentation.getTargetContext();
        Set<String> granted = new HashSet<>();
        Context context = new ContextWrapper(base) {
            @Override public int checkSelfPermission(String permission) {
                return granted.contains(permission) ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
            }
        };
        HuxerUIPermission permissions = new HuxerUIPermission(context);
        Launcher launcher = new Launcher();
        int[] result = {-1};

        permissions.request(CALENDAR_READ_WRITE, launcher, status -> result[0] = status);
        check(Arrays.equals(launcher.names, new String[] {Manifest.permission.READ_CALENDAR,
                Manifest.permission.WRITE_CALENDAR}), "Calendar read and write must be requested together");
        granted.add(Manifest.permission.WRITE_CALENDAR);
        check(permissions.dispatchResult(launcher.code, launcher.names, new int[] {-1, 0}) && result[0] == LIMITED,
                "Calendar write access alone must be reported as limited read-write access");
        check(permissions.check(CALENDAR_ADD) == HuxerUIPermission.GRANTED, "Calendar add must accept write access");

        permissions.request(CONTACTS_READ, launcher, status -> result[0] = status);
        int retiredCode = launcher.code;
        permissions.launcherRemoved(launcher);
        check(result[0] == HuxerUIPermission.UNAVAILABLE, "Removing the launcher must retire its pending request");
        permissions.request(CONTACTS_READ, launcher, status -> result[0] = status);
        check(!permissions.dispatchResult(retiredCode, new String[] {Manifest.permission.READ_CONTACTS}, new int[] {0}),
                "A retired permission result must not finish the new request");
        check(permissions.dispatchResult(launcher.code, new String[0], new int[0])
                        && result[0] == HuxerUIPermission.UNAVAILABLE,
                "An empty permission callback must be treated as cancellation");

        check(permissions.openSettings(LOCATION, APPLICATION, launcher) && launcher.openedApplication,
                "Application location settings must use the application page");
        check(permissions.openSettings(LOCATION, SYSTEM, launcher)
                        && Settings.ACTION_LOCATION_SOURCE_SETTINGS.equals(launcher.settings.getAction()),
                "System location settings must open the location switch page");
        check(!permissions.openSettings(CONTACTS_READ, SYSTEM, launcher), "Unsupported settings must return false");

        if (Build.VERSION.SDK_INT >= 28) {
            LocationManager location = base.getSystemService(LocationManager.class);
            if (location != null && location.isLocationEnabled()) {
                granted.add(Manifest.permission.ACCESS_COARSE_LOCATION);
                check(permissions.check(LOCATION) == HuxerUIPermission.GRANTED,
                        "Approximate access must satisfy foreground location");
                check(permissions.check(PRECISE_LOCATION) == LIMITED,
                        "Approximate access must not satisfy precise location");
                permissions.request(PRECISE_LOCATION, launcher, status -> result[0] = status);
                check(Arrays.equals(launcher.names, new String[] {Manifest.permission.ACCESS_FINE_LOCATION,
                        Manifest.permission.ACCESS_COARSE_LOCATION}), "Precision upgrades must include coarse location");
                check(permissions.dispatchResult(launcher.code, new String[0], new int[0]),
                        "The precision request must finish after cancellation");
            } else if (location != null) {
                check(permissions.check(LOCATION) == SYSTEM_DISABLED,
                        "A disabled location switch must be distinct from app denial");
            }
        }

        if (Build.VERSION.SDK_INT >= 33) {
            check(permissions.check(HuxerUIPermission.NOTIFICATIONS) == HuxerUIPermission.DENIED,
                    "Application notification authorization must be independent of notification channels");
            check(permissions.openSettings(HuxerUIPermission.NOTIFICATIONS, APPLICATION, launcher)
                            && Settings.ACTION_APP_NOTIFICATION_SETTINGS.equals(launcher.settings.getAction())
                            && base.getPackageName().equals(launcher.settings.getStringExtra(Settings.EXTRA_APP_PACKAGE)),
                    "Notification settings must target the application package");
        }

        if (Build.VERSION.SDK_INT >= 34) {
            permissions.request(PHOTOS_READ, launcher, status -> result[0] = status);
            check(Arrays.equals(launcher.names, new String[] {Manifest.permission.READ_MEDIA_IMAGES,
                    Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED}),
                    "Media requests must let the user grant selected items");
            granted.add(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED);
            check(permissions.dispatchResult(launcher.code, launcher.names, new int[] {-1, 0}) && result[0] == LIMITED,
                    "Selected photos must not be reported as full access");
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static final class Launcher implements HuxerUIApplication.PermissionLauncher {
        String[] names;
        int code;
        boolean openedApplication;
        Intent settings;

        @Override public void request(String permission, int requestCode) {
            request(new String[] {permission}, requestCode);
        }

        @Override public void request(String[] permissions, int requestCode) {
            names = permissions;
            code = requestCode;
        }

        @Override public boolean openSettings() {
            openedApplication = true;
            return true;
        }

        @Override public boolean openSettings(Intent intent) {
            settings = intent;
            return true;
        }
    }
}
