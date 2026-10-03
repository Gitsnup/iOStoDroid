package dev.radek.gamestub

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject

/** Branded information stub; it never loads or executes iOS binaries or game data. */
class GameStubActivity : Activity() {
    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val metadata = try {
            assets.open("radek/game-stub.json").bufferedReader().use { JSONObject(it.readText()) }
        } catch (_: Exception) { JSONObject() }
        val application = metadata.optJSONObject("application") ?: JSONObject()
        val title = application.optString("name").takeIf { it.isNotBlank() }
            ?: metadata.optString("originalName", "IPA Game Stub")
        val content = metadata.optJSONObject("bundleContent") ?: JSONObject()
        val background = Color.rgb(13, 20, 33)
        val muted = Color.rgb(174, 190, 210)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(dp(24), dp(32), dp(24), dp(36))
            setBackgroundColor(background)
        }
        setContentView(ScrollView(this).apply { addView(root) })

        try {
            root.addView(ImageView(this).apply {
                setImageDrawable(packageManager.getApplicationIcon(packageName))
                contentDescription = "$title application icon"
                scaleType = ImageView.ScaleType.FIT_CENTER
            }, LinearLayout.LayoutParams(dp(112), dp(112)).apply { bottomMargin = dp(18) })
        } catch (_: Exception) { }

        addText(root, title, 25f, Color.WHITE, true)
        addText(root, "Android compatibility preview", 15f, Color.rgb(92, 227, 181), true)
        addText(root, "This signed APK uses the IPA's recovered launcher icon and includes bounded non-executable bundle resources. It does not contain converted game code and is not playable.", 15f, muted)
        addText(root, "Bundle ID: ${application.optString("bundleId", "unavailable")}", 13f, muted)
        addText(root, "Minimum iOS: ${application.optString("minimumIOSVersion").ifBlank { "Not declared" }}", 13f, muted)
        addText(root, "IPA filename: ${metadata.optString("originalName", "unavailable")}", 13f, muted)
        addText(root, "Source SHA-256: ${metadata.optString("sourceSha256", "unavailable")}", 11f, muted)

        val resourceCount = content.optInt("fileCount", 0)
        val resourceMiB = content.optLong("bytes", 0L) / 1048576.0
        addText(root, "Safe bundle resources included: $resourceCount files · %.1f MiB".format(resourceMiB), 13f, Color.WHITE, true)
        val skippedResources = content.optInt("skippedCount", 0)
        if (skippedResources > 0 || content.optString("status") == "UNAVAILABLE") {
            addText(root, "Resource copy status: ${content.optString("status")} · $skippedResources allowed file(s) skipped or unavailable", 12f, muted)
        }
        addText(root, "Preview images below are copied data only. They are not a functioning game screen.", 12f, muted)
        addImagePreviews(root, muted)
    }

    private fun addImagePreviews(parent: LinearLayout, muted: Int) {
        val index = try {
            assets.open("radek/content-index.json").bufferedReader().use { org.json.JSONArray(it.readText()) }
        } catch (_: Exception) { return }
        var shown = 0
        for (i in 0 until index.length()) {
            if (shown >= 4) break
            val path = index.optJSONObject(i)?.optString("path") ?: continue
            if (path.substringAfterLast('.', "").lowercase() !in setOf("png", "jpg", "jpeg", "webp", "gif", "bmp")) continue
            val bitmap = decodePreview("ipa-content/$path") ?: continue
            parent.addView(ImageView(this).apply {
                setImageBitmap(bitmap)
                contentDescription = "Included bundle resource ${path.substringAfterLast('/')}"
                scaleType = ImageView.ScaleType.FIT_CENTER
                setBackgroundColor(Color.rgb(25, 37, 54))
                setPadding(dp(8), dp(8), dp(8), dp(8))
            }, LinearLayout.LayoutParams(dp(176), dp(132)).apply {
                bottomMargin = dp(8)
                gravity = Gravity.CENTER_HORIZONTAL
            })
            addText(parent, path.substringAfterLast('/'), 11f, muted)
            shown += 1
        }
    }

    private fun decodePreview(assetPath: String): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        try { assets.open(assetPath).use { BitmapFactory.decodeStream(it, null, bounds) } } catch (_: Exception) { return null }
        if (bounds.outWidth !in 1..8192 || bounds.outHeight !in 1..8192) return null
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / sample > 512) sample *= 2
        val options = BitmapFactory.Options().apply { inSampleSize = sample; inPreferredConfig = Bitmap.Config.RGB_565 }
        return try { assets.open(assetPath).use { BitmapFactory.decodeStream(it, null, options) } } catch (_: Exception) { null }
    }

    private fun addText(parent: LinearLayout, value: String, size: Float, color: Int, bold: Boolean = false) {
        parent.addView(TextView(this).apply {
            text = value
            textSize = size
            setTextColor(color)
            if (bold) setTypeface(typeface, android.graphics.Typeface.BOLD)
            gravity = Gravity.CENTER
            setPadding(0, dp(8), 0, dp(8))
        }, LinearLayout.LayoutParams(-1, -2))
    }
}
