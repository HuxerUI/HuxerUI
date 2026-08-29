package org.huxerui;

import android.content.Context;

/**
 * Implements one nonvisual platform instance created through {@code JavaPlatformModuleFactory}.
 *
 * <p>A library normally keeps this Java interface behind a strongly typed C++ service. HuxerUI creates the Java
 * instance on the Android UI thread, forwards method calls through {@link HuxerUIPlatformChannel}, and calls
 * {@link #dispose()} after requests and events have been detached.</p>
 *
 * <pre>{@code
 * public final class PlatformTimer implements HuxerUIPlatformModule.Factory {
 *     @Override
 *     public HuxerUIPlatformModule create(
 *             Context context, PlatformPayload options, HuxerUIPlatformChannel.Events events) {
 *         return new TimerInstance(events);
 *     }
 * }
 * }</pre>
 */
public interface HuxerUIPlatformModule {
    /** Creates a fresh Java platform instance for one C++ PlatformModule open operation. */
    interface Factory {
        /**
         * Creates the platform instance.
         *
         * @param context the HuxerUI host Context; retain only through ordinary Android ownership rules
         * @param options the immutable construction options encoded by the C++ Module factory
         * @param events the framework-owned event sink; close or stop retaining it during disposal
         * @return a new platform instance owned by HuxerUI
         */
        HuxerUIPlatformModule create(Context context, PlatformPayload options, HuxerUIPlatformChannel.Events events);
    }

    /**
     * Handles one asynchronous method invocation.
     *
     * <p>Complete or fail {@code result} at most once. Return a cancellation operation when the work can be cancelled,
     * or {@code null} otherwise. HuxerUI ignores late results after cancellation or disposal.</p>
     */
    HuxerUIPlatformChannel.Cancellation invoke(
            String method, PlatformPayload arguments, HuxerUIPlatformChannel.Result result);

    /** Releases listeners, platform resources, and background work owned by this instance. */
    void dispose();
}
