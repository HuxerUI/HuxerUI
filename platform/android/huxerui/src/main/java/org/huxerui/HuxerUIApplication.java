package org.huxerui;

import android.app.Activity;
import android.app.Application;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ComponentCallbacks2;
import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Build;
import android.os.Bundle;
import android.os.Debug;
import android.os.Handler;
import android.os.Looper;

import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Owns one native Runtime independently of attached HuxerUI Views.
 * <p>All host operations run on the Android main thread unless explicitly stated otherwise. The existing Android
 * Application remains the process owner; this object registers lifecycle callbacks and holds its application Context.
 * A background host can initialize services without creating a View. Native permission presentation requires either
 * an attached source View or an explicitly installed launcher whose Activity is resumed.
 * <p>Foreground Activity setup, with a host-provided PermissionLauncher:
 * <pre>{@code
 * HuxerUIApplication application = HuxerUIApplication.initialize(this, getIntent(), launcher);
 * // Forward later onNewIntent calls to application.dispatchIntent(intent).
 * // Forward onRequestPermissionsResult to application.dispatchPermissionResult(...).
 * }</pre>
 */
public final class HuxerUIApplication implements Application.ActivityLifecycleCallbacks, ComponentCallbacks2 {
    static { System.loadLibrary("huxerui"); }

    /**
     * Activity-owned presentation operations used by permissions and notification authorization on the main thread.
     * Implementations forward Android permission results to dispatchPermissionResult. Removal invalidates pending
     * requests associated with this exact launcher; they are not transferred to a replacement Activity.
     */
    public interface PermissionLauncher {
        /**
         * Presents one Android permission request through the owning Activity.
         * @param permission Android manifest permission name selected by the framework.
         * @param requestCode Framework correlation code to preserve when forwarding the permission result.
         */
        void request(String permission, int requestCode);
        /**
         * Opens system settings for this application's permissions through the owning Activity.
         * @return Whether the settings activity was successfully requested.
         */
        boolean openSettings();
    }

    private static HuxerUIApplication active;
    private final Context context;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<Activity, Integer> activities = new IdentityHashMap<>();
    private final Map<HuxerUIView, Boolean> views = new WeakHashMap<>();
    private final HuxerUIPermission permission;
    private final HuxerUILocalNotification localNotification;
    private WeakReference<Activity> presentationOwner = new WeakReference<>(null);
    private PermissionLauncher permissionLauncher;
    private long presentationIdentity = 1;
    private long nativeHandle;

    private HuxerUIApplication(Context context) {
        this.context = context.getApplicationContext();
        permission = new HuxerUIPermission(this.context);
        localNotification = new HuxerUILocalNotification(this.context);
    }

    /**
     * Ensures application services exist for a background host without creating a UI or launch activation.
     * @param context Android Context whose application Context is the process Application.
     * @return The existing Runtime host, or a newly initialized background host.
     * @throws IllegalStateException If called off the main thread.
     */
    public static HuxerUIApplication initialize(Context context) {
        return initialize(context, null, false);
    }

    /**
     * Ensures a foreground Runtime exists; only a newly created Runtime consumes the startup Intent.
     * @param context Android Context whose application Context is the process Application.
     * @param intent Initial Intent, or null; unsupported input becomes a normal foreground launch.
     * @return The current application host without replaying activation when it already exists.
     * @throws IllegalStateException If called off the main thread.
     */
    public static HuxerUIApplication initialize(Context context, Intent intent) {
        return initialize(context, HuxerUIApplicationActivation.fromIntent(context, intent), true);
    }

    /**
     * Ensures a foreground Runtime and optionally installs presentation independent of a HuxerUIView.
     * @param activity Activity owning the native permission launcher.
     * @param intent Startup Intent consumed only by a newly created Runtime; use dispatchIntent for later Intents.
     * @param launcher Activity presentation operations, or null to leave an existing endpoint unchanged.
     * @return The active application host.
     * @throws IllegalStateException If called off the main thread.
     */
    public static HuxerUIApplication initialize(Activity activity, Intent intent, PermissionLauncher launcher) {
        return initialize(activity, HuxerUIApplicationActivation.fromIntent(activity, intent), true, launcher);
    }

    /**
     * Initializes from a predecoded activation without installing an application permission launcher.
     * @param context Context providing the process Application.
     * @param activation Startup payload, or null; consumed only by a new foreground Runtime.
     * @param foreground Whether startup should deliver activation rather than begin as a background host.
     * @return The existing or newly created host, accessed on the main thread.
     */
    static HuxerUIApplication initialize(Context context, HuxerUIApplicationActivation activation, boolean foreground) {
        return initialize(context, activation, foreground, null);
    }

    /**
     * Creates the native lifetime after Java callbacks and any explicit presentation endpoint are ready.
     * @param context Context providing the process Application; must be an Activity when launcher is non-null.
     * @param activation Decoded startup payload, or null for the default foreground launch.
     * @param foreground Whether to deliver startup activation; false creates services without activation.
     * @param launcher Optional Activity presentation endpoint, also applied to an already active host.
     * @return The single active application host.
     */
    private static HuxerUIApplication initialize(Context context, HuxerUIApplicationActivation activation,
                                                 boolean foreground, PermissionLauncher launcher) {
        requireMainThread();
        if (active != null) {
            if (launcher != null) active.setPermissionLauncher((Activity) context, launcher);
            return active;
        }
        HuxerUIApplication application = new HuxerUIApplication(context);
        if (context instanceof Activity) application.activities.put((Activity) context, foreground ? 1 : 2);
        if (launcher != null) {
            application.presentationOwner = new WeakReference<>((Activity) context);
            application.permissionLauncher = launcher;
        }
        active = application;
        try {
            Application owner = (Application) application.context;
            owner.registerActivityLifecycleCallbacks(application);
            owner.registerComponentCallbacks(application);
            application.nativeHandle = nativeInitialize(application, foreground, application.lifecycleState(),
                    activation == null ? 0 : activation.kind, activation == null ? null : activation.value,
                    activation == null ? null : activation.name, activation == null ? -1L : activation.size,
                    activation == null ? null : activation.contentType, activation != null && activation.writable,
                    activation == null ? null : activation.data);
            return application;
        } catch (RuntimeException | Error exception) {
            application.onRuntimeStopped();
            throw exception;
        }
    }

    /**
     * Requests Runtime shutdown on the main thread; calls after retirement have no effect.
     * Native retirement releases attached windows before removing process callbacks and the active host.
     */
    public void requestShutdown() {
        requireMainThread();
        if (nativeHandle != 0L) nativeRequestShutdown(nativeHandle);
    }

    /**
     * Replaces the application presentation endpoint on the main thread without requiring a HuxerUIView.
     * @param owner Activity kept weakly and required to be resumed before presentation; null removes the owner.
     * @param launcher Native operations for owner; pass null together with a null owner to remove the endpoint.
     * Pending requests belonging to a replaced launcher are invalidated, and the presentation generation advances.
     */
    public void setPermissionLauncher(Activity owner, PermissionLauncher launcher) {
        requireMainThread();
        if (presentationOwner.get() == owner && permissionLauncher == launcher) return;
        retireLauncher(permissionLauncher);
        ++presentationIdentity;
        presentationOwner = new WeakReference<>(owner);
        permissionLauncher = launcher;
    }

    /**
     * Resolves the original presentation source on the main thread.
     * @param source Original HuxerUIView, or null to request the explicitly installed application endpoint.
     * @return A usable launcher, or null when that source is unavailable; a View source never falls back to another
     * host.
     */
    PermissionLauncher presentationLauncher(HuxerUIView source) {
        if (source != null) return source.isAttachedToWindow() && source.getWindowVisibility() == android.view.View.VISIBLE
                ? source.permissionLauncher() : null;
        Activity owner = presentationOwner.get();
        Integer state = activities.get(owner);
        return owner != null && !owner.isFinishing() && state != null && state == 0 ? permissionLauncher : null;
    }

    /**
     * Identifies the selected endpoint generation for stale-completion checks.
     * @param source Original View, or null for the application endpoint.
     * @return A generation meaningful only for the selected source.
     */
    private long presentationIdentity(HuxerUIView source) {
        return source == null ? presentationIdentity : source.presentationIdentity();
    }

    /**
     * Invalidates pending permission and notification requests tied to an endpoint being removed.
     * @param launcher Exact former endpoint, or null for no work; called on the main thread.
     */
    void retireLauncher(PermissionLauncher launcher) {
        if (launcher == null) return;
        permission.launcherRemoved(launcher);
        localNotification.launcherRemoved(launcher);
    }

    /**
     * Routes an Activity permission completion to the operation that issued it, on the main thread.
     * @param requestCode Android correlation code supplied by PermissionLauncher.request.
     * @param permissions Permission names from onRequestPermissionsResult.
     * @param grantResults Corresponding Android grant results, including an empty cancellation result.
     * @return Whether HuxerUI recognized and consumed this request code.
     */
    public boolean dispatchPermissionResult(int requestCode, String[] permissions, int[] grantResults) {
        requireMainThread();
        return localNotification.dispatchPermissionResult(requestCode)
                || permission.dispatchResult(requestCode, permissions, grantResults);
    }

    /**
     * Delivers a supported new Intent on the main thread; it does not deduplicate repeated calls.
     * @param intent New activation Intent, or null; never replay an Activity recreation Intent through this method.
     * @return Whether a supported activation was delivered to a live Runtime.
     */
    public boolean dispatchIntent(Intent intent) {
        requireMainThread();
        HuxerUIApplicationActivation activation = HuxerUIApplicationActivation.fromIntent(context, intent);
        if (activation == null || nativeHandle == 0L) return false;
        dispatchActivation(activation);
        return true;
    }

    /**
     * Passes a decoded activation to the native Runtime on the main thread.
     * @param activation Non-null payload whose JNI values are consumed synchronously; ignored after retirement.
     */
    void dispatchActivation(HuxerUIApplicationActivation activation) {
        if (nativeHandle == 0L) return;
        nativeHandleActivation(nativeHandle, activation.kind, activation.value, activation.name,
                activation.size, activation.contentType, activation.writable, activation.data);
    }

    /**
     * Tracks a View weakly so Runtime shutdown can release its native window.
     * @param view Attached View, accessed on the main thread.
     */
    void attach(HuxerUIView view) { views.put(view, Boolean.TRUE); }
    /**
     * Removes a View from application shutdown tracking on the main thread.
     * @param view Original View whose attachment is ending.
     */
    void detach(HuxerUIView view) { views.remove(view); }
    /**
     * Returns the application Context without retaining an Activity.
     * @return The process Application Context owned by this host.
     */
    Context getContext() { return context; }
    /**
     * Looks up the active host on the main thread without creating a Runtime.
     * @return The current host, or null before initialization and after retirement.
     */
    static HuxerUIApplication current() { return active; }

    /**
     * Posts a native queue drain to the main Handler from any calling thread.
     * The posted callback rechecks the native handle and becomes inert after Runtime retirement.
     */
    private void schedulePlatformTasks() {
        handler.post(() -> { if (nativeHandle != 0L) nativeDrainTasks(nativeHandle); });
    }

    /**
     * Releases tracked windows, invalidates presentation, and unregisters lifecycle callbacks on the main thread.
     * Called by native retirement or failed initialization; an old host cannot clear a replacement active host.
     */
    private void onRuntimeStopped() {
        for (HuxerUIView view : new ArrayList<>(views.keySet())) view.releaseWindow();
        views.clear();
        nativeHandle = 0L;
        retireLauncher(permissionLauncher);
        permissionLauncher = null;
        ((Application) context).unregisterActivityLifecycleCallbacks(this);
        ((Application) context).unregisterComponentCallbacks(this);
        activities.clear();
        if (active == this) active = null;
    }

    /**
     * Aggregates tracked Activity state independently of HuxerUIView attachments.
     * @return Native lifecycle value: active (0), inactive (1), or background (2); no Activities means background.
     */
    private int lifecycleState() {
        int result = 2;
        for (int state : activities.values()) result = Math.min(result, state);
        return result;
    }

    /**
     * Records an Activity state and publishes the resulting aggregate to a live Runtime.
     * @param activity Activity reported by Application.ActivityLifecycleCallbacks on the main thread.
     * @param state Native lifecycle value: active (0), inactive (1), or background (2).
     */
    private void update(Activity activity, int state) {
        activities.put(activity, state);
        if (nativeHandle != 0L) nativeUpdateLifecycleState(nativeHandle, lifecycleState());
    }

    /**
     * {@inheritDoc}
     * Tracks the newly created Activity as inactive.
     * @param activity Activity reported on the main thread.
     * @param state Android saved-instance data; not retained by this host.
     */
    @Override public void onActivityCreated(Activity activity, Bundle state) { update(activity, 1); }
    /**
     * {@inheritDoc}
     * Tracks a visible Activity as inactive until resumed.
     * @param activity Activity reported on the main thread.
     */
    @Override public void onActivityStarted(Activity activity) { update(activity, 1); }
    /**
     * {@inheritDoc}
     * Tracks the Activity as active and makes its launcher eligible for presentation.
     * @param activity Activity reported on the main thread.
     */
    @Override public void onActivityResumed(Activity activity) { update(activity, 0); }
    /**
     * {@inheritDoc}
     * Tracks the Activity as inactive, making its application launcher unavailable.
     * @param activity Activity reported on the main thread.
     */
    @Override public void onActivityPaused(Activity activity) { update(activity, 1); }
    /**
     * {@inheritDoc}
     * Tracks the stopped Activity as background.
     * @param activity Activity reported on the main thread.
     */
    @Override public void onActivityStopped(Activity activity) { update(activity, 2); }
    /**
     * {@inheritDoc}
     * Keeps Runtime ownership independent of Activity saved-instance state.
     * @param activity Activity reported on the main thread.
     * @param state Android saved-instance data; not retained by this host.
     */
    @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
    /**
     * {@inheritDoc}
     * Removes the Activity and retires its explicit permission endpoint if selected.
     * @param activity Activity reported on the main thread.
     */
    @Override public void onActivityDestroyed(Activity activity) {
        if (presentationOwner.get() == activity) setPermissionLauncher(null, null);
        activities.remove(activity);
        if (nativeHandle != 0L) nativeUpdateLifecycleState(nativeHandle, lifecycleState());
    }
    /**
     * {@inheritDoc}
     * Refreshes application resources from the process Context on the main thread.
     * @param configuration New Android configuration; native resources reread the current Context.
     */
    @Override public void onConfigurationChanged(Configuration configuration) {
        if (nativeHandle != 0L) nativeUpdateResources(nativeHandle);
    }
    /**
     * {@inheritDoc}
     * No additional application-wide cache is owned by this Java host.
     */
    @Override public void onLowMemory() {}
    /**
     * {@inheritDoc}
     * @param level Android memory-pressure level; no additional host cache needs trimming.
     */
    @Override public void onTrimMemory(int level) {}

    /**
     * Checks the Java host's application-thread invariant.
     * @throws IllegalStateException If the caller is not on the Android main Looper.
     */
    private static void requireMainThread() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("HuxerUI application operations require the main thread");
        }
    }

    /**
     * Reads the first clipboard item through the application Context for the native clipboard bridge.
     * @return UTF-8 text, or null when no item can be read as text.
     */
    private byte[] readClipboardText() {
        ClipboardManager clipboard = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null || !clipboard.hasPrimaryClip()) {
            return null;
        }
        ClipData clip = clipboard.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0) {
            return null;
        }
        CharSequence text = clip.getItemAt(0).coerceToText(context);
        return text == null ? null : text.toString().getBytes(StandardCharsets.UTF_8);
    }

    /**
     * Publishes plain text for the native clipboard bridge on the main thread.
     * @param utf8 Non-null UTF-8 text bytes.
     * @return Whether an Android clipboard service was available to receive the text.
     */
    private boolean writeClipboardText(byte[] utf8) {
        ClipboardManager clipboard = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            return false;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("HuxerUI", new String(utf8, StandardCharsets.UTF_8)));
        return true;
    }

    /**
     * Resolves the preferred process locale, falling back to Locale.getDefault when necessary.
     * @return A UTF-8 BCP 47 language tag for native resource selection.
     */
    private byte[] resourceLocale() {
        Configuration configuration = context.getResources().getConfiguration();
        Locale locale = Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && !configuration.getLocales().isEmpty()
                ? configuration.getLocales().get(0)
                : configuration.locale;
        if (locale == null) {
            locale = Locale.getDefault();
        }
        return locale.toLanguageTag().getBytes(StandardCharsets.UTF_8);
    }

    /**
     * Reads application display density for resource selection without a View.
     * @return Android density, expressed as physical pixels per density-independent pixel.
     */
    private float resourceScale() {
        return context.getResources().getDisplayMetrics().density;
    }

    /**
     * Reads Android's current process proportional set size for Runtime metrics.
     * @return Proportional resident memory in bytes, converted from Debug.getPss kilobytes.
     */
    private long processPssBytes() {
        return Debug.getPss() * 1024L;
    }

    /**
     * Queries a permission without presenting native UI.
     * @param permissionKind Framework PermissionKind wire value.
     * @return Framework PermissionStatus wire value.
     */
    int checkPermission(int permissionKind) {
        return permission.check(permissionKind);
    }

    /**
     * Requests authorization through the original presentation endpoint on the main thread.
     * @param nativeHandle Native completion token for this request, not the application Runtime handle.
     * @param permissionKind Framework PermissionKind wire value.
     * @param source Original HuxerUIView, or null for the explicit application endpoint.
     */
    void requestPermission(long nativeHandle, int permissionKind, HuxerUIView source) {
        permission.request(nativeHandle, permissionKind, presentationLauncher(source));
    }

    /**
     * Opens application permission settings through the selected presentation endpoint.
     * @param permissionKind Framework PermissionKind wire value.
     * @param source Original View, or null for the explicit application endpoint.
     * @return Whether the selected launcher requested settings successfully.
     */
    boolean openPermissionSettings(int permissionKind, HuxerUIView source) {
        return permission.openSettings(permissionKind, presentationLauncher(source));
    }

    /**
     * Queries notification support without a window.
     * @return Native LocalNotificationCapabilities bit flags.
     */
    int localNotificationCapabilities() {
        return localNotification.capabilities();
    }

    /**
     * Checks the current application's notification authorization without presenting UI.
     * @return Native PermissionStatus wire value.
     */
    int checkLocalNotificationAuthorization() {
        return localNotification.checkAuthorization();
    }

    /**
     * Requests notification authorization on the main thread using the original presentation endpoint.
     * @param nativeHandle Native completion token for this request, not the Runtime handle.
     * @param source Original View, or null for the explicit application endpoint.
     */
    void requestLocalNotificationAuthorization(long nativeHandle, HuxerUIView source) {
        localNotification.requestAuthorization(nativeHandle, presentationLauncher(source));
    }

    /**
     * Publishes an immediate notification through the application Context.
     * @param identifier Stable application notification identifier used for replacement and cancellation.
     * @param title Resolved notification title.
     * @param body Resolved notification body.
     * @param templateIdentifier Configured Android notification channel/template identifier.
     * @param data Encoded activation data passed back when the notification is opened.
     * @return Native LocalNotificationOperationStatus wire value.
     */
    int showLocalNotification(String identifier, String title, String body, String templateIdentifier, byte[] data) {
        return localNotification.show(identifier, title, body, templateIdentifier, data);
    }

    /**
     * Schedules an application notification for a wall-clock delivery time.
     * @param identifier Stable application notification identifier used for replacement and cancellation.
     * @param title Resolved notification title.
     * @param body Resolved notification body.
     * @param templateIdentifier Configured Android notification channel/template identifier.
     * @param data Encoded activation data passed back when the notification is opened.
     * @param deliveryTimeMillis Delivery timestamp in milliseconds since the Unix epoch.
     * @return Native LocalNotificationOperationStatus wire value.
     */
    int scheduleLocalNotification(String identifier, String title, String body, String templateIdentifier,
                                  byte[] data, long deliveryTimeMillis) {
        return localNotification.schedule(identifier, title, body, templateIdentifier, data, deliveryTimeMillis);
    }

    /**
     * Cancels presentation and pending delivery for one application notification identifier.
     * @param identifier Stable identifier originally passed to show or schedule.
     * @return Native LocalNotificationOperationStatus wire value.
     */
    int cancelLocalNotification(String identifier) {
        return localNotification.cancel(identifier);
    }

    /**
     * Creates the C++ Runtime after the Java service host is ready, on the main thread.
     * @param host Java application host retained by native services.
     * @param foreground Whether to deliver startup activation; absent foreground input becomes LaunchActivation.
     * @param lifecycle Initial application lifecycle wire value.
     *
     * @param kind Activation wire value: absent (0), URL (1), file (2), or notification (3).
     * @param value URI or notification identifier, or null for no decoded activation.
     * @param name Optional file display name.
     * @param size File size in bytes, or -1 when unknown.
     * @param contentType Optional file MIME type.
     * @param writable Whether the file grant permits writing.
     * @param data Encoded notification activation data, or null.
     *
     * @return Native Runtime handle used only during this lifetime.
     */
    private static native long nativeInitialize(HuxerUIApplication host, boolean foreground, int lifecycle,
            int kind, String value, String name, long size, String contentType, boolean writable, byte[] data);
    /**
     * Delivers a subsequent activation on the main thread; JNI payloads are borrowed for this call.
     * @param handle Live native Runtime handle.
     *
     * @param kind Activation wire value: absent (0), URL (1), file (2), or notification (3).
     * @param value URI or notification identifier, or null for no decoded activation.
     * @param name Optional file display name.
     * @param size File size in bytes, or -1 when unknown.
     * @param contentType Optional file MIME type.
     * @param writable Whether the file grant permits writing.
     * @param data Encoded notification activation data, or null.
     */
    private static native void nativeHandleActivation(long handle, int kind, String value, String name, long size,
            String contentType, boolean writable, byte[] data);
    /**
     * Requests shutdown of the original native Runtime on the main thread.
     * @param handle Live native Runtime handle.
     */
    private static native void nativeRequestShutdown(long handle);
    /**
     * Drains the native application dispatcher on the main thread.
     * @param handle Live native Runtime handle.
     */
    private static native void nativeDrainTasks(long handle);
    /**
     * Publishes aggregated Android Activity state to the original Runtime.
     * @param handle Live native Runtime handle.
     * @param lifecycle Native value: active (0), inactive (1), or background (2).
     */
    private static native void nativeUpdateLifecycleState(long handle, int lifecycle);
    /**
     * Refreshes application resource configuration from the retained Context.
     * @param handle Live native Runtime handle.
     */
    private static native void nativeUpdateResources(long handle);
}
