package org.huxerui;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class HuxerUIHttpRequest implements Runnable {
    private static final int RESULT_COMPLETE = 0;
    private static final int RESULT_TRANSPORT_ERROR = 1;
    private static final int RESULT_TIMEOUT = 2;
    private static final int RESULT_CANCELED = 3;
    private static final int WORKER_COUNT = 4;
    private static final int QUEUE_CAPACITY = 64;
    private static final int BUFFER_SIZE = 64 * 1024;

    private static final ThreadPoolExecutor executor = new ThreadPoolExecutor(
            WORKER_COUNT, WORKER_COUNT, 30L, TimeUnit.SECONDS, new LinkedBlockingQueue<>(QUEUE_CAPACITY));
    private static final ScheduledThreadPoolExecutor timeoutExecutor = new ScheduledThreadPoolExecutor(1);

    static {
        executor.allowCoreThreadTimeOut(true);
        timeoutExecutor.setKeepAliveTime(30L, TimeUnit.SECONDS);
        timeoutExecutor.allowCoreThreadTimeOut(true);
        timeoutExecutor.setRemoveOnCancelPolicy(true);
    }

    private final long nativeHandle;
    private final String url;
    private final String method;
    private final String[] headerNames;
    private final String[] headerValues;
    private final byte[] body;
    private final long timeoutMillis;
    private final AtomicBoolean finished = new AtomicBoolean();
    private final AtomicBoolean readPending = new AtomicBoolean();
    private final Object callbackLock = new Object();

    private volatile HttpURLConnection connection;
    private volatile InputStream input;
    private volatile Future<?> worker;
    private volatile ScheduledFuture<?> timeout;

    HuxerUIHttpRequest(long nativeHandle, String url, String method, String[] headerNames, String[] headerValues,
            byte[] body, long timeoutMillis) {
        this.nativeHandle = nativeHandle;
        this.url = url;
        this.method = method;
        this.headerNames = headerNames;
        this.headerValues = headerValues;
        this.body = body;
        this.timeoutMillis = timeoutMillis;
    }

    void start() {
        try {
            worker = executor.submit(this);
            if (timeoutMillis > 0L) {
                ScheduledFuture<?> deadline =
                        timeoutExecutor.schedule(this::finishTimeout, timeoutMillis, TimeUnit.MILLISECONDS);
                timeout = deadline;
                if (finished.get()) {
                    deadline.cancel(false);
                }
            }
        } catch (RuntimeException exception) {
            finishStartError(exception);
        }
    }

    @Override
    public void run() {
        try {
            HttpURLConnection activeConnection = (HttpURLConnection) new URL(url).openConnection();
            connection = activeConnection;
            if (finished.get()) {
                return;
            }

            activeConnection.setRequestMethod(method);
            activeConnection.setInstanceFollowRedirects(true);
            if (timeoutMillis > 0L) {
                int nativeTimeout = (int) Math.min(timeoutMillis, Integer.MAX_VALUE);
                activeConnection.setConnectTimeout(nativeTimeout);
                activeConnection.setReadTimeout(nativeTimeout);
            }
            for (int index = 0; index < headerNames.length; ++index) {
                activeConnection.addRequestProperty(headerNames[index], headerValues[index]);
            }
            if (body.length != 0) {
                activeConnection.setDoOutput(true);
                try (OutputStream output = activeConnection.getOutputStream()) {
                    output.write(body);
                }
                publishUpload(body.length);
            }

            int statusCode = activeConnection.getResponseCode();
            List<String> responseHeaderNames = new ArrayList<>();
            List<String> responseHeaderValues = new ArrayList<>();
            for (Map.Entry<String, List<String>> header : activeConnection.getHeaderFields().entrySet()) {
                if (header.getKey() == null || header.getValue() == null) {
                    continue;
                }
                for (String value : header.getValue()) {
                    if (value != null) {
                        responseHeaderNames.add(header.getKey());
                        responseHeaderValues.add(value);
                    }
                }
            }

            input = statusCode >= 400 ? activeConnection.getErrorStream() : activeConnection.getInputStream();
            long bodySize = reliableBodySize(activeConnection, statusCode);
            publishResponse(activeConnection.getURL().toString(), statusCode,
                    responseHeaderNames.toArray(new String[0]), responseHeaderValues.toArray(new String[0]), bodySize);
        } catch (SocketTimeoutException ignored) {
            finishTimeout();
        } catch (Exception exception) {
            finishTransportError(exception);
        }
    }

    void read() {
        if (finished.get() || !readPending.compareAndSet(false, true)) {
            return;
        }
        try {
            worker = executor.submit(this::readChunk);
        } catch (RuntimeException exception) {
            readPending.set(false);
            finishStartError(exception);
        }
    }

    void cancel() {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelPlatformWork();
        publishTerminal(RESULT_CANCELED, null);
    }

    private void readChunk() {
        try {
            InputStream source = input;
            if (source == null) {
                finishComplete();
                return;
            }
            byte[] buffer = new byte[BUFFER_SIZE];
            int count;
            do {
                count = source.read(buffer);
            } while (count == 0 && !finished.get());
            if (count < 0) {
                finishComplete();
                return;
            }
            if (finished.get()) {
                return;
            }
            publishBody(count == buffer.length ? buffer : Arrays.copyOf(buffer, count));
        } catch (SocketTimeoutException ignored) {
            finishTimeout();
        } catch (Exception exception) {
            finishTransportError(exception);
        } finally {
            readPending.set(false);
        }
    }

    private void publishUpload(long transferredBytes) {
        synchronized (callbackLock) {
            if (!finished.get()) {
                nativeUpload(nativeHandle, transferredBytes);
            }
        }
    }

    private void publishResponse(String responseUrl, int statusCode, String[] responseHeaderNames,
            String[] responseHeaderValues, long bodySize) {
        synchronized (callbackLock) {
            if (!finished.get()) {
                nativeResponse(nativeHandle, responseUrl, statusCode, responseHeaderNames, responseHeaderValues,
                        bodySize);
            }
        }
    }

    private void publishBody(byte[] bytes) {
        synchronized (callbackLock) {
            if (!finished.get()) {
                nativeBody(nativeHandle, bytes);
            }
        }
    }

    private void finishComplete() {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        closePlatformWork();
        publishTerminal(RESULT_COMPLETE, null);
    }

    private void finishTransportError(Exception exception) {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        closePlatformWork();
        String detail = exception.getLocalizedMessage();
        String message = detail == null || detail.isEmpty() ? "HuxerUI HTTP request failed"
                                                            : "HuxerUI HTTP request failed: " + detail;
        publishTerminal(RESULT_TRANSPORT_ERROR, message);
    }

    private void finishStartError(Exception exception) {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelPlatformWork();
        String detail = exception.getLocalizedMessage();
        String message = detail == null || detail.isEmpty() ? "HuxerUI HTTP request failed"
                                                            : "HuxerUI HTTP request failed: " + detail;
        publishTerminal(RESULT_TRANSPORT_ERROR, message);
    }

    private void finishTimeout() {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelPlatformWork();
        publishTerminal(RESULT_TIMEOUT, "HuxerUI HTTP request timed out");
    }

    private void publishTerminal(int result, String message) {
        synchronized (callbackLock) {
            nativeTerminal(nativeHandle, result, message);
        }
    }

    private void cancelPlatformWork() {
        Future<?> activeWorker = worker;
        if (activeWorker != null) {
            activeWorker.cancel(true);
        }
        closePlatformWork();
    }

    private void closePlatformWork() {
        cancelTimeout();
        InputStream activeInput = input;
        input = null;
        if (activeInput != null) {
            try {
                activeInput.close();
            } catch (IOException ignored) {
            }
        }
        HttpURLConnection activeConnection = connection;
        connection = null;
        if (activeConnection != null) {
            activeConnection.disconnect();
        }
    }

    private void cancelTimeout() {
        ScheduledFuture<?> activeTimeout = timeout;
        if (activeTimeout != null) {
            activeTimeout.cancel(false);
        }
    }

    private long reliableBodySize(HttpURLConnection activeConnection, int statusCode) {
        if (method.equals("HEAD") || (statusCode >= 100 && statusCode < 200) || statusCode == 204 ||
                statusCode == 304) {
            return 0L;
        }
        if (activeConnection.getContentEncoding() != null ||
                activeConnection.getHeaderField("Transfer-Encoding") != null) {
            return -1L;
        }
        String contentLength = activeConnection.getHeaderField("Content-Length");
        if (contentLength == null) {
            return -1L;
        }
        try {
            long parsed = Long.parseLong(contentLength);
            return parsed >= 0L ? parsed : -1L;
        } catch (NumberFormatException ignored) {
            return -1L;
        }
    }

    private static native void nativeUpload(long nativeHandle, long transferredBytes);

    private static native void nativeResponse(long nativeHandle, String url, int statusCode, String[] headerNames,
            String[] headerValues, long bodySize);

    private static native void nativeBody(long nativeHandle, byte[] body);

    private static native void nativeTerminal(long nativeHandle, int result, String message);
}
