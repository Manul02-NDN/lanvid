plugins {
    id("com.android.library")
}

android {
    namespace = "com.lanvid.sdk"
    compileSdk {
        version = release(37) {
            minorApiLevel = 2
        }
    }
    ndkVersion = "28.2.13676358"

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-O3")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.18.0")
}

tasks.register("exportAar") {
    dependsOn("assembleRelease")
    val srcAar = layout.buildDirectory.file("outputs/aar/lanvid-sdk-release.aar")
    val dstAar = rootProject.layout.projectDirectory.file("../../dist/android/lanvid-video-sdk-3.1.0.aar")
    doLast {
        val out = dstAar.asFile
        out.parentFile.mkdirs()
        srcAar.get().asFile.copyTo(out, overwrite = true)
        println("Exported LanVid Video Android SDK AAR: ${out.absolutePath} (${out.length()} bytes)")
    }
}
