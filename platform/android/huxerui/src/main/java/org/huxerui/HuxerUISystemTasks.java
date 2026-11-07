package org.huxerui;

import android.app.ActivityManager;
import android.app.Application;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.PersistableBundle;
import android.os.Process;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

import static org.huxerui.PlatformPayload.booleanValue;
import static org.huxerui.PlatformPayload.int64;
import static org.huxerui.PlatformPayload.object;
import static org.huxerui.PlatformPayload.string;

/** Owns Android job and foreground service allowances on the application's main thread. */
final class HuxerUISystemTasks {
    private static final int BACKGROUND = 0;
    private static final int UNSUPPORTED = 0;
    private static final int UNAVAILABLE = 1;
    private static final int NOT_PERMITTED = 2;
    private static final int NOT_FOREGROUND = 3;
    private static final int CONFIGURATION = 4;
    private static final int REJECTED = 6;
    private static final int PLATFORM = 7;
    private static final long NO_EARLIEST = Long.MIN_VALUE;
    private static final String JOB_NAMESPACE = "org.huxerui.system_tasks";
    private static final String IDENTIFIERS = "org.huxerui.system_task.identifiers";
    private static final String CHANNEL = "org.huxerui.system_task.channel";
    private static final String ICON = "org.huxerui.system_task.icon";
    static final String TOKEN = "org.huxerui.system_task.token";
    static final String STOP = "org.huxerui.system_task.stop";
    static final String NOTIFICATION = "org.huxerui.system_task.notification";
    static final String NOTIFICATION_ID = "org.huxerui.system_task.notification_id";
    private static final PlatformPayload SUCCESS = object(Collections.emptyMap());

    private final HuxerUIApplication host;
    private final Context context;
    private final Map<Long, JobRun> jobs = new HashMap<>();
    private final Map<String, ForegroundRun> foreground = new LinkedHashMap<>();
    private long nextAllowance = 1;
    private int nextNotification = 0x48550000;

    HuxerUISystemTasks(HuxerUIApplication host, Context context) {
        this.host = host;
        this.context = context;
    }

    private static final class JobRun {
        final HuxerUIBackgroundJobService service;
        final JobParameters parameters;
        final PlatformPayload request;

        JobRun(HuxerUIBackgroundJobService service, JobParameters parameters, PlatformPayload request) {
            this.service = service;
            this.parameters = parameters;
            this.request = request;
        }
    }

    private static final class ForegroundRun {
        final PlatformPayload request;
        final ComponentName component;
        final String channel;
        final int icon;
        final int actionId;
        String title;
        String subtitle;
        int completed;
        int total;
        boolean showProgress;
        long allowance;
        HuxerUIForegroundTaskService service;

        ForegroundRun(PlatformPayload request, ServiceInfo info, int actionId) {
            this.request = request;
            this.component = new ComponentName(info.packageName, info.name);
            this.channel = info.metaData.getString(CHANNEL, "");
            this.icon = info.metaData.getInt(ICON);
            this.actionId = actionId;
            this.title = text(request, "title");
            this.subtitle = text(request, "subtitle");
        }
    }

    PlatformPayload recover(PlatformPayload identifier) {
        try {
            ComponentName component = jobComponent();
            PlatformPayload recovered = null;
            for (JobInfo job : scheduler().getAllPendingJobs()) {
                if (!component.equals(job.getService())) continue;
                PlatformPayload request = jobRequest(job.getExtras());
                if (request != null && identifier.requireString().equals(text(request, "identifier"))) {
                    if (recovered != null) return error(PLATFORM, "Multiple system jobs match the task");
                    recovered = request;
                }
            }
            return recovered == null ? SUCCESS : object(Collections.singletonMap("request", recovered));
        } catch (IllegalArgumentException | PackageManager.NameNotFoundException exception) {
            return error(CONFIGURATION, exception.getMessage());
        } catch (RuntimeException exception) {
            return error(PLATFORM, exception.getMessage());
        }
    }

    PlatformPayload submit(PlatformPayload request) {
        try {
            if (number(request, "kind") == BACKGROUND) {
                JobScheduler scheduler = scheduler();
                ComponentName component = jobComponent();
                List<JobInfo> existing = scheduler.getAllPendingJobs();
                Set<Integer> used = new HashSet<>();
                for (JobInfo job : existing) {
                    used.add(job.getId());
                }
                int jobId = text(request, "token").hashCode() & Integer.MAX_VALUE;
                while (used.contains(jobId)) jobId = (jobId + 1) & Integer.MAX_VALUE;
                long earliest = number(request, "earliest");
                long latency = earliest == NO_EARLIEST ? 1L : Math.max(1L, earliest - System.currentTimeMillis());
                PersistableBundle extras = new PersistableBundle();
                extras.putString(TOKEN, text(request, "token"));
                extras.putString("identifier", text(request, "identifier"));
                extras.putLong("earliest", earliest);
                extras.putBoolean("network", request.requireField("network").requireBoolean());
                extras.putBoolean("power", request.requireField("power").requireBoolean());
                JobInfo job = new JobInfo.Builder(jobId, component)
                        .setExtras(extras)
                        .setMinimumLatency(latency)
                        .setRequiredNetworkType(extras.getBoolean("network") ? JobInfo.NETWORK_TYPE_ANY
                                : JobInfo.NETWORK_TYPE_NONE)
                        .setRequiresCharging(extras.getBoolean("power"))
                        .build();
                return scheduler.schedule(job) == JobScheduler.RESULT_SUCCESS ? SUCCESS
                        : error(REJECTED, "JobScheduler rejected the request");
            }

            ServiceInfo info = foregroundComponent(text(request, "identifier"));
            if (!host.isForegroundForSystemTasks()) {
                return error(NOT_FOREGROUND, "Starting a foreground task requires a resumed Activity");
            }
            ForegroundRun run = new ForegroundRun(request, info, nextNotification++);
            String token = text(request, "token");
            foreground.put(token, run);
            try {
                Intent intent = new Intent().setComponent(run.component).putExtra(TOKEN, token)
                        .putExtra(NOTIFICATION, notification(run)).putExtra(NOTIFICATION_ID, run.actionId);
                ComponentName started = Build.VERSION.SDK_INT >= 26 ? context.startForegroundService(intent)
                        : context.startService(intent);
                if (started == null) {
                    foreground.remove(token);
                    return error(REJECTED, "Android rejected the foreground service");
                }
            } catch (RuntimeException exception) {
                foreground.remove(token);
                throw exception;
            }
            return SUCCESS;
        } catch (PackageManager.NameNotFoundException | IllegalArgumentException exception) {
            return error(CONFIGURATION, exception.getMessage());
        } catch (SecurityException exception) {
            return error(NOT_PERMITTED, exception.getMessage());
        } catch (IllegalStateException exception) {
            return error(NOT_FOREGROUND, exception.getMessage());
        } catch (RuntimeException exception) {
            return error(PLATFORM, exception.getMessage());
        }
    }

    PlatformPayload withdraw(PlatformPayload request) {
        String token = text(request, "token");
        try {
            if (number(request, "kind") == BACKGROUND) {
                ComponentName component = jobComponent();
                JobScheduler scheduler = scheduler();
                for (JobInfo job : scheduler.getAllPendingJobs()) {
                    if (component.equals(job.getService()) && token.equals(job.getExtras().getString(TOKEN))) {
                        scheduler.cancel(job.getId());
                        break;
                    }
                }
            } else {
                ForegroundRun run = foreground.get(token);
                if (run != null) finishForeground(run);
            }
            return SUCCESS;
        } catch (PackageManager.NameNotFoundException | IllegalArgumentException exception) {
            return error(CONFIGURATION, exception.getMessage());
        } catch (RuntimeException exception) {
            return error(PLATFORM, exception.getMessage());
        }
    }

    boolean beginJob(HuxerUIBackgroundJobService service, JobParameters parameters) {
        PlatformPayload request = jobRequest(parameters.getExtras());
        if (request == null) return false;
        long allowance = nextAllowance++;
        boolean admitted = false;
        try {
            jobComponent();
            jobs.put(allowance, new JobRun(service, parameters, request));
            admitted = emit("granted", request, allowance, null);
            return admitted;
        } catch (PackageManager.NameNotFoundException | RuntimeException exception) {
            return false;
        } finally {
            if (!admitted) jobs.remove(allowance);
        }
    }

    void stopJob(HuxerUIBackgroundJobService service, JobParameters parameters) {
        String token = parameters.getExtras().getString(TOKEN);
        for (Map.Entry<Long, JobRun> entry : jobs.entrySet()) {
            JobRun run = entry.getValue();
            if (run.service == service && run.parameters.getJobId() == parameters.getJobId()
                    && text(run.request, "token").equals(token)) {
                long allowance = entry.getKey();
                jobs.remove(allowance);
                emit("revoked", run.request, allowance, null);
                return;
            }
        }
    }

    boolean beginForeground(HuxerUIForegroundTaskService service, String token) {
        ForegroundRun run = foreground.get(token);
        if (run == null || run.service != null
                || !run.component.equals(new ComponentName(service, service.getClass()))) return false;
        run.service = service;
        run.allowance = nextAllowance++;
        boolean admitted = false;
        try {
            admitted = emit("granted", run.request, run.allowance, null);
            return admitted;
        } finally {
            if (!admitted) finishForeground(run);
        }
    }

    /**
     * Reports failed foreground-service promotion for a request whose handler has not been admitted.
     * @param token Exact request from the startup Intent.
     * @param exception Platform startup failure, mapped to permission or configuration rejection.
     */
    void rejectForeground(String token, RuntimeException exception) {
        ForegroundRun run = foreground.get(token);
        if (run == null || run.service != null) return;
        foreground.remove(token);
        int code = exception instanceof SecurityException ? NOT_PERMITTED : CONFIGURATION;
        try {
            emit("rejected", run.request, 0, error(code, exception.getMessage()));
        } catch (RuntimeException failure) {
            Log.e("HuxerUI", "HuxerUI could not report foreground task startup failure", failure);
        }
    }

    void stopForegroundTask(HuxerUIForegroundTaskService service, String token) {
        ForegroundRun run = foreground.get(token);
        if (run != null && run.service == service) emit("stop", run.request, run.allowance, null);
    }

    boolean hasForegroundTask(HuxerUIForegroundTaskService service) {
        return activeForeground(service) != null;
    }

    void revokeForeground(HuxerUIForegroundTaskService service) {
        for (ForegroundRun run : new ArrayList<>(foreground.values())) {
            if (run.service == service) {
                foreground.remove(text(run.request, "token"));
                try {
                    emit("revoked", run.request, run.allowance, null);
                } catch (RuntimeException exception) {
                    Log.e("HuxerUI", "HuxerUI could not report a revoked foreground allowance", exception);
                }
            }
        }
    }

    PlatformPayload finish(PlatformPayload value) {
        long allowance = number(value, "allowance");
        JobRun job = jobs.remove(allowance);
        if (job != null) {
            job.service.jobFinished(job.parameters, value.requireField("retry").requireBoolean());
            return SUCCESS;
        }
        ForegroundRun run = foregroundRun(allowance);
        if (run != null) finishForeground(run);
        return SUCCESS;
    }

    PlatformPayload update(PlatformPayload value) {
        ForegroundRun run = foregroundRun(number(value, "allowance"));
        if (run == null) return error(UNAVAILABLE, "The foreground service allowance has ended");
        try {
            Map<String, PlatformPayload> fields = value.fields();
            if (fields.containsKey("title")) {
                run.title = text(value, "title");
                run.subtitle = text(value, "subtitle");
            } else {
                String total = text(value, "total");
                int maximum = total.isEmpty() ? 0 : Integer.parseInt(total);
                int completed = total.isEmpty() ? 0 : Integer.parseInt(text(value, "completed"));
                run.total = maximum;
                run.completed = completed;
                run.showProgress = true;
            }
            if (activeForeground(run.service) == run) showForeground(run);
            return SUCCESS;
        } catch (NumberFormatException exception) {
            return error(UNSUPPORTED, "Notification progress exceeds the platform numeric range");
        } catch (RuntimeException exception) {
            return error(PLATFORM, exception.getMessage());
        }
    }

    void disconnect() {
        List<JobRun> detachedJobs = new ArrayList<>(jobs.values());
        jobs.clear();
        for (JobRun job : detachedJobs) {
            try {
                job.service.jobFinished(job.parameters, false);
            } catch (RuntimeException exception) {
                Log.e("HuxerUI", "HuxerUI could not finish a detached background job", exception);
            }
        }
        for (ForegroundRun run : new ArrayList<>(foreground.values())) {
            try {
                finishForeground(run);
            } catch (RuntimeException exception) {
                Log.e("HuxerUI", "HuxerUI could not finish a detached foreground task", exception);
            }
        }
    }

    private ForegroundRun foregroundRun(long allowance) {
        for (ForegroundRun run : foreground.values()) if (run.allowance == allowance && allowance != 0) return run;
        return null;
    }

    private ForegroundRun activeForeground(HuxerUIForegroundTaskService service) {
        if (service == null) return null;
        for (ForegroundRun run : foreground.values()) if (run.service == service) return run;
        return null;
    }

    private void finishForeground(ForegroundRun run) {
        foreground.remove(text(run.request, "token"));
        HuxerUIForegroundTaskService service = run.service;
        if (service == null) return;
        ForegroundRun remaining = activeForeground(service);
        if (remaining != null) {
            try {
                showForeground(remaining);
            } catch (RuntimeException exception) {
                Log.w("HuxerUI", "HuxerUI could not refresh the remaining foreground notification", exception);
            }
            return;
        }
        for (ForegroundRun pending : foreground.values()) {
            if (pending.component.equals(run.component)) return;
        }
        try {
            service.removeForeground();
        } finally {
            service.stopSelf();
        }
    }

    private void showForeground(ForegroundRun run) {
        context.getSystemService(NotificationManager.class).notify(run.service.notificationId, notification(run));
    }

    @SuppressWarnings("deprecation")
    private Notification notification(ForegroundRun run) {
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(context, run.channel)
                : new Notification.Builder(context);
        Intent stop = new Intent(STOP).setComponent(run.component).putExtra(TOKEN, text(run.request, "token"));
        PendingIntent stopAction = PendingIntent.getService(context, run.actionId, stop, PendingIntent.FLAG_IMMUTABLE);
        builder.setSmallIcon(run.icon).setContentTitle(run.title).setContentText(run.subtitle)
                .setOngoing(true).setOnlyAlertOnce(true)
                .addAction(new Notification.Action.Builder(null, "Stop", stopAction).build());
        if (run.showProgress) builder.setProgress(run.total, run.completed, run.total == 0);
        Intent launch = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        if (launch != null) builder.setContentIntent(PendingIntent.getActivity(
                context, 0, launch, PendingIntent.FLAG_IMMUTABLE));
        return builder.build();
    }

    private JobScheduler scheduler() {
        JobScheduler scheduler = context.getSystemService(JobScheduler.class);
        return Build.VERSION.SDK_INT >= 34 ? scheduler.forNamespace(JOB_NAMESPACE) : scheduler;
    }

    @SuppressWarnings("deprecation")
    private ComponentName jobComponent() throws PackageManager.NameNotFoundException {
        ComponentName component = new ComponentName(context, HuxerUIBackgroundJobService.class);
        ServiceInfo info = context.getPackageManager().getServiceInfo(component, 0);
        validateProcess(info);
        if (!"android.permission.BIND_JOB_SERVICE".equals(info.permission) || !info.exported) {
            throw new IllegalArgumentException("JobService must be exported and protected by BIND_JOB_SERVICE");
        }
        return component;
    }

    @SuppressWarnings("deprecation")
    private ServiceInfo foregroundComponent(String identifier) throws PackageManager.NameNotFoundException {
        PackageManager packages = context.getPackageManager();
        ServiceInfo[] services = packages.getPackageInfo(context.getPackageName(),
                PackageManager.GET_SERVICES | PackageManager.GET_META_DATA).services;
        ServiceInfo match = null;
        if (services != null) {
            for (ServiceInfo info : services) {
                Bundle metadata = info.metaData;
                if (metadata == null) continue;
                for (String name : metadata.getString(IDENTIFIERS, "").split(",")) {
                    if (!identifier.equals(name.trim())) continue;
                    if (match != null) throw new IllegalArgumentException("Task is assigned to multiple services");
                    match = info;
                }
            }
        }
        if (match == null) throw new IllegalArgumentException("No foreground service is configured for " + identifier);
        validateProcess(match);
        try {
            Class<?> type = Class.forName(match.name, false, context.getClassLoader());
            if (!HuxerUIForegroundTaskService.class.isAssignableFrom(type)) {
                throw new IllegalArgumentException("Task service must be HuxerUIForegroundTaskService or a subclass");
            }
        } catch (ClassNotFoundException exception) {
            throw new IllegalArgumentException("Task service class is missing", exception);
        }
        String channel = match.metaData.getString(CHANNEL, "");
        if (match.exported || match.metaData.getInt(ICON) == 0 ||
                (Build.VERSION.SDK_INT >= 26 && context.getSystemService(NotificationManager.class)
                        .getNotificationChannel(channel) == null)) {
            throw new IllegalArgumentException("Task service needs a private component, icon, and notification channel");
        }
        if (Build.VERSION.SDK_INT >= 29 && context.getApplicationInfo().targetSdkVersion >= 34
                && match.getForegroundServiceType() == 0) {
            throw new IllegalArgumentException("Task service needs a foreground service type");
        }
        return match;
    }

    private void validateProcess(ServiceInfo service) {
        String expected = context.getApplicationInfo().processName;
        String current = null;
        if (Build.VERSION.SDK_INT >= 28) {
            current = Application.getProcessName();
        } else {
            List<ActivityManager.RunningAppProcessInfo> processes = context.getSystemService(ActivityManager.class)
                    .getRunningAppProcesses();
            if (processes != null) for (ActivityManager.RunningAppProcessInfo process : processes) {
                if (process.pid == Process.myPid()) current = process.processName;
            }
        }
        if (!service.enabled || !expected.equals(current) || !expected.equals(service.processName)
                || (service.flags & ServiceInfo.FLAG_ISOLATED_PROCESS) != 0) {
            throw new IllegalArgumentException("Task components and Runtime must use the default application process");
        }
    }

    private static PlatformPayload jobRequest(PersistableBundle extras) {
        String token = extras.getString(TOKEN);
        String identifier = extras.getString("identifier");
        if (token == null || token.isEmpty() || identifier == null || identifier.isEmpty()) return null;
        Map<String, PlatformPayload> fields = new HashMap<>();
        fields.put("token", string(token));
        fields.put("identifier", string(identifier));
        fields.put("instance", string(""));
        fields.put("kind", int64(BACKGROUND));
        fields.put("earliest", int64(extras.getLong("earliest", NO_EARLIEST)));
        fields.put("network", booleanValue(extras.getBoolean("network")));
        fields.put("power", booleanValue(extras.getBoolean("power")));
        return object(fields);
    }

    private boolean emit(String event, PlatformPayload request, long allowance, PlatformPayload failure) {
        Map<String, PlatformPayload> fields = new HashMap<>();
        if (failure != null) fields.putAll(failure.fields());
        fields.put("event", string(event));
        fields.put("request", request);
        fields.put("allowance", int64(allowance));
        return host.dispatchSystemTaskEvent(object(fields));
    }

    private static PlatformPayload error(int code, String message) {
        Map<String, PlatformPayload> fields = new HashMap<>();
        fields.put("error", int64(code));
        fields.put("message", string("HuxerUI Android system task: "
                + (message == null ? "platform failure" : message)));
        return object(fields);
    }

    private static String text(PlatformPayload value, String key) {
        return value.requireField(key).requireString();
    }

    private static long number(PlatformPayload value, String key) {
        return value.requireField(key).requireInt64();
    }
}
