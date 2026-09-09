package org.huxerui;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.RemoteViews;

import java.util.Collections;

final class HuxerUILocalNotificationTest {
    private static final int CAN_CANCEL = 1 << 2;
    private static int assertions;

    private HuxerUILocalNotificationTest() {}

    static int verify(Context context) {
        assertions = 0;
        check(HuxerUILocalNotification.configuredCapabilities(context) == CAN_CANCEL,
              "A host without presentation metadata must expose only cancellation");
        String identifier = "reminders/project 42/准备";
        Uri firstActivation = HuxerUILocalNotification.activationUri(context, identifier);
        Uri secondActivation = HuxerUILocalNotification.activationUri(context, identifier);
        Uri otherActivation = HuxerUILocalNotification.activationUri(context, identifier + "-other");
        check(firstActivation.equals(secondActivation), "Equal identifiers must retain one activation identity");
        check(!firstActivation.equals(otherActivation),
              "Distinct identifiers must retain distinct activation identities");
        check(!firstActivation.equals(HuxerUILocalNotification.alarmUri(context, identifier)),
              "Activation and alarm PendingIntents must not share an identity");

        Intent valid = new Intent(HuxerUILocalNotification.ACTIVATION_ACTION)
                               .setData(firstActivation)
                               .putExtra(HuxerUILocalNotification.IDENTIFIER_EXTRA, identifier);
        HuxerUIApplicationActivation activation = HuxerUIApplicationActivation.fromIntent(context, valid);
        check(activation != null, "A tagged notification Intent must decode");
        check(activation.kind == HuxerUIApplicationActivation.NOTIFICATION,
              "A notification Intent must retain its activation kind");
        check(identifier.equals(activation.value), "A notification Intent must retain its full identifier");

        PlatformPayload payload = PlatformPayload.object(Collections.singletonMap("reminder_id", PlatformPayload.int64(42)));
        byte[] encoded = PlatformPayload.encodeEnvelope(payload).bytes;
        Intent withData = new Intent(valid).putExtra(HuxerUILocalNotification.DATA_EXTRA, encoded);
        HuxerUIApplicationActivation withDataActivation = HuxerUIApplicationActivation.fromIntent(context, withData);
        check(withDataActivation != null && HuxerUILocalNotification.decodeData(withDataActivation.data).equals(payload),
              "Notification activation must retain its application data");
        check(HuxerUIApplicationActivation.fromIntent(context,
                new Intent(valid).putExtra(HuxerUILocalNotification.DATA_EXTRA, new byte[]{1, 2})) == null,
              "Malformed notification data must reject activation");
        check(HuxerUIApplicationActivation.fromIntent(context,
                new Intent(valid).putExtra(HuxerUILocalNotification.DATA_EXTRA, "wrong type")) == null,
              "A notification data extra with the wrong type must reject activation");
        check(HuxerUIApplicationActivation.fromIntent(context,
                new Intent(valid).putExtra(HuxerUILocalNotification.DATA_EXTRA, new byte[65537])) == null,
              "Oversized notification data must reject activation");

        Intent wrongIdentity = new Intent(valid).setData(otherActivation);
        check(HuxerUIApplicationActivation.fromIntent(context, wrongIdentity) == null,
              "Notification data must match its identifier");
        Intent missingIdentifier = new Intent(HuxerUILocalNotification.ACTIVATION_ACTION)
                                           .setData(HuxerUILocalNotification.activationUri(context, "missing"));
        check(HuxerUIApplicationActivation.fromIntent(context, missingIdentifier) == null,
              "A notification Intent without an identifier must be rejected");
        Intent unrelated =
                new Intent(Intent.ACTION_MAIN).putExtra(HuxerUILocalNotification.IDENTIFIER_EXTRA, identifier);
        check(HuxerUIApplicationActivation.fromIntent(context, unrelated) == null,
              "Unrelated Intent actions must not become notification activation");

        HuxerUILocalNotificationContent content =
                new HuxerUILocalNotificationContent(identifier, "Title", "Body", "reminder.rich", payload);
        check(content.getData().equals(payload), "Template content must retain the application data");
        check(identifier.equals(content.getIdentifier()), "Template content must retain the notification identifier");
        check("Title".equals(content.getTitle()), "Template content must retain the title");
        check("Body".equals(content.getBody()), "Template content must retain the body");
        check("reminder.rich".equals(content.getTemplateIdentifier()),
              "Template content must retain the template identifier");

        RemoteViews compact = new RemoteViews("android", android.R.layout.simple_list_item_1);
        RemoteViews expanded = new RemoteViews("android", android.R.layout.simple_list_item_2);
        HuxerUILocalNotificationLayout compactOnly = new HuxerUILocalNotificationLayout(compact);
        check(compactOnly.getContentView() == compact && compactOnly.getExpandedContentView() == null
                && compactOnly.getHeadsUpContentView() == null, "The single-view constructor must omit other layouts");
        HuxerUILocalNotificationLayout layout = new HuxerUILocalNotificationLayout(compact, expanded, null);
        check(layout.getContentView() == compact, "A template layout must retain its compact view");
        check(layout.getExpandedContentView() == expanded, "A template layout must retain its expanded view");
        check(layout.getHeadsUpContentView() == null, "A template layout may omit its heads-up view");
        check(layout.hasViews(), "A template layout with a supplied view must be usable");
        check(!new HuxerUILocalNotificationLayout(null, null, null).hasViews(),
              "An empty template layout must be rejected");
        return assertions;
    }

    private static void check(boolean value, String message) {
        ++assertions;
        if (!value) {
            throw new AssertionError(message);
        }
    }
}
