package dev.radek.conventor

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File

/** Read-only, URI-granted access to one attached result; never exposes IPA/workspace files. */
class ResultProvider : ContentProvider() {
    override fun onCreate() = true
    private fun file(uri: Uri): File {
        val parts = uri.pathSegments
        require(parts.size == 2 && parts[0].matches(Regex("[0-9]+-[0-9a-f-]{36}")) && parts[1] == "RadekiOSConventor-debug.apk")
        val root = File(requireNotNull(context).filesDir, "library").canonicalFile
        val result = File(File(root, parts[0]), parts[1]).canonicalFile
        require(result.path.startsWith(root.path + File.separator) && result.isFile)
        return result
    }
    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        require(mode == "r") { "read only" }
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY)
    }
    override fun getType(uri: Uri) = "application/vnd.android.package-archive"
    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        val file = file(uri)
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        return MatrixCursor(columns).apply { addRow(columns.map { when (it) { OpenableColumns.DISPLAY_NAME -> file.name; OpenableColumns.SIZE -> file.length(); else -> null } }) }
    }
    override fun insert(uri: Uri, values: ContentValues?): Uri? = throw UnsupportedOperationException("read only")
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = throw UnsupportedOperationException("read only")
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = throw UnsupportedOperationException("read only")
}
