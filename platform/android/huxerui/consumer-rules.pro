-keep class org.huxerui.HuxerUIApplication { *; }
-keep class org.huxerui.HuxerUIApplication$* { *; }
-keep class org.huxerui.HuxerUIHttpRequest { *; }
-keep class org.huxerui.HuxerUIFilePicker { *; }
-keep class org.huxerui.HuxerUIFilePicker$* { *; }
-keep class org.huxerui.HuxerUIFileReference { *; }
-keep class org.huxerui.HuxerUIFileReference$* { *; }
-keep class org.huxerui.HuxerUIPlatformChannel { *; }
-keep class org.huxerui.HuxerUIPlatformChannel$* { *; }
-keep class org.huxerui.HuxerUIExternalTexture { *; }
-keep class org.huxerui.HuxerUIBufferReference { *; }
-keep class org.huxerui.HuxerUIBufferReference$* { *; }
-keep class org.huxerui.PlatformPayload { *; }
-keep class org.huxerui.PlatformPayload$* { *; }
-keep class org.huxerui.HuxerUIPlatformModule { *; }
-keep class org.huxerui.HuxerUIPlatformModule$* { *; }
-keep class * implements org.huxerui.HuxerUIPlatformModule$Factory { public <init>(); *; }
-keep class * implements org.huxerui.HuxerUIPlatformView$Factory { public <init>(); *; }
-keep class org.huxerui.HuxerUIActivityResultLauncher { *; }
-keep class org.huxerui.HuxerUIActivityResultLauncher$* { *; }
-keep class * implements org.huxerui.HuxerUIPlatformModule$WindowFactory { public <init>(); *; }
-keepclassmembers class org.huxerui.HuxerUIView {
    org.huxerui.HuxerUIFilePicker filePicker();
    private void requestTextInputKeyboard(long, boolean);
}
-keepclassmembers class org.huxerui.HuxerUITextLayout {
    float[] measure();
    double[] lineMetrics();
    long hitTest(float, float);
    float[] caret(long, boolean);
    float[] range(long, long);
    float[] inlineObjectPlacement(long);
    long previous(long);
    long next(long);
}
