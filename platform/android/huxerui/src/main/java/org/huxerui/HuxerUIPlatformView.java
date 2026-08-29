package org.huxerui;

import android.content.Context;
import android.view.View;

/**
 * Implements one Android View embedded by a Java-backed HuxerUI PlatformView factory.
 *
 * <p>The Java instance owns the stable Android {@link View} and any listeners or auxiliary state. Declarative
 * properties arrive through {@link #update(PlatformPayload)}, while optional Controller commands arrive through
 * {@link #invoke(String, PlatformPayload, HuxerUIPlatformChannel.Result)}. All methods run on the Android UI
 * thread.</p>
 *
 * <pre>{@code
 * public final class PlatformTextField implements HuxerUIPlatformView.Factory {
 *     @Override
 *     public HuxerUIPlatformView create(
 *             Context context, PlatformPayload properties, HuxerUIPlatformChannel.Events events) {
 *         return new TextFieldInstance(context, properties, events);
 *     }
 * }
 * }</pre>
 */
public interface HuxerUIPlatformView {
    /** Creates a fresh Java instance for one mounted HuxerUI PlatformView. */
    interface Factory {
        /**
         * Creates the platform instance and its detached Android View.
         *
         * @param context the HuxerUI host Context
         * @param properties the complete initial controlled properties
         * @param events the framework-owned event sink for this mounted View
         * @return a new instance owned by HuxerUI
         */
        HuxerUIPlatformView create(Context context, PlatformPayload properties, HuxerUIPlatformChannel.Events events);
    }

    /** Returns the stable Android View embedded for the lifetime of this instance. */
    View getView();

    /** Applies a complete replacement of the controlled properties. */
    void update(PlatformPayload properties);

    /**
     * Handles an optional Controller method invocation.
     *
     * <p>The default implementation reports {@code huxerui/unsupported-method}. Implementations with a Controller
     * complete or fail {@code result} at most once and may return a cancellation operation.</p>
     */
    default HuxerUIPlatformChannel.Cancellation invoke(
            String method, PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
        result.fail("huxerui/unsupported-method", "HuxerUI PlatformView does not support controller calls",
                PlatformPayload.nullValue());
        return null;
    }

    /** Releases listeners, platform resources, and background work owned by this instance. */
    void dispose();
}
