package org.huxerui;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.AppOpsManager;
import android.app.NotificationManager;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.os.Build;
import android.os.Environment;
import android.os.Process;
import android.os.UserManager;
import android.provider.Settings;

final class HuxerUIPermission {
    // Wire values follow huxerui::Permission, PermissionStatus, and PermissionSettingsTarget.
    private static final int CAMERA = 0;
    private static final int MICROPHONE = 1;
    private static final int LOCATION = 2;
    private static final int PRECISE_LOCATION = 3;
    private static final int BACKGROUND_LOCATION = 4;
    static final int NOTIFICATIONS = 5;
    private static final int CONTACTS_READ = 6;
    private static final int CALENDAR_ADD = 7;
    private static final int CALENDAR_READ_WRITE = 8;
    private static final int PHOTOS_READ = 9;
    private static final int VIDEOS_READ = 10;
    private static final int PHOTO_LIBRARY_ADD = 11;
    private static final int BLUETOOTH_SCAN = 12;
    private static final int BLUETOOTH_CONNECT = 13;

    static final int GRANTED = 1;
    static final int DENIED = 2;
    private static final int RESTRICTED = 4;
    static final int UNAVAILABLE = 5;
    private static final int LIMITED = 7;
    private static final int SYSTEM_DISABLED = 8;

    private static final int APPLICATION_SETTINGS = 0;
    private static final int SYSTEM_SETTINGS = 1;
    // Use a separate range from activity results. A retired launcher may still forward its old callback.
    private static final int FIRST_REQUEST_CODE = 0x4800;
    private static final int LAST_REQUEST_CODE = 0x4fff;
    private static int nextRequestCode = FIRST_REQUEST_CODE;

    interface Completion {
        void complete(int status);
    }

    private final Context context;
    private HuxerUIApplication.PermissionLauncher activeLauncher;
    private Completion activeCompletion;
    private int activePermission;
    private int activeRequestCode;

    HuxerUIPermission(Context context) {
        this.context = context;
    }

    int check(int permission) {
        try {
            if (permission == NOTIFICATIONS) {
                return checkNotifications(context);
            }
            if (permission == PHOTO_LIBRARY_ADD && usesScopedStorage()) {
                return GRANTED;
            }
            String[] names = permissionNames(permission);
            if (names == null || !hasDeclarations(context, names)) {
                return UNAVAILABLE;
            }
            if (isLocation(permission)) {
                if (permission == BACKGROUND_LOCATION
                        && !hasDeclarations(context, Manifest.permission.ACCESS_COARSE_LOCATION)
                        && !hasDeclarations(context, Manifest.permission.ACCESS_FINE_LOCATION)) {
                    return UNAVAILABLE;
                }
                int locationStatus = locationAvailability();
                if (locationStatus != GRANTED) {
                    return locationStatus;
                }
                boolean foreground = hasForegroundLocation();
                if (permission == LOCATION || permission == BACKGROUND_LOCATION && Build.VERSION.SDK_INT < 29) {
                    return foreground ? GRANTED : DENIED;
                }
                if (!foreground) {
                    return DENIED;
                }
                if (permission == PRECISE_LOCATION) {
                    return isGranted(Manifest.permission.ACCESS_FINE_LOCATION) ? GRANTED : LIMITED;
                }
            }
            if (isBluetooth(permission)) {
                int bluetoothStatus = bluetoothAvailability();
                if (bluetoothStatus != GRANTED) {
                    return bluetoothStatus;
                }
            }
            int grants = 0;
            for (String name : names) {
                if (isGranted(name)) {
                    ++grants;
                }
            }
            if (grants == names.length) {
                return GRANTED;
            }
            if ((permission == PHOTOS_READ || permission == VIDEOS_READ) && Build.VERSION.SDK_INT >= 34
                    && isGranted(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)) {
                return LIMITED;
            }
            return permission == CALENDAR_READ_WRITE && grants != 0 ? LIMITED : DENIED;
        } catch (RuntimeException exception) {
            return UNAVAILABLE;
        }
    }

    void request(long nativeHandle, int permission, HuxerUIApplication.PermissionLauncher launcher) {
        request(permission, launcher, status -> nativeComplete(nativeHandle, status));
    }

    void request(int permission, HuxerUIApplication.PermissionLauncher launcher, Completion completion) {
        int status = check(permission);
        if (status != DENIED && status != LIMITED) {
            completion.complete(status);
            return;
        }
        String[] names;
        try {
            names = requestNames(permission);
        } catch (RuntimeException exception) {
            completion.complete(UNAVAILABLE);
            return;
        }
        if (names == null) {
            completion.complete(status);
            return;
        }
        if (launcher == null || activeCompletion != null) {
            completion.complete(UNAVAILABLE);
            return;
        }
        int requestCode = nextRequestCode;
        nextRequestCode = requestCode == LAST_REQUEST_CODE ? FIRST_REQUEST_CODE : requestCode + 1;
        activeCompletion = completion;
        activePermission = permission;
        activeLauncher = launcher;
        activeRequestCode = requestCode;
        try {
            launcher.request(names, requestCode);
        } catch (RuntimeException exception) {
            if (activeRequestCode == requestCode) {
                complete(UNAVAILABLE);
            }
        }
    }

    boolean dispatchResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode != activeRequestCode || activeCompletion == null) {
            return false;
        }
        // A nonempty callback only ends the interaction. Recheck the actual grants, including partial access.
        boolean canceled = permissions == null || permissions.length == 0 || grantResults == null
                || grantResults.length == 0;
        complete(canceled ? UNAVAILABLE : check(activePermission));
        return true;
    }

    boolean openSettings(int permission, int target, HuxerUIApplication.PermissionLauncher launcher) {
        if (launcher == null || permission < CAMERA || permission > BLUETOOTH_CONNECT) {
            return false;
        }
        try {
            if (target == APPLICATION_SETTINGS) {
                if (permission == NOTIFICATIONS && Build.VERSION.SDK_INT >= 26) {
                    Intent intent = new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                            .putExtra(Settings.EXTRA_APP_PACKAGE, context.getPackageName());
                    try {
                        if (launcher.openSettings(intent)) {
                            return true;
                        }
                    } catch (ActivityNotFoundException ignored) {
                    }
                }
                return launcher.openSettings();
            }
            if (target == SYSTEM_SETTINGS) {
                if (isLocation(permission)) {
                    return launcher.openSettings(new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS));
                }
                if (isBluetooth(permission)) {
                    return launcher.openSettings(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS));
                }
            }
        } catch (RuntimeException exception) {
            return false;
        }
        return false;
    }

    void launcherRemoved(HuxerUIApplication.PermissionLauncher launcher) {
        if (launcher == activeLauncher && activeCompletion != null) {
            complete(UNAVAILABLE);
        }
    }

    @SuppressLint("InlinedApi")
    private String[] permissionNames(int permission) {
        switch (permission) {
        case CAMERA:
            return new String[] {Manifest.permission.CAMERA};
        case MICROPHONE:
            return new String[] {Manifest.permission.RECORD_AUDIO};
        case LOCATION:
            return new String[] {Manifest.permission.ACCESS_COARSE_LOCATION};
        case PRECISE_LOCATION:
            return new String[] {Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION};
        case BACKGROUND_LOCATION:
            return Build.VERSION.SDK_INT >= 29 ? new String[] {Manifest.permission.ACCESS_BACKGROUND_LOCATION}
                    : permissionNames(LOCATION);
        case CONTACTS_READ:
            return new String[] {Manifest.permission.READ_CONTACTS};
        case CALENDAR_ADD:
            return new String[] {Manifest.permission.WRITE_CALENDAR};
        case CALENDAR_READ_WRITE:
            return new String[] {Manifest.permission.READ_CALENDAR, Manifest.permission.WRITE_CALENDAR};
        case PHOTOS_READ:
            return new String[] {usesMediaPermissions() ? Manifest.permission.READ_MEDIA_IMAGES
                    : Manifest.permission.READ_EXTERNAL_STORAGE};
        case VIDEOS_READ:
            return new String[] {usesMediaPermissions() ? Manifest.permission.READ_MEDIA_VIDEO
                    : Manifest.permission.READ_EXTERNAL_STORAGE};
        case PHOTO_LIBRARY_ADD:
            return new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE};
        case BLUETOOTH_SCAN:
            if (usesNearbyDevicePermissions()) {
                return new String[] {Manifest.permission.BLUETOOTH_SCAN};
            }
            if (!hasDeclarations(context, Manifest.permission.BLUETOOTH, Manifest.permission.BLUETOOTH_ADMIN)) {
                return null;
            }
            return new String[] {Build.VERSION.SDK_INT < 29
                    && hasDeclarations(context, Manifest.permission.ACCESS_COARSE_LOCATION)
                    ? Manifest.permission.ACCESS_COARSE_LOCATION : Manifest.permission.ACCESS_FINE_LOCATION};
        case BLUETOOTH_CONNECT:
            return new String[] {usesNearbyDevicePermissions() ? Manifest.permission.BLUETOOTH_CONNECT
                    : Manifest.permission.BLUETOOTH};
        default:
            return null;
        }
    }

    private String[] requestNames(int permission) {
        if (permission == NOTIFICATIONS) {
            return Build.VERSION.SDK_INT >= 33 && context.getApplicationInfo().targetSdkVersion >= 33
                    ? new String[] {Manifest.permission.POST_NOTIFICATIONS} : null;
        }
        if (permission == BACKGROUND_LOCATION && Build.VERSION.SDK_INT >= 29
                && (Build.VERSION.SDK_INT >= 30 || !hasForegroundLocation())) {
            return null;
        }
        if (permission == BLUETOOTH_CONNECT && !usesNearbyDevicePermissions()) {
            return null;
        }
        String[] names = permissionNames(permission);
        if ((permission == PHOTOS_READ || permission == VIDEOS_READ) && Build.VERSION.SDK_INT >= 34
                && usesMediaPermissions()
                && hasDeclarations(context, Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)) {
            return new String[] {names[0], Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED};
        }
        return names;
    }

    private boolean usesMediaPermissions() {
        return Build.VERSION.SDK_INT >= 33 && context.getApplicationInfo().targetSdkVersion >= 33;
    }

    private boolean usesNearbyDevicePermissions() {
        return Build.VERSION.SDK_INT >= 31 && context.getApplicationInfo().targetSdkVersion >= 31;
    }

    static int checkNotifications(Context context) {
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                if (!hasDeclarations(context, Manifest.permission.POST_NOTIFICATIONS)) {
                    return UNAVAILABLE;
                }
                if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                        != PackageManager.PERMISSION_GRANTED) {
                    return DENIED;
                }
            }
            if (Build.VERSION.SDK_INT >= 24) {
                NotificationManager manager = context.getSystemService(NotificationManager.class);
                return manager == null ? UNAVAILABLE : manager.areNotificationsEnabled() ? GRANTED : DENIED;
            }
            AppOpsManager manager = context.getSystemService(AppOpsManager.class);
            if (manager == null) {
                return UNAVAILABLE;
            }
            int mode = manager.checkOpNoThrow("android:post_notification", Process.myUid(), context.getPackageName());
            return mode == AppOpsManager.MODE_ALLOWED || mode == AppOpsManager.MODE_DEFAULT ? GRANTED : DENIED;
        } catch (RuntimeException exception) {
            return UNAVAILABLE;
        }
    }

    @SuppressWarnings("deprecation")
    private int locationAvailability() {
        UserManager user = context.getSystemService(UserManager.class);
        if (user != null && user.hasUserRestriction(UserManager.DISALLOW_SHARE_LOCATION)) {
            return RESTRICTED;
        }
        LocationManager manager = context.getSystemService(LocationManager.class);
        if (manager == null) {
            return UNAVAILABLE;
        }
        boolean enabled = Build.VERSION.SDK_INT >= 28 ? manager.isLocationEnabled()
                : Settings.Secure.getInt(context.getContentResolver(), Settings.Secure.LOCATION_MODE,
                        Settings.Secure.LOCATION_MODE_OFF) != Settings.Secure.LOCATION_MODE_OFF;
        if (enabled) {
            return GRANTED;
        }
        return Build.VERSION.SDK_INT >= 28 && user != null
                && user.hasUserRestriction(UserManager.DISALLOW_CONFIG_LOCATION) ? RESTRICTED : SYSTEM_DISABLED;
    }

    @SuppressLint("MissingPermission")
    private int bluetoothAvailability() {
        BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null) {
            return UNAVAILABLE;
        }
        try {
            return adapter.isEnabled() ? GRANTED : SYSTEM_DISABLED;
        } catch (SecurityException exception) {
            // Some hosts require connect authorization even to read the adapter's power state.
            return GRANTED;
        }
    }

    @SuppressWarnings("deprecation")
    private static boolean usesScopedStorage() {
        return Build.VERSION.SDK_INT >= 29 && !Environment.isExternalStorageLegacy();
    }

    @SuppressWarnings("deprecation")
    static boolean hasDeclarations(Context context, String... names) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(
                    context.getPackageName(), PackageManager.GET_PERMISSIONS);
            if (info.requestedPermissions == null) {
                return false;
            }
            for (String name : names) {
                boolean found = false;
                for (String declared : info.requestedPermissions) {
                    if (name.equals(declared)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return false;
                }
            }
            return true;
        } catch (PackageManager.NameNotFoundException | RuntimeException exception) {
            return false;
        }
    }

    private boolean isGranted(String permission) {
        return context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean hasForegroundLocation() {
        return isGranted(Manifest.permission.ACCESS_COARSE_LOCATION)
                || isGranted(Manifest.permission.ACCESS_FINE_LOCATION);
    }

    private static boolean isLocation(int permission) {
        return permission == LOCATION || permission == PRECISE_LOCATION || permission == BACKGROUND_LOCATION;
    }

    private static boolean isBluetooth(int permission) {
        return permission == BLUETOOTH_SCAN || permission == BLUETOOTH_CONNECT;
    }

    private void complete(int status) {
        Completion completion = activeCompletion;
        activeCompletion = null;
        activeLauncher = null;
        activeRequestCode = 0;
        completion.complete(status);
    }

    private static native void nativeComplete(long nativeHandle, int status);
}
