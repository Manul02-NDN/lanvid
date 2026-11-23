# ProGuard / R8 rules for LanVid Monitor (Google Play Release)
# Keep JNI bridge & Kotlin/Compose model classes referenced by C++ (monitor_jni.cpp).

-keep class com.lanvid.monitor.MonitorNative {
    public static native <methods>;
    *;
}

-keep class com.lanvid.monitor.** { *; }
-keep class com.manul02.lanvid.monitor.** { *; }

-keepclasseswithmembernames class * {
    native <methods>;
}

-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
