package org.huxerui;

import android.Manifest;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;

final class HuxerUIPermission {
    private static final int CAMERA = 0;
    private static final int MICROPHONE = 1;

    private static final int GRANTED = 1;
    private static final int DENIED = 2;
    private static final int UNAVAILABLE = 5;

    private static final int REQUEST_CODE = 0x4851;

    private final android.content.Context context;
    private HuxerUIApplication.PermissionLauncher activeLauncher;
    private long activeNativeHandle;
    private String activePermission;

    HuxerUIPermission(android.content.Context context) {
        this.context = context;
    }

    int check(int permission) {
        String name = permissionName(permission);
        if (name == null || !isDeclared(name)) {
            return UNAVAILABLE;
        }
        return context.checkSelfPermission(name) == PackageManager.PERMISSION_GRANTED ? GRANTED : DENIED;
    }

    void request(long nativeHandle, int permission, HuxerUIApplication.PermissionLauncher launcher) {
        String name = permissionName(permission);
        int status = check(permission);
        if (status == GRANTED || status == UNAVAILABLE || launcher == null) {
            int result = launcher == null && status != GRANTED ? UNAVAILABLE : status;
            nativeComplete(nativeHandle, result);
            return;
        }
        if (activeNativeHandle != 0L) {
            nativeComplete(nativeHandle, UNAVAILABLE);
            return;
        }
        activeNativeHandle = nativeHandle;
        activePermission = name;
        activeLauncher = launcher;
        try {
            launcher.request(name, REQUEST_CODE);
        } catch (RuntimeException exception) {
            complete(UNAVAILABLE);
        }
    }

    boolean dispatchResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode != REQUEST_CODE || activeNativeHandle == 0L) {
            return false;
        }
        int status = DENIED;
        if (permissions != null && grantResults != null) {
            for (int index = 0; index < Math.min(permissions.length, grantResults.length); ++index) {
                if (activePermission.equals(permissions[index])
                        && grantResults[index] == PackageManager.PERMISSION_GRANTED) {
                    status = GRANTED;
                    break;
                }
            }
        }
        complete(status);
        return true;
    }

    boolean openSettings(int permission, HuxerUIApplication.PermissionLauncher launcher) {
        if (permissionName(permission) == null || launcher == null) {
            return false;
        }
        try {
            return launcher.openSettings();
        } catch (RuntimeException exception) {
            return false;
        }
    }

    /**
     * Completes a pending request as unavailable when its original presentation endpoint is removed.
     * @param launcher Exact former endpoint, checked on the main thread; other requests are unaffected.
     */
    void launcherRemoved(HuxerUIApplication.PermissionLauncher launcher) {
        if (launcher == activeLauncher && activeNativeHandle != 0L) {
            complete(UNAVAILABLE);
        }
    }

    private String permissionName(int permission) {
        switch (permission) {
        case CAMERA:
            return Manifest.permission.CAMERA;
        case MICROPHONE:
            return Manifest.permission.RECORD_AUDIO;
        default:
            return null;
        }
    }

    @SuppressWarnings("deprecation")
    private boolean isDeclared(String permission) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(
                    context.getPackageName(), PackageManager.GET_PERMISSIONS);
            if (info.requestedPermissions == null) {
                return false;
            }
            for (String declared : info.requestedPermissions) {
                if (permission.equals(declared)) {
                    return true;
                }
            }
        } catch (PackageManager.NameNotFoundException exception) {
            return false;
        }
        return false;
    }

    private void complete(int status) {
        long nativeHandle = activeNativeHandle;
        activeNativeHandle = 0L;
        activePermission = null;
        activeLauncher = null;
        nativeComplete(nativeHandle, status);
    }

    private static native void nativeComplete(long nativeHandle, int status);
}
