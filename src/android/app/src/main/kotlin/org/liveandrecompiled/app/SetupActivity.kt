package org.liveandrecompiled.app

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.os.StatFs
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import kotlin.concurrent.thread

// The app's first screen. The game's files go in the app's own storage (filesDir/assets, where libmain.so looks for
// them) once: the player picks their copy with Android's document picker, a disc image (.iso) or a folder with the
// extracted files or the digital release's package, and it is checked and unpacked there. After that the app goes
// straight to the game.
class SetupActivity : Activity() {
    private val assets by lazy { File(filesDir, "assets") }
    private lateinit var status: TextView
    private lateinit var bar: ProgressBar
    private lateinit var buttons: List<Button>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (ready()) {
            play()
            return
        }
        setContentView(layout())
    }

    // the game's files are there, from a finished unpack, and the release this build was made from
    private fun ready(): Boolean = File(assets, READY).exists() && Native.verify(assets.path).isEmpty()

    private fun play() {
        startActivity(Intent(this, GameActivity::class.java))
        finish()
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun layout(): View {
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(32), dp(24), dp(24))
        }
        fun text(s: String, size: Float, bold: Boolean = false) = TextView(this).apply {
            text = s
            textSize = size
            setTextColor(Color.WHITE)
            if (bold) typeface = Typeface.DEFAULT_BOLD
            setPadding(0, 0, 0, dp(12))
        }
        column.addView(text(getString(R.string.app_name), 24f, true))
        column.addView(text(
            "The game's files are not on this device yet. Pick your copy of the game, and they are unpacked into " +
                "this app's storage (about 5 GB) once.\n\n" +
                "• A disc image (.iso) of the USA release\n" +
                "• A folder with the extracted files (default.xbe with its folders)\n" +
                "• A folder with the Xbox Store release's DefaultPackage and DefaultPackage.data", 15f))
        val iso = Button(this).apply {
            text = "Choose a disc image (.iso)"
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                }, PICK_ISO)
            }
        }
        val folder = Button(this).apply {
            text = "Choose a folder"
            setOnClickListener { startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER) }
        }
        buttons = listOf(iso, folder)
        buttons.forEach(column::addView)
        bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            visibility = View.INVISIBLE
            setPadding(0, dp(16), 0, dp(8))
        }
        column.addView(bar)
        status = text("", 14f).apply { gravity = Gravity.START }
        column.addView(status)
        return ScrollView(this).apply {
            setBackgroundColor(Color.rgb(0x1B, 0x1F, 0x24))
            addView(column)
        }
    }

    @Deprecated("Activity's own; the app uses no AndroidX")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (resultCode != RESULT_OK || uri == null) return
        when (requestCode) {
            PICK_ISO -> unpack { unpackIso(uri) }
            PICK_FOLDER -> unpack { unpackFolder(uri) }
        }
    }

    // a step on a thread of its own: the old files gone first, the screen kept on, the result checked; work returns
    // "" or why it failed
    private fun unpack(work: () -> String) {
        buttons.forEach { it.isEnabled = false }
        bar.visibility = View.VISIBLE
        bar.isIndeterminate = true
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        say("Getting ready…")
        thread(name = "unpack") {
            val error = try {
                assets.deleteRecursively()
                assets.mkdirs()
                if (StatFs(filesDir.path).availableBytes < NEEDED) {
                    "There is not enough free space: the game's files need about 6 GB."
                } else {
                    work().ifEmpty { Native.verify(assets.path) }
                }
            } catch (e: Exception) {
                e.message ?: e.toString()
            }
            if (error.isEmpty()) File(assets, READY).writeText(Native.expectedSha1())
            else assets.deleteRecursively()
            runOnUiThread {
                window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                if (error.isEmpty()) {
                    play()
                } else {
                    bar.visibility = View.INVISIBLE
                    buttons.forEach { it.isEnabled = true }
                    say(error)
                }
            }
        }
    }

    private fun unpackIso(uri: Uri): String {
        val pfd = contentResolver.openFileDescriptor(uri, "r") ?: return "The file could not be opened."
        return pfd.use { Native.unpackIso(it.fd, assets.path, progress) }
    }

    private fun unpackFolder(uri: Uri): String {
        say("Looking for the game in the folder…")
        val tree = Tree(contentResolver, uri)
        return when (val found = tree.find()) {
            null -> "There is no game in this folder: it needs default.xbe and its folders, a disc image (.iso), or " +
                "DefaultPackage with DefaultPackage.data."
            is Found.Iso -> unpackIso(found.file.uri)
            is Found.Files -> {
                say("Copying the game's files…")
                tree.copy(found.dir, assets) { done, total -> progress.report(done, total, "") }
                ""
            }
            is Found.Package -> {
                val fds = found.data.map {
                    contentResolver.openFileDescriptor(it.uri, "r") ?: return "${it.name} could not be opened."
                }
                try {
                    Native.unpackPackage(fds.map { it.fd }.toIntArray(), assets.path, progress)
                } finally {
                    fds.forEach { it.close() }
                }
            }
        }
    }

    private val progress = Progress { done, total, text ->
        runOnUiThread {
            if (total > 0) {
                bar.isIndeterminate = false
                bar.progress = (done * 1000 / total).toInt()
            }
            if (text.isNotEmpty()) status.text = text
        }
    }

    private fun say(text: String) = runOnUiThread { status.text = text }

    private companion object {
        const val PICK_ISO = 1
        const val PICK_FOLDER = 2
        const val READY = ".ready"
        const val NEEDED = 6_000_000_000L
    }
}
