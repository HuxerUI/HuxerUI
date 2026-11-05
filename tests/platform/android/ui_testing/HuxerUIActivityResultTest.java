package org.huxerui;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Looper;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.function.BooleanSupplier;

/** Exercises the Java presentation queue and the real C++ window factory/FilePicker bridge. */
public final class HuxerUIActivityResultTest {
    private final Instrumentation instrumentation;
    private final ArrayList<HuxerUIView> views = new ArrayList<>();
    private static int disposed;

    private HuxerUIActivityResultTest(Instrumentation instrumentation) {
        this.instrumentation = instrumentation;
    }

    static void verify(Instrumentation instrumentation) throws Exception {
        HuxerUIActivityResultTest test = new HuxerUIActivityResultTest(instrumentation);
        test.verifyQueue();
        test.verifyNativeBridge();
    }

    private void main(Runnable action) {
        Throwable[] failure = new Throwable[1];
        instrumentation.runOnMainSync(() -> {
            try { action.run(); } catch (Throwable error) { failure[0] = error; }
        });
        if (failure[0] != null) throw new AssertionError("Main-thread regression", failure[0]);
    }

    private void idle() { instrumentation.waitForIdleSync(); }

    private void await(BooleanSupplier condition, String message) throws Exception {
        long deadline = System.nanoTime() + 5_000_000_000L;
        boolean[] complete = new boolean[1];
        do {
            idle();
            main(() -> complete[0] = condition.getAsBoolean());
            if (complete[0]) return;
            Thread.sleep(5);
        } while (System.nanoTime() < deadline);
        throw new AssertionError(message);
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Reply implements HuxerUIActivityResultLauncher.Callback {
        int results;
        int failures;
        int code;
        Intent data;
        @Override public void onResult(int code, Intent data) {
            check(Looper.myLooper() == Looper.getMainLooper(), "Results must run on the main thread");
            ++results;
            this.code = code;
            this.data = data;
        }
        @Override public void onFailure(RuntimeException error) {
            check(Looper.myLooper() == Looper.getMainLooper(), "Failures must run on the main thread");
            ++failures;
        }
    }

    private static final class Host implements HuxerUIView.ActivityResultHost {
        final ArrayList<Integer> codes = new ArrayList<>();
        final ArrayList<Intent> intents = new ArrayList<>();
        final ArrayList<Integer> cancellations = new ArrayList<>();
        HuxerUIView view;
        boolean fail;
        boolean replyOnCancel;
        @Override public void launch(Intent intent, int code) {
            codes.add(code);
            intents.add(intent);
            if (fail) throw new IllegalStateException("Test launch failure");
        }
        @Override public void cancel(int code) {
            cancellations.add(code);
            if (replyOnCancel) view.dispatchActivityResult(code, Activity.RESULT_CANCELED, null);
        }
        int lastCode() { return codes.get(codes.size() - 1); }
    }

    private Host newHost() {
        Host host = new Host();
        host.view = new HuxerUIView(instrumentation.getContext());
        host.view.setActivityResultHost(host);
        views.add(host.view);
        return host;
    }

    private void verifyQueue() throws Exception {
        Host[] hosts = new Host[2];
        Reply first = new Reply();
        Reply removed = new Reply();
        Reply next = new Reply();
        HuxerUIPlatformChannel.Cancellation[] cancel = new HuxerUIPlatformChannel.Cancellation[2];
        main(() -> {
            hosts[0] = newHost();
            hosts[1] = newHost();
            HuxerUIActivityResultLauncher launcher = hosts[0].view.getActivityResultLauncher();
            cancel[0] = launcher.launch(new Intent("first"), first);
            cancel[1] = launcher.launch(new Intent("removed"), removed);
            launcher.launch(new Intent("next"), next);
            cancel[1].cancel();
            cancel[1].cancel();
            check(hosts[0].codes.isEmpty() && first.results == 0, "Launch and result must not run inline");
        });
        idle();
        main(() -> {
            check(hosts[0].codes.isEmpty(), "A paused host must not present queued requests");
            hosts[0].view.setActivityResultResumed(true);
            hosts[1].view.setActivityResultResumed(true);
            hosts[1].view.getActivityResultLauncher().launch(new Intent("other-window"), new Reply());
        });
        await(() -> hosts[0].codes.size() == 1 && hosts[1].codes.size() == 1, "Independent windows did not launch");
        main(() -> {
            check(!hosts[0].codes.get(0).equals(hosts[1].codes.get(0)), "Window requests must not share identities");
            check(!hosts[0].view.dispatchActivityResult(hosts[1].lastCode(), 9, null), "Foreign result was consumed");
            cancel[0].cancel();
            cancel[0].cancel();
        });
        idle();
        Intent payload = new Intent("returned");
        main(() -> {
            check(hosts[0].codes.size() == 1 && hosts[0].cancellations.size() == 1,
                    "Cancellation must keep the active slot until native completion");
            check(hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), 51, null), "Canceled result not consumed");
        });
        await(() -> hosts[0].codes.size() == 2, "Cancellation did not release the completed presentation");
        main(() -> {
            check("next".equals(hosts[0].intents.get(1).getAction()), "Queued cancellation broke FIFO");
            check(!hosts[0].view.dispatchActivityResult(hosts[0].codes.get(0), 17, null), "Late result was consumed");
            hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), 42, payload);
            check(next.results == 0, "Result callback ran inline");
            check(!hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), 43, null), "Duplicate result was consumed");
        });
        await(() -> next.results == 1, "Result was not delivered");
        main(() -> check(next.code == 42 && next.data == payload && first.results == 0 && removed.results == 0,
                "Result payload or cancellation suppression changed"));

        Reply failed = new Reply();
        Reply close = new Reply();
        main(() -> {
            hosts[0].fail = true;
            hosts[0].view.getActivityResultLauncher().launch(new Intent("failure"), failed);
        });
        await(() -> failed.failures == 1, "Launch failure was not reported");
        main(() -> {
            hosts[0].fail = false;
            hosts[0].view.getActivityResultLauncher().launch(new Intent("close"), close);
        });
        await(() -> hosts[0].codes.size() == 4, "Launch failure left the queue occupied");
        HuxerUIActivityResultLauncher[] retired = new HuxerUIActivityResultLauncher[1];
        Reply unavailable = new Reply();
        main(() -> {
            retired[0] = hosts[0].view.getActivityResultLauncher();
            hosts[0].replyOnCancel = true;
            hosts[0].view.setActivityResultHost(null);
            retired[0].launch(new Intent("retired"), unavailable);
        });
        await(() -> close.failures == 1 && unavailable.failures == 1, "Host closure must fail old requests");
        main(() -> {
            check(close.results == 0, "A reentrant close result must not replace host failure");
            hosts[0].view.setActivityResultHost(hosts[0]);
            Bundle state = new Bundle();
            hosts[0].view.saveActivityResultState(state);
            HuxerUIView.restoreActivityResultState(state);
            hosts[0].view.getActivityResultLauncher().launch(new Intent("restored"), new Reply());
        });
        idle();
        main(() -> {
            check(hosts[0].codes.size() == 4, "Saved host must wait for resume");
            hosts[0].view.setActivityResultResumed(true);
        });
        await(() -> hosts[0].codes.size() == 5, "Restored host did not resume");
        Reply suppressed = new Reply();
        Reply canceledResult = new Reply();
        main(() -> {
            check(hosts[0].codes.get(4) > hosts[0].codes.get(3), "Restoration reused a retired identity");
            hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), Activity.RESULT_CANCELED, null);
            cancel[0] = hosts[0].view.getActivityResultLauncher().launch(new Intent("suppress"), suppressed);
            hosts[0].view.getActivityResultLauncher().launch(new Intent("system-canceled"), canceledResult);
        });
        await(() -> hosts[0].codes.size() == 6, "Delivery cancellation request did not launch");
        main(() -> {
            hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), Activity.RESULT_OK, payload);
            cancel[0].cancel();
        });
        await(() -> hosts[0].codes.size() == 7, "Completed cancellation stalled the queue");
        main(() -> hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), Activity.RESULT_CANCELED, null));
        await(() -> canceledResult.results == 1, "Platform cancellation must remain an ordinary result");
        main(() -> {
            check(suppressed.results == 0 && suppressed.failures == 0 && canceledResult.data == null
                    && canceledResult.code == Activity.RESULT_CANCELED, "Pending callback cancellation failed");
            for (HuxerUIView view : views) view.releaseWindow();
            views.clear();
        });
        idle();
        try {
            retired[0].launch(new Intent(), new Reply());
            throw new AssertionError("Off-thread launch must fail");
        } catch (IllegalStateException expected) {}
    }

    private void verifyNativeBridge() throws Exception {
        Host[] hosts = new Host[2];
        HuxerUIApplication[] application = new HuxerUIApplication[1];
        main(() -> {
            prepareNative();
            application[0] = HuxerUIApplication.initialize(instrumentation.getTargetContext());
            verifyApplicationNative();
        });
        try {
            await(() -> statusNative(-1, 0) == 1, "Application factory no longer works without UI");
            main(() -> {
                for (int index = 0; index < hosts.length; ++index) {
                    hosts[index] = newHost();
                    hosts[index].view.setTag(index + 1);
                    hosts[index].view.setActivityResultResumed(true);
                    attachNative(hosts[index].view);
                    invokeNative(index, false, "identity");
                }
            });
            await(() -> statusNative(0, 0) == 1 && statusNative(1, 0) == 1, "Window factories did not reply");
            main(() -> {
                check(statusNative(0, 2) == 100 && statusNative(1, 2) == 200, "Factory reused the first window");
                invokeNative(0, true, "identity");
                invokeNative(1, true, "identity");
            });
            await(() -> statusNative(0, 0) == 2 && statusNative(1, 0) == 2, "Configured factories did not reply");
            main(() -> {
                check(statusNative(0, 2) == 107 && statusNative(1, 2) == 207, "Factory options were not encoded");
                invokeNative(0, false, "launch");
            });
            await(() -> hosts[0].codes.size() == 1, "Module did not present");
            main(() -> { pickNative(0); pickNative(1); });
            await(() -> hosts[1].codes.size() == 1, "Another window's picker was blocked");
            main(() -> {
                check(hosts[0].codes.size() == 1, "FilePicker overlapped a module presentation");
                hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), 33, null);
                hosts[1].view.dispatchActivityResult(hosts[1].lastCode(), Activity.RESULT_CANCELED, null);
            });
            await(() -> hosts[0].codes.size() == 2 && statusNative(0, 0) == 3 && statusNative(1, 1) == 1,
                    "Mixed presentation completion failed");
            main(() -> {
                check(statusNative(0, 2) == 33, "Module result did not reach C++");
                check(Intent.ACTION_OPEN_DOCUMENT.equals(hosts[0].intents.get(1).getAction()),
                        "FilePicker did not use the shared route");
                cancelNative(0, true);
                invokeNative(0, false, "launch");
            });
            idle();
            main(() -> {
                check(hosts[0].codes.size() == 2, "Canceled FilePicker released its slot before native completion");
                hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), Activity.RESULT_CANCELED, null);
            });
            await(() -> hosts[0].codes.size() == 3, "Module did not follow the canceled picker");
            main(() -> {
                check(statusNative(0, 1) == 0, "Canceled coroutine received a picker result");
                cancelNative(0, false);
                pickNative(0);
            });
            idle();
            main(() -> {
                check(hosts[0].codes.size() == 3, "Channel cancellation released the native slot early");
                hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), 99, null);
            });
            await(() -> hosts[0].codes.size() == 4, "Picker did not follow the canceled module");
            main(() -> {
                check(statusNative(0, 0) == 3, "Canceled channel received a late result");
                hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), Activity.RESULT_CANCELED, null);
            });
            await(() -> statusNative(0, 1) == 1, "Picker did not recover after cancellation");
            main(() -> {
                hosts[0].view.releaseWindow();
                hosts[0].view.setActivityResultHost(null);
                hosts[0].view.setActivityResultHost(hosts[0]);
                attachNative(hosts[0].view);
                pickNative(0);
                pickNative(2);
            });
            await(() -> statusNative(0, 1) == 2 && hosts[0].codes.size() == 5,
                    "Retained FilePicker rebound or replacement picker failed");
            main(() -> hosts[0].view.dispatchActivityResult(hosts[0].lastCode(), Activity.RESULT_CANCELED, null));
            await(() -> statusNative(2, 1) == 1, "Replacement picker result was lost");
        } finally {
            main(HuxerUIActivityResultTest::closeNative);
            idle();
            main(() -> {
                check(disposed == 7, "Each Java module must be disposed exactly once");
                for (HuxerUIView view : views) view.releaseWindow();
                views.clear();
                application[0].requestShutdown();
            });
            idle();
            main(HuxerUIActivityResultTest::finishNative);
        }
    }

    // Create real native windows without requiring a visible Activity or scheduling renderer frames.
    private static void attachNative(HuxerUIView view) {
        try {
            Method create = HuxerUIView.class.getDeclaredMethod("nativeCreate", HuxerUIView.class, int.class);
            create.setAccessible(true);
            Field handle = HuxerUIView.class.getDeclaredField("nativeHandle");
            handle.setAccessible(true);
            handle.setLong(view, (long) create.invoke(null, view, 0));
        } catch (ReflectiveOperationException error) {
            throw new AssertionError("Cannot create native test window", error);
        }
    }

    public static final class Factory implements HuxerUIPlatformModule.Factory, HuxerUIPlatformModule.WindowFactory {
        @Override public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                HuxerUIPlatformChannel.Events events) {
            check(context == context.getApplicationContext(), "Application factory must receive application Context");
            return instance(null, 5, events);
        }
        @Override public HuxerUIPlatformModule create(HuxerUIView window, PlatformPayload options,
                HuxerUIPlatformChannel.Events events) {
            HuxerUIActivityResultLauncher launcher = window.getActivityResultLauncher();
            long identity = ((Integer) window.getTag()) * 100L + (options.isNull() ? 0 : options.requireInt64());
            return instance(launcher, identity, events);
        }
        private HuxerUIPlatformModule instance(HuxerUIActivityResultLauncher launcher, long identity,
                HuxerUIPlatformChannel.Events events) {
            return new HuxerUIPlatformModule() {
                @Override public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
                        HuxerUIPlatformChannel.Result result) {
                    if (method.equals("identity")) {
                        result.complete(PlatformPayload.int64(identity));
                        return null;
                    }
                    HuxerUIPlatformChannel.Cancellation request = launcher.launch(new Intent("module"),
                            new HuxerUIActivityResultLauncher.Callback() {
                                @Override public void onResult(int code, Intent data) {
                                    result.complete(PlatformPayload.int64(code));
                                }
                                @Override public void onFailure(RuntimeException error) {
                                    result.fail("host-closed", error.getMessage(), PlatformPayload.nullValue());
                                }
                            });
                    return () -> { request.cancel(); result.close(); };
                }
                @Override public void dispose() { ++disposed; events.close(); }
            };
        }
    }

    private static native void prepareNative();
    private static native void verifyApplicationNative();
    private static native void invokeNative(int window, boolean configured, String method);
    private static native void pickNative(int window);
    private static native void cancelNative(int window, boolean picker);
    private static native long statusNative(int window, int field);
    private static native void closeNative();
    private static native void finishNative();
}
