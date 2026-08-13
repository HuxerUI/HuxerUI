package org.huxerui.demo;

import android.content.Context;

import java.util.Objects;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

final class NativeTimer {
    private final ScheduledExecutorService executor;
    private long nativeBridge;
    private long generation;
    private ScheduledFuture<?> task;

    NativeTimer(Context context, long nativeBridge) {
        String threadName = "HuxerUI-" + Objects.requireNonNull(context).getPackageName() + "-Timer";
        executor = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, threadName);
            thread.setDaemon(true);
            return thread;
        });
        this.nativeBridge = nativeBridge;
    }

    synchronized void start(long intervalMilliseconds, long nextGeneration) {
        stopLocked();
        generation = nextGeneration;
        task = executor.scheduleAtFixedRate(
                () -> tick(nextGeneration),
                intervalMilliseconds,
                intervalMilliseconds,
                TimeUnit.MILLISECONDS);
    }

    synchronized void cancel(long cancelledGeneration) {
        if (generation == cancelledGeneration) {
            stopLocked();
        }
    }

    synchronized void stop() {
        stopLocked();
    }

    synchronized void disposeNativeBridge() {
        stopLocked();
        nativeBridge = 0L;
        executor.shutdownNow();
    }

    private synchronized void tick(long scheduledGeneration) {
        if (nativeBridge != 0L && generation == scheduledGeneration) {
            nativeTick(nativeBridge, scheduledGeneration);
        }
    }

    private void stopLocked() {
        generation = 0L;
        if (task != null) {
            task.cancel(false);
            task = null;
        }
    }

    private static native void nativeTick(long bridge, long generation);
}
