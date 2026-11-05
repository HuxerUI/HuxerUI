package org.huxerui;

import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayDeque;
import java.util.Objects;

/**
 * Launches result-producing Intents through one HuxerUI window attachment's presentation queue.
 * Retain the launcher obtained during module creation, rather than looking up a replacement window for each call.
 * The launcher stops retaining its Activity endpoint when the attachment or endpoint closes.
 */
public final class HuxerUIActivityResultLauncher {
    /** Receives at most one result or failure, asynchronously on the Android main thread. */
    public interface Callback {
        /** Receives the unchanged platform code and nullable Intent; RESULT_CANCELED is an ordinary result. */
        void onResult(int resultCode, Intent data);

        /** Reports an unavailable/closed host or a launch failure, independently of the platform result code. */
        void onFailure(RuntimeException error);
    }

    private static final String STATE_KEY = "org.huxerui.activityResult.nextRequestCode";
    // Keep framework result codes separate from permission codes and never reuse a retired request's identity.
    private static final int FIRST_REQUEST_CODE = 0x40000000;
    private static int nextRequestCode = FIRST_REQUEST_CODE;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ArrayDeque<Request> queued = new ArrayDeque<>();
    private HuxerUIView.ActivityResultHost host;
    private Request active;
    private boolean resumed;
    private boolean closed;
    private boolean drainPosted;

    HuxerUIActivityResultLauncher(HuxerUIView.ActivityResultHost host) {
        this.host = host;
    }

    /**
     * Enqueues an Intent on the main thread and returns a non-null, idempotent main-thread cancellation operation.
     * Configure the non-null Intent before calling and do not mutate it until the request ends. Only protocols
     * defined to return an Activity result are supported. The SDK retains Java objects without exposing request codes.
     * Requests share FilePicker's native presentation queue in admission order; paused hosts wait to resume.
     * Cancellation suppresses callback delivery and requests native dismissal, but an active slot remains occupied
     * until its result arrives or the host closes. A closed launcher never binds to another attachment.
     * Invalid arguments or an off-thread call throw synchronously; host and launch failures use onFailure.
     */
    public HuxerUIPlatformChannel.Cancellation launch(Intent intent, Callback callback) {
        return enqueue(intent, callback, null);
    }

    // Transports may need a terminal cleanup even after delivery is canceled, e.g. to release a native holder.
    HuxerUIPlatformChannel.Cancellation enqueue(Intent intent, Callback callback, Runnable canceledCompletion) {
        requireThread();
        Request request = new Request(Objects.requireNonNull(intent), Objects.requireNonNull(callback),
                canceledCompletion);
        if (!available()) {
            request.finish(0, null, new IllegalStateException("HuxerUI Android Activity result host is unavailable"));
        } else {
            queued.addLast(request);
            scheduleDrain();
        }
        return request;
    }

    static void requireThread() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("HuxerUI Activity results require the application thread");
        }
    }

    static void saveState(Bundle state) {
        requireThread();
        state.putInt(STATE_KEY, nextRequestCode);
    }

    static void restoreState(Bundle state) {
        requireThread();
        if (state != null) nextRequestCode = Math.max(nextRequestCode, state.getInt(STATE_KEY, FIRST_REQUEST_CODE));
    }

    boolean available() { return !closed && host != null; }
    boolean closed() { return closed; }

    void setResumed(boolean value) {
        requireThread();
        resumed = value;
        scheduleDrain();
    }

    boolean dispatchResult(int requestCode, int resultCode, Intent data) {
        requireThread();
        if (closed || active == null || active.requestCode != requestCode) return false;
        active.finish(resultCode, data, null);
        return true;
    }

    void close() {
        requireThread();
        if (closed) return;
        closed = true;
        resumed = false;
        HuxerUIView.ActivityResultHost previous = host;
        host = null;
        RuntimeException error = new IllegalStateException("HuxerUI Android Activity result host closed");
        if (active != null) {
            Request request = active;
            try { previous.cancel(request.requestCode); } catch (RuntimeException ignored) {}
            request.finish(0, null, error);
        }
        while (!queued.isEmpty()) queued.peekFirst().finish(0, null, error);
    }

    private void scheduleDrain() {
        if (drainPosted || !available() || !resumed || active != null || queued.isEmpty()) return;
        drainPosted = true;
        handler.post(() -> {
            drainPosted = false;
            if (!available() || !resumed || active != null || queued.isEmpty()) return;
            Request request = queued.removeFirst();
            if (nextRequestCode == Integer.MAX_VALUE) {
                request.finish(0, null, new IllegalStateException("HuxerUI Android Activity identities exhausted"));
                return;
            }
            request.requestCode = nextRequestCode++;
            active = request;
            Intent intent = request.intent;
            request.intent = null;
            try {
                host.launch(intent, request.requestCode);
            } catch (RuntimeException error) {
                request.finish(0, null, error);
            }
        });
    }

    private final class Request implements HuxerUIPlatformChannel.Cancellation {
        private Intent intent;
        private Callback callback;
        private Runnable canceledCompletion;
        private int requestCode;
        private boolean canceled;
        private boolean finished;
        private boolean delivered;

        Request(Intent intent, Callback callback, Runnable canceledCompletion) {
            this.intent = intent;
            this.callback = callback;
            this.canceledCompletion = canceledCompletion;
        }

        @Override
        public void cancel() {
            requireThread();
            if (delivered || canceled) return;
            canceled = true;
            callback = null;
            if (finished) return;
            if (active == this) {
                // finishActivity requests dismissal; only a matching result or host closure releases the slot.
                try { host.cancel(requestCode); } catch (RuntimeException ignored) {}
            } else {
                finish(0, null, null);
            }
        }

        void finish(int code, Intent data, RuntimeException error) {
            if (finished) return;
            finished = true;
            intent = null;
            queued.remove(this);
            if (active == this) active = null;
            handler.post(() -> {
                delivered = true;
                Callback receiver = callback;
                Runnable cleanup = canceledCompletion;
                callback = null;
                canceledCompletion = null;
                if (canceled) {
                    if (cleanup != null) cleanup.run();
                } else if (error != null) {
                    receiver.onFailure(error);
                } else {
                    receiver.onResult(code, data);
                }
            });
            scheduleDrain();
        }
    }
}
