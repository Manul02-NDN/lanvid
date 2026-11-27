import java.io.File

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.lanvid.app"
    compileSdk {
        version = release(37) {
            minorApiLevel = 2
        }
    }
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.lanvid.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 270
        versionName = "2.7.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-O3")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    signingConfigs {
        create("release") {
            val ks = rootProject.file("lanvid-release.jks")
            storeFile = ks
            storePassword = System.getenv("LANVID_KS_PASS") ?: "lanvid2026"
            keyAlias = System.getenv("LANVID_KEY_ALIAS") ?: "lanvid"
            keyPassword = System.getenv("LANVID_KEY_PASS") ?: "lanvid2026"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildFeatures {
        compose = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.18.0")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation(platform("androidx.compose:compose-bom:2026.11.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
}

tasks.register("exportUniversalApk") {
    dependsOn("assembleRelease")
    val srcApk = layout.buildDirectory.file("outputs/apk/release/app-release.apk")
    val dstApk = rootProject.layout.projectDirectory.file("LanVid-Android-universal.apk")
    doLast {
        srcApk.get().asFile.copyTo(dstApk.asFile, overwrite = true)
        println("Exported signed universal APK: ${dstApk.asFile.absolutePath} (${dstApk.asFile.length()} bytes)")
    }
}
