// The Android app: the game's activity (SDL's, libmain.so) and the first screen that unpacks the player's copy of the
// game on the device. The release builds it without libmain.so, the base APK the APK builder (src/apkbuilder) adds
// the player's own build of the game to; a developer build (tools/build.sh --android) puts the build's in.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "LiveAndRecompiled"
include(":app")
