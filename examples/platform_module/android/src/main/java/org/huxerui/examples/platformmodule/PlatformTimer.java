package org.huxerui.examples.platformmodule;

import android.content.Context;

import java.util.Objects;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

public final class PlatformTimer implements HuxerUIPlatformModule.Factory {
    public PlatformTimer() {}

    @Override
    public HuxerUIPlatformModule create(
            Context context, PlatformPayload options, HuxerUIPlatformChannel.Events events) {
        options.requireNull();
        return new Instance(context, events);
    }

    private static final class Instance implements HuxerUIPlatformModule {
        private final ScheduledExecutorService executor;
        private final HuxerUIPlatformChannel.Events events;
        private long generation;
        private long tick;
        private ScheduledFuture<?> task;
        private HuxerUIPlatformChannel.Result pendingStart;
        private boolean disposed;

        Instance(Context context, HuxerUIPlatformChannel.Events events) {
            String threadName = "HuxerUI-" + Objects.requireNonNull(context).getPackageName() + "-Timer";
            executor = Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(runnable, threadName);
                thread.setDaemon(true);
                return thread;
            });
            this.events = events;
        }

        @Override
        public HuxerUIPlatformChannel.Cancellation invoke(
                String method, PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            switch (method) {
                case "start":
                    return start(arguments.requireInt64(), result);
                case "stop":
                    arguments.requireNull();
                    stop(result);
                    return null;
                default:
                    result.fail("huxerui/unsupported-method", "HuxerUI example timer does not support " + method,
                            PlatformPayload.nullValue());
                    return null;
            }
        }

        @Override
        public void dispose() {
            synchronized (this) {
                if (disposed) {
                    return;
                }
                disposed = true;
                generation++;
                pendingStart = null;
                stopLocked();
            }
            executor.shutdownNow();
        }

        private HuxerUIPlatformChannel.Cancellation start(
                long intervalMilliseconds, HuxerUIPlatformChannel.Result result) {
            if (intervalMilliseconds <= 0L) {
                result.fail("example/timer-interval", "The Android timer interval must be positive",
                        PlatformPayload.nullValue());
                return null;
            }

            HuxerUIPlatformChannel.Result replaced;
            long nextGeneration;
            boolean started = true;
            synchronized (this) {
                if (disposed) {
                    result.fail("example/timer-closed", "The Android timer is closed", PlatformPayload.nullValue());
                    return null;
                }
                replaced = pendingStart;
                stopLocked();
                nextGeneration = ++generation;
                tick = 0L;
                pendingStart = result;
                try {
                    task = executor.scheduleAtFixedRate(()
                                                                -> tick(nextGeneration),
                            intervalMilliseconds, intervalMilliseconds, TimeUnit.MILLISECONDS);
                } catch (RejectedExecutionException exception) {
                    pendingStart = null;
                    started = false;
                }
            }
            if (replaced != null) {
                replaced.fail("example/timer-replaced", "The timer was replaced by a newer start call",
                        PlatformPayload.nullValue());
            }
            if (!started) {
                result.fail(
                        "example/timer-java", "The Android timer could not be started", PlatformPayload.nullValue());
                return null;
            }
            return () -> cancelStart(nextGeneration);
        }

        private void stop(HuxerUIPlatformChannel.Result result) {
            HuxerUIPlatformChannel.Result pending;
            synchronized (this) {
                if (disposed) {
                    result.fail("example/timer-closed", "The Android timer is closed", PlatformPayload.nullValue());
                    return;
                }
                generation++;
                pending = pendingStart;
                pendingStart = null;
                stopLocked();
            }
            if (pending != null) {
                pending.fail("example/timer-stopped", "The timer stopped before its first tick",
                        PlatformPayload.nullValue());
            }
            result.complete(PlatformPayload.nullValue());
        }

        private synchronized void cancelStart(long cancelledGeneration) {
            if (!disposed && generation == cancelledGeneration) {
                generation++;
                pendingStart = null;
                stopLocked();
            }
        }

        private void tick(long scheduledGeneration) {
            HuxerUIPlatformChannel.Result firstResult;
            long nextTick;
            synchronized (this) {
                if (disposed || generation != scheduledGeneration || task == null) {
                    return;
                }
                nextTick = ++tick;
                firstResult = pendingStart;
                pendingStart = null;
            }
            if (firstResult != null) {
                firstResult.complete(PlatformPayload.int64(nextTick));
            }
            events.emit("tick", PlatformPayload.int64(nextTick));
        }

        private void stopLocked() {
            if (task != null) {
                task.cancel(false);
                task = null;
            }
        }
    }
}
