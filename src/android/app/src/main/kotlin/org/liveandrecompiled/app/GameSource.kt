package org.liveandrecompiled.app

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File

// a document in a folder the player picked
data class Doc(val uri: Uri, val id: String, val name: String, val dir: Boolean, val size: Long)

// the game in a picked folder: its extracted files, a disc image, or the digital release's package
sealed interface Found {
    data class Files(val dir: Doc) : Found
    data class Iso(val file: Doc) : Found
    data class Package(val data: List<Doc>) : Found  // Data0000, Data0001, ... in order
}

// a folder tree from ACTION_OPEN_DOCUMENT_TREE, read through the documents provider
class Tree(private val resolver: ContentResolver, private val tree: Uri) {
    val root: Doc = DocumentsContract.getTreeDocumentId(tree).let {
        Doc(DocumentsContract.buildDocumentUriUsingTree(tree, it), it, "", true, 0)
    }

    fun children(dir: Doc): List<Doc> {
        val out = mutableListOf<Doc>()
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dir.id)
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
        )
        resolver.query(uri, columns, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                val id = c.getString(0)
                val isDir = c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR
                out += Doc(DocumentsContract.buildDocumentUriUsingTree(tree, id), id, c.getString(1) ?: "", isDir,
                    if (c.isNull(3)) 0 else c.getLong(3))
            }
        }
        return out
    }

    // the game, nearest the picked folder first and, in one folder, the files before an image before a package (as the
    // game itself looks); a few folders deep, so the folder above the game works too
    fun find(): Found? {
        var level = listOf(root)
        repeat(4) {
            val next = mutableListOf<Doc>()
            for (dir in level) {
                val kids = children(dir)
                if (kids.any { !it.dir && it.name.equals("default.xbe", ignoreCase = true) }) return Found.Files(dir)
                kids.filter { !it.dir && it.name.endsWith(".iso", ignoreCase = true) }.minByOrNull { it.name }
                    ?.let { return Found.Iso(it) }
                packageData(kids)?.let { return Found.Package(it) }
                kids.firstOrNull { it.dir && it.name.equals("DefaultPackage.data", ignoreCase = true) }
                    ?.let { data -> packageData(children(data))?.let { return Found.Package(it) } }
                next += kids.filter { it.dir }
            }
            level = next
        }
        return null
    }

    // the Data files of a package's .data folder, in order, when these are they
    private fun packageData(kids: List<Doc>): List<Doc>? {
        val data = kids.filter { !it.dir && Regex("Data\\d{4}").matches(it.name) }.sortedBy { it.name }
        return if (data.firstOrNull()?.name == "Data0000") data else null
    }

    // every file below dir, with its path under it
    fun files(dir: Doc, prefix: String = ""): List<Pair<String, Doc>> =
        children(dir).flatMap { if (it.dir) files(it, prefix + it.name + "/") else listOf(prefix + it.name to it) }

    // the files below dir into out, done telling the bytes copied of total
    fun copy(dir: Doc, out: File, done: (Long, Long) -> Unit) {
        val all = files(dir)
        val total = all.sumOf { it.second.size }
        var copied = 0L
        val buf = ByteArray(4 shl 20)
        for ((path, doc) in all) {
            if (path.split('/').any { it == ".." }) continue
            val to = File(out, path)
            to.parentFile?.mkdirs()
            resolver.openInputStream(doc.uri)!!.use { input ->
                to.outputStream().use { output ->
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        output.write(buf, 0, n)
                        copied += n
                        done(copied, total)
                    }
                }
            }
        }
    }
}
