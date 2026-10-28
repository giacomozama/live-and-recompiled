plugins {
    id("com.android.application") version "9.2.1" apply false
}

// the build goes beside the other builds, out of the source tree
layout.buildDirectory.set(rootDir.resolve("../../out/build/android"))
