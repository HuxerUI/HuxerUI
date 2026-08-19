package org.huxerui;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
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
    private static final int RESULT_RESPONSE = 0;
    private static final int RESULT_TRANSPORT_ERROR = 1;
    private static final int RESULT_TIMEOUT = 2;
    private static final int RESULT_CANCELED = 3;
    private static final int WORKER_COUNT = 4;
    private static final int QUEUE_CAPACITY = 64;
    private static final int BUFFER_SIZE = 16 * 1024;

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

    private volatile HttpURLConnection connection;
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
        HttpURLConnection activeConnection = null;
        try {
            activeConnection = (HttpURLConnection) new URL(url).openConnection();
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
                activeConnection.setFixedLengthStreamingMode((long) body.length);
                try (OutputStream output = activeConnection.getOutputStream()) {
                    output.write(body);
                }
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

            InputStream input =
                    statusCode >= 400 ? activeConnection.getErrorStream() : activeConnection.getInputStream();
            byte[] responseBody = input == null ? new byte[0] : readBody(input);
            finishResponse(activeConnection.getURL().toString(), statusCode, responseHeaderNames.toArray(new String[0]),
                    responseHeaderValues.toArray(new String[0]), responseBody);
        } catch (SocketTimeoutException ignored) {
            finishTimeout();
        } catch (Exception exception) {
            finishTransportError(exception);
        } finally {
            connection = null;
            if (activeConnection != null) {
                activeConnection.disconnect();
            }
        }
    }

    void cancel() {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelPlatformWork();
        nativeComplete(nativeHandle, RESULT_CANCELED, null, 0, null, null, null, null);
    }

    private void finishResponse(String responseUrl, int statusCode, String[] responseHeaderNames,
            String[] responseHeaderValues, byte[] responseBody) {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelTimeout();
        nativeComplete(nativeHandle, RESULT_RESPONSE, responseUrl, statusCode, responseHeaderNames,
                responseHeaderValues, responseBody, null);
    }

    private void finishTransportError(Exception exception) {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelTimeout();
        String detail = exception.getLocalizedMessage();
        String message = detail == null || detail.isEmpty() ? "HuxerUI HTTP request failed"
                                                            : "HuxerUI HTTP request failed: " + detail;
        nativeComplete(nativeHandle, RESULT_TRANSPORT_ERROR, null, 0, null, null, null, message);
    }

    private void finishStartError(Exception exception) {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelPlatformWork();
        String detail = exception.getLocalizedMessage();
        String message = detail == null || detail.isEmpty() ? "HuxerUI HTTP request failed"
                                                            : "HuxerUI HTTP request failed: " + detail;
        nativeComplete(nativeHandle, RESULT_TRANSPORT_ERROR, null, 0, null, null, null, message);
    }

    private void finishTimeout() {
        if (!finished.compareAndSet(false, true)) {
            return;
        }
        cancelPlatformWork();
        nativeComplete(nativeHandle, RESULT_TIMEOUT, null, 0, null, null, null, "HuxerUI HTTP request timed out");
    }

    private void cancelPlatformWork() {
        cancelTimeout();
        Future<?> activeWorker = worker;
        if (activeWorker != null) {
            activeWorker.cancel(true);
        }
        HttpURLConnection activeConnection = connection;
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

    private static byte[] readBody(InputStream input) throws IOException {
        try (InputStream source = input; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[BUFFER_SIZE];
            int count;
            while ((count = source.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        }
    }

    private static native void nativeComplete(long nativeHandle, int result, String url, int statusCode,
            String[] headerNames, String[] headerValues, byte[] body, String message);
}
