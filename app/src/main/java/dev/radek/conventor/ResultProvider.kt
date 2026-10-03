package dev.radek.conventor

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import org.json.JSONObject
import java.io.File

/** Read-only, URI-granted access to validated host or generated APK results. */
class ResultProvider : ContentProvider() {
    override fun onCreate() = true

    private fun entry(uri: Uri): Pair<File, JSONObject> {
        val parts = uri.pathSegments
        require(parts.size == 2 && parts[0].matches(Regex("[0-9]+-[0-9a-f-]{36}")))
        val root = File(requireNotNull(context).filesDir, "library").canonicalFile
        val entryDirectory = File(root, parts[0]).canonicalFile
        require(entryDirectory.path.startsWith(root.path + File.separator) && entryDirectory.isDirectory)
        val report = JSONObject(File(entryDirectory, "report.json").readText())
        val allowedName = parts[1] == GameStubBuilder.fileName(report) || parts[1] == "RadekiOSConventor-debug.apk"
        require(allowedName) { "unsupported result name" }
        val result = File(entryDirectory, parts[1]).canonicalFile
        require(result.path.startsWith(root.path + File.separator) && result.isFile)
        return result to report
    }

    private fun isInstallable(report: JSONObject, name: String): Boolean {
        if (name == "RadekiOSConventor-debug.apk") return true // old attached-host-result compatibility
        return report.optJSONObject("hostConversion")?.optString("status") == "ATTACHED" ||
            report.optJSONObject("automaticPackage")?.optBoolean("installableAndroidPackage", false) == true ||
            report.optJSONObject("forceConversion")?.optBoolean("installableAndroidPackage", false) == true
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        require(mode == "r") { "read only" }
        return ParcelFileDescriptor.open(entry(uri).first, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri): String {
        val (file, report) = entry(uri)
        return if (isInstallable(report, file.name)) "application/vnd.android.package-archive" else "application/zip"
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        val file = entry(uri).first
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        return MatrixCursor(columns).apply {
            addRow(columns.map { when (it) {
                OpenableColumns.DISPLAY_NAME -> file.name
                OpenableColumns.SIZE -> file.length()
                else -> null
            } })
        }
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = throw UnsupportedOperationException("read only")
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = throw UnsupportedOperationException("read only")
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = throw UnsupportedOperationException("read only")
}
