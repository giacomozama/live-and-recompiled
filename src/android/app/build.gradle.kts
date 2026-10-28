// The app. Gradle properties, all optional:
//   sdlJava      SDL's Java sources (org.libsdl.app), from the SDL the game is built with: the Android build's
//                _deps/sdl3-src/android-project/app/src/main/java, which is the default
//   nativeLibs   a folder with arm64-v8a/libmain.so to put in the APK (a developer build); without it the APK has no
//                native code, the base APK the release ships
//   versionName, versionCode
plugins {
    id("com.android.application")
}

val repo = rootDir.resolve("../..").canonicalFile
val sdlJava = (findProperty("sdlJava") as String?)?.let(::file)
    ?: repo.resolve("out/build/zig-android/_deps/sdl3-src/android-project/app/src/main/java")
val nativeLibs = (findProperty("nativeLibs") as String?)?.let(::file)

layout.buildDirectory.set(repo.resolve("out/build/android/app"))

android {
    namespace = "org.liveandrecompiled.app"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.liveandrecompiled.conker"
        minSdk = 28
        targetSdk = 36
        versionCode = (findProperty("versionCode") as String?)?.toInt() ?: 1
        versionName = (findProperty("versionName") as String?) ?: "dev"
        ndk { abiFilters += "arm64-v8a" }
    }

    sourceSets["main"].java.directories.add(sdlJava.path)
    if (nativeLibs != null) sourceSets["main"].jniLibs.directories.add(nativeLibs.path)

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    // libmain.so stays stored and page aligned in the APK, mapped from there rather than unpacked at install
    packaging {
        jniLibs.useLegacyPackaging = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

tasks.named("preBuild") {
    doFirst {
        check(sdlJava.resolve("org/libsdl/app/SDLActivity.java").exists()) {
            "SDL's Java sources are missing at $sdlJava: build the Android port first, or set -PsdlJava"
        }
    }
}
