package org.liveandrecompiled.app

// what the app asks of libmain.so before the game runs (src/runtime/app/android.cpp): each call returns "" or why it
// failed
fun interface Progress {
    // done of total bytes, or -1 for neither; text, when there is a step to name
    fun report(done: Long, total: Long, text: String)
}

object Native {
    init {
        System.loadLibrary("main")
    }

    // the default.xbe's SHA-1 this build was made from
    @JvmStatic external fun expectedSha1(): String

    // the game's files in dir are the release this build was made from
    @JvmStatic external fun verify(dir: String): String

    // a disc image, open as fd, unpacked into out
    @JvmStatic external fun unpackIso(fd: Int, out: String, progress: Progress): String

    // the digital release's Data0000, Data0001, ..., open as fds in that order, unpacked into out
    @JvmStatic external fun unpackPackage(fds: IntArray, out: String, progress: Progress): String
}
