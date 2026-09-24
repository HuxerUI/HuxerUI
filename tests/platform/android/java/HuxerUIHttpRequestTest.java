package org.huxerui;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;
import java.net.URLStreamHandler;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises the production Java request and its JNI callbacks without an Android UI or a live network. */
public final class HuxerUIHttpRequestTest {
    private static final Map<Long, Fixture> fixtures = new ConcurrentHashMap<>();
    private static long nextHandle;

    private static final class Fixture {
        final long handle = ++nextHandle;
        final byte[] expected = new byte[256 * 1024];
        final ByteArrayOutputStream received = new ByteArrayOutputStream();
        final CountDownLatch headers = new CountDownLatch(1);
        final CountDownLatch terminal = new CountDownLatch(1);
        final CountDownLatch readEntered = new CountDownLatch(1);
        final CountDownLatch inputEntered = new CountDownLatch(1);
        final CountDownLatch bodyEntered = new CountDownLatch(1);
        final CountDownLatch closed = new CountDownLatch(1);
        final AtomicInteger callbackDepth = new AtomicInteger();
        final AtomicInteger reads = new AtomicInteger();
        final AtomicInteger terminals = new AtomicInteger();
        final AtomicReference<String> failure = new AtomicReference<>();
        final HuxerUIHttpRequest request;
        volatile CountDownLatch readGate;
        volatile CountDownLatch inputGate;
        volatile CountDownLatch bodyGate;
        volatile boolean automatic;
        volatile boolean crossThread;
        volatile boolean disconnected;
        volatile int result = -1;
        int offset;

        Fixture(long timeout) {
            for (int i = 0; i < expected.length; ++i) expected[i] = (byte) ((i / 4) >>> ((i % 4) * 8));
            fixtures.put(handle, this);
            request = new HuxerUIHttpRequest(handle, "http://fixture/" + handle, "GET", new String[0],
                    new String[0], new byte[0], timeout);
        }

        final InputStream input = new InputStream() {
            @Override public int read() { throw new AssertionError("Expected a bulk read"); }

            @Override public int read(byte[] buffer, int start, int length) throws IOException {
                reads.incrementAndGet();
                if (callbackDepth.get() != 0) failure.compareAndSet(null, "Read overtook the preceding callback");
                readEntered.countDown();
                awaitUninterruptibly(readGate);
                if (closed.getCount() == 0) throw new IOException("Closed test stream");
                if (offset == expected.length) return -1;
                int count = Math.min(Math.min(length, 1024), expected.length - offset);
                System.arraycopy(expected, offset, buffer, start, count);
                offset += count;
                return count;
            }

            @Override public void close() {
                closed.countDown();
                if (readGate != null) readGate.countDown();
            }
        };

        URLConnection connection(URL url) {
            return new HttpURLConnection(url) {
                @Override public void connect() {}
                @Override public boolean usingProxy() { return false; }
                @Override public void disconnect() { disconnected = true; }
                @Override public int getResponseCode() { return 200; }
                @Override public Map<String, List<String>> getHeaderFields() { return Collections.emptyMap(); }
                @Override public String getHeaderField(String name) {
                    return name.equals("Content-Length") ? Integer.toString(expected.length) : null;
                }
                @Override public InputStream getInputStream() {
                    inputEntered.countDown();
                    awaitUninterruptibly(inputGate);
                    return input;
                }
            };
        }

        void demand() {
            if (!automatic) return;
            if (!crossThread) {
                request.read();
                return;
            }
            CountDownLatch returned = new CountDownLatch(1);
            Thread caller = new Thread(() -> {
                request.read();
                returned.countDown();
            });
            caller.start();
            try {
                check(returned.await(2, TimeUnit.SECONDS), "Read demand blocked inside a callback");
            } catch (InterruptedException exception) {
                failure.compareAndSet(null, "Interrupted demand");
            }
        }
    }

    public static void onResponse(long handle) {
        Fixture fixture = fixtures.get(handle);
        fixture.callbackDepth.incrementAndGet();
        try {
            fixture.headers.countDown();
            fixture.demand();
        } finally {
            fixture.callbackDepth.decrementAndGet();
        }
    }

    public static void onBody(long handle, byte[] bytes) {
        Fixture fixture = fixtures.get(handle);
        if (fixture.callbackDepth.incrementAndGet() != 1) fixture.failure.set("Overlapping callbacks");
        try {
            fixture.received.write(bytes, 0, bytes.length);
            fixture.demand();
            fixture.bodyEntered.countDown();
            awaitUninterruptibly(fixture.bodyGate);
        } finally {
            fixture.callbackDepth.decrementAndGet();
        }
    }

    public static void onTerminal(long handle, int result) {
        Fixture fixture = fixtures.get(handle);
        fixture.result = result;
        fixture.terminals.incrementAndGet();
        fixture.terminal.countDown();
    }

    private static void awaitUninterruptibly(CountDownLatch gate) {
        if (gate == null) return;
        boolean interrupted = false;
        for (;;) {
            try {
                if (!gate.await(5, TimeUnit.SECONDS)) throw new AssertionError("Test gate timed out");
                break;
            } catch (InterruptedException ignored) {
                interrupted = true;
            }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void await(CountDownLatch latch, String message) throws InterruptedException {
        check(latch.await(3, TimeUnit.SECONDS), message);
    }

    private static void verifyReads(boolean crossThread) throws Exception {
        Fixture fixture = new Fixture(0);
        fixture.automatic = true;
        fixture.crossThread = crossThread;
        try {
            fixture.request.start();
            await(fixture.terminal, "Read demand was lost before EOF");
            check(fixture.result == 0, "Unexpected terminal result");
            check(fixture.failure.get() == null, fixture.failure.get());
            check(Arrays.equals(fixture.expected, fixture.received.toByteArray()), "Bytes reordered or lost");
            check(fixture.terminals.get() == 1, "Duplicate terminal callback");
        } finally {
            fixture.request.cancel();
        }
    }

    private static void verifyIdleTermination(boolean timeout) throws Exception {
        Fixture fixture = new Fixture(timeout ? 100 : 0);
        fixture.request.start();
        await(fixture.headers, "Missing headers");
        if (!timeout) fixture.request.cancel();
        await(fixture.terminal, "Idle request did not terminate");
        fixture.request.read();
        fixture.request.cancel();
        check(fixture.result == (timeout ? 2 : 3), "Wrong idle terminal result");
        check(fixture.reads.get() == 0, "Idle request read without demand");
        check(fixture.closed.getCount() == 0 && fixture.disconnected, "Idle resources leaked");
        check(fixture.terminals.get() == 1, "Duplicate terminal callback");
    }

    private static void verifyCancelDuringRead() throws Exception {
        Fixture fixture = new Fixture(0);
        fixture.readGate = new CountDownLatch(1);
        fixture.request.start();
        await(fixture.headers, "Missing headers");
        fixture.request.read();
        await(fixture.readEntered, "Read did not start");
        fixture.request.cancel();
        await(fixture.terminal, "Active read did not cancel");
        check(fixture.result == 3 && fixture.closed.getCount() == 0, "Active read resources leaked");
        check(fixture.received.size() == 0, "Canceled read published data");
    }

    private static void verifyLateInput() throws Exception {
        Fixture fixture = new Fixture(0);
        fixture.inputGate = new CountDownLatch(1);
        fixture.request.start();
        await(fixture.inputEntered, "Input acquisition did not start");
        fixture.request.cancel();
        fixture.inputGate.countDown();
        await(fixture.closed, "Input acquired after cancellation leaked");
        check(fixture.result == 3 && fixture.terminals.get() == 1, "Cancellation was not terminal");
    }

    private static ThreadPoolExecutor executor() throws Exception {
        Field field = HuxerUIHttpRequest.class.getDeclaredField("executor");
        field.setAccessible(true);
        return (ThreadPoolExecutor) field.get(null);
    }

    private static void verifyCancelDuringPublication() throws Exception {
        Fixture fixture = new Fixture(0);
        fixture.automatic = true;
        fixture.bodyGate = new CountDownLatch(1);
        fixture.request.start();
        await(fixture.bodyEntered, "Body publication did not start");
        Thread cancel = new Thread(fixture.request::cancel);
        cancel.start();
        try {
            await(fixture.closed, "Cancel did not retire the active input");
        } finally {
            fixture.bodyGate.countDown();
            cancel.join(3000);
        }
        await(fixture.terminal, "Cancel did not finish after publication");
        check(fixture.reads.get() == 1, "Pending demand restarted after cancellation");
        check(fixture.result == 3 && fixture.terminals.get() == 1, "Wrong cancellation result");
    }

    private static void verifyRejectedRead() throws Exception {
        Fixture fixture = new Fixture(0);
        fixture.automatic = true;
        fixture.bodyGate = new CountDownLatch(1);
        CountDownLatch entered = new CountDownLatch(3);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch drained = new CountDownLatch(64);
        ThreadPoolExecutor executor = executor();
        try {
            fixture.request.start();
            await(fixture.bodyEntered, "Body publication did not start");
            for (int i = 0; i < 3; ++i) executor.execute(() -> {
                entered.countDown();
                awaitUninterruptibly(release);
            });
            await(entered, "Workers did not start");
            for (int i = 0; i < 64; ++i) executor.execute(drained::countDown);
            fixture.bodyGate.countDown();
            await(fixture.terminal, "Rejected read handoff did not terminate");
            check(fixture.result == 1 && fixture.terminals.get() == 1, "Wrong read rejection result");
            check(fixture.reads.get() == 1 && fixture.closed.getCount() == 0, "Rejected read leaked resources");
        } finally {
            fixture.bodyGate.countDown();
            release.countDown();
            fixture.request.cancel();
            await(drained, "Queued work did not drain");
        }
    }

    private static void verifyRejectedStart() throws Exception {
        ThreadPoolExecutor executor = executor();
        CountDownLatch entered = new CountDownLatch(4);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch drained = new CountDownLatch(64);
        try {
            for (int i = 0; i < 4; ++i) executor.execute(() -> {
                entered.countDown();
                awaitUninterruptibly(release);
            });
            await(entered, "Workers did not start");
            for (int i = 0; i < 64; ++i) executor.execute(drained::countDown);
            Fixture fixture = new Fixture(0);
            fixture.request.start();
            await(fixture.terminal, "Rejected request did not terminate");
            check(fixture.result == 1 && fixture.terminals.get() == 1, "Wrong rejection result");
        } finally {
            release.countDown();
            await(drained, "Queued work did not drain");
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        URL.setURLStreamHandlerFactory(protocol -> protocol.equals("http") ? new URLStreamHandler() {
            @Override protected URLConnection openConnection(URL url) {
                return fixtures.get(Long.parseLong(url.getPath().substring(1))).connection(url);
            }
        } : null);
        int failures = 0;
        try {
            for (String test : new String[] {"reentrant", "cross-thread", "idle-cancel", "idle-timeout",
                    "active-cancel", "late-input", "publication-cancel", "rejected-read", "rejected-start"}) {
                try {
                    switch (test) {
                        case "reentrant": verifyReads(false); break;
                        case "cross-thread": verifyReads(true); break;
                        case "idle-cancel": verifyIdleTermination(false); break;
                        case "idle-timeout": verifyIdleTermination(true); break;
                        case "active-cancel": verifyCancelDuringRead(); break;
                        case "late-input": verifyLateInput(); break;
                        case "publication-cancel": verifyCancelDuringPublication(); break;
                        case "rejected-read": verifyRejectedRead(); break;
                        case "rejected-start": verifyRejectedStart(); break;
                        default: throw new AssertionError(test);
                    }
                    System.out.println("PASS " + test);
                } catch (Throwable error) {
                    ++failures;
                    System.err.println("FAIL " + test + ": " + error);
                }
            }
        } finally {
            for (Fixture fixture : fixtures.values()) fixture.request.cancel();
            executor().shutdownNow();
            Field field = HuxerUIHttpRequest.class.getDeclaredField("timeoutExecutor");
            field.setAccessible(true);
            ((ScheduledThreadPoolExecutor) field.get(null)).shutdownNow();
        }
        if (failures != 0) throw new AssertionError(failures + " HTTP regression checks failed");
    }
}
