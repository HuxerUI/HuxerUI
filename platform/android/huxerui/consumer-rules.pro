-keep class org.huxerui.HuxerUIHttpRequest { *; }
-keep class org.huxerui.HuxerUIFilePicker { *; }
-keep class org.huxerui.HuxerUIFilePicker$* { *; }
-keep class org.huxerui.HuxerUIFileReference { *; }
-keep class org.huxerui.HuxerUIFileReference$* { *; }
-keepclassmembers class org.huxerui.HuxerUIView {
    boolean canOpenFiles();
    boolean canSaveFiles();
    org.huxerui.HuxerUIFilePicker$Operation prepareOpenFiles(long, java.lang.String[], java.lang.String[], boolean);
    org.huxerui.HuxerUIFilePicker$Operation prepareSaveFile(long, java.lang.String, java.lang.String, java.lang.String[], java.lang.String[]);
}
