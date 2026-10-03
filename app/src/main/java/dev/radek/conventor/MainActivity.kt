package dev.radek.conventor

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.widget.*
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors
import java.util.zip.ZipFile

private object Jobs {
    private val executor = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private val recent = mutableListOf<String>()
    @Volatile var busy = false
    @Volatile var message = ""
    @Volatile var stage = "Ready"
    @Volatile var percent = 0
    var listener: (() -> Unit)? = null

    @Synchronized fun history(): List<String> = recent.toList()
    @Synchronized fun update(progress: Int, phase: String, text: String) {
        percent = progress.coerceIn(0, 100)
        stage = phase
        message = text
        val line = if (phase.isBlank()) text else "$phase · $text"
        if (recent.lastOrNull() != line) {
            recent.add(line)
            if (recent.size > 8) recent.removeAt(0)
        }
        main.post { listener?.invoke() }
    }
    fun update(progress: WorkflowProgress) = update(progress.percent, progress.stage, progress.message)
    fun update(text: String) = update(percent, stage.ifBlank { "Working" }, text)

    @Synchronized fun run(block: () -> Unit) {
        check(!busy) { "A job is already running" }
        busy = true
        percent = 0
        stage = "Starting"
        message = "Preparing conversion workflow"
        recent.clear()
        executor.execute {
            try { block() }
            catch (e: Exception) { update(percent, "Stopped", "${e.javaClass.simpleName}: ${e.message}") }
            finally { busy = false; main.post { listener?.invoke() } }
        }
    }
}

class MainActivity : Activity() {
    private val bgColor = Color.rgb(12, 19, 31)
    private val panel = Color.rgb(23, 34, 51)
    private val muted = Color.rgb(160, 178, 199)
    private val accent = Color.rgb(92, 227, 181)
    private lateinit var library: Library
    private lateinit var body: LinearLayout
    private var selected: File? = null
    private var progressLabel: TextView? = null
    private var liveProgressBar: ProgressBar? = null
    private var livePercentLabel: TextView? = null
    private var liveStageLabel: TextView? = null
    private var liveMessageLabel: TextView? = null
    private var liveEventsLabel: TextView? = null
    private val settingsPrefs by lazy { getSharedPreferences("radek_preferences", MODE_PRIVATE) }
    private var wasBusy = false
    private var returnToDetailAfterJob: File? = null
    private var installAfterJob: Pair<File, String>? = null
    private var pendingInstall: Pair<File, String>? = null
    private val pickerIpa = 100
    private val pickerApk = 101
    private fun dp(n: Int) = (n * resources.displayMetrics.density).toInt()

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        window.statusBarColor = bgColor; window.navigationBarColor = bgColor
        library = Library(applicationContext)
        if (!Jobs.busy) library.recoverInterrupted()
        selected = state?.getString("selected")?.let { File(library.root, it) }
        Jobs.listener = {
            progressLabel?.text = Jobs.message
            renderLiveProgress()
            if (wasBusy && !Jobs.busy) {
                wasBusy = false
                val target = returnToDetailAfterJob
                returnToDetailAfterJob = null
                if (target != null && target.isDirectory) detail(target) else home()
                val installRequest = installAfterJob
                installAfterJob = null
                if (installRequest != null && installRequest.first.isDirectory) installArtifact(installRequest.first, installRequest.second)
            }
        }
        if (selected != null && File(selected, "report.json").isFile) detail(selected!!) else home()
    }
    override fun onSaveInstanceState(out: Bundle) { super.onSaveInstanceState(out); out.putString("selected", selected?.name) }
    override fun onDestroy() { Jobs.listener = null; super.onDestroy() }
    override fun onResume() {
        super.onResume()
        if (Jobs.busy) { wasBusy = true; progressLabel?.text = Jobs.message; renderLiveProgress() }
        val pending = pendingInstall
        if (pending != null && packageManager.canRequestPackageInstalls()) {
            pendingInstall = null
            installArtifact(pending.first, pending.second)
        }
    }

    private fun rounded(color: Int): GradientDrawable = GradientDrawable().apply { setColor(color); cornerRadius = dp(18).toFloat() }
    private fun screen() {
        progressLabel = null
        liveProgressBar = null
        livePercentLabel = null
        liveStageLabel = null
        liveMessageLabel = null
        liveEventsLabel = null
        val scroll = ScrollView(this).apply { setBackgroundColor(bgColor); isFillViewport = true }
        body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(22), dp(30), dp(22), dp(36)) }
        scroll.addView(body); setContentView(scroll)
    }
    private fun text(value: String, size: Float = 16f, color: Int = Color.WHITE, bold: Boolean = false, parent: LinearLayout = body): TextView = TextView(this).apply {
        text = value; textSize = size; setTextColor(color); if (bold) setTypeface(typeface, Typeface.BOLD)
        setPadding(0, dp(6), 0, dp(7)); parent.addView(this)
    }
    private fun button(label: String, primary: Boolean = false, parent: LinearLayout = body, action: () -> Unit): Button = Button(this).apply {
        text = label; isAllCaps = false; textSize = 15f; setTextColor(if (primary) bgColor else accent)
        background = rounded(if (primary) accent else panel)
        parent.addView(this, LinearLayout.LayoutParams(-1, dp(54)).apply { topMargin = dp(12); bottomMargin = dp(4) })
        setOnClickListener { action() }
    }
    private fun dangerButton(label: String, action: () -> Unit): Button = Button(this).apply {
        text = label; isAllCaps = false; textSize = 15f; setTextColor(Color.WHITE)
        background = rounded(Color.rgb(195, 45, 58))
        body.addView(this, LinearLayout.LayoutParams(-1, dp(54)).apply { topMargin = dp(12); bottomMargin = dp(4) })
        setOnClickListener { action() }
    }
    private fun card(parent: LinearLayout = body): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL; background = rounded(panel); setPadding(dp(18), dp(14), dp(18), dp(16))
        parent.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(14) })
    }
    private fun addLiveProgressCard(parent: LinearLayout = body) {
        val progressCard = card(parent)
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        progressCard.addView(header)
        text("LIVE CONVERSION WORKFLOW", 11f, accent, true, header)
        livePercentLabel = text("${Jobs.percent}%", 23f, Color.WHITE, true, progressCard)
        liveProgressBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            isIndeterminate = false
            progress = Jobs.percent
            progressTintList = android.content.res.ColorStateList.valueOf(accent)
            progressBackgroundTintList = android.content.res.ColorStateList.valueOf(Color.rgb(45, 59, 78))
        }
        progressCard.addView(liveProgressBar, LinearLayout.LayoutParams(-1, dp(10)).apply { topMargin = dp(2); bottomMargin = dp(8) })
        liveStageLabel = text(Jobs.stage, 15f, Color.WHITE, true, progressCard)
        liveMessageLabel = text(Jobs.message, 13f, muted, parent = progressCard)
        text("Workflow steps completed · not a percentage of playable code", 11f, muted, parent = progressCard)
        liveEventsLabel = text("", 11f, muted, parent = progressCard)
        renderLiveProgress()
    }
    private fun renderLiveProgress() {
        liveProgressBar?.progress = Jobs.percent
        livePercentLabel?.text = "${Jobs.percent}%"
        liveStageLabel?.text = Jobs.stage
        liveMessageLabel?.text = Jobs.message
        val events = Jobs.history()
        liveEventsLabel?.let { label ->
            label.visibility = if (settingsPrefs.getBoolean("detailed_steps", true) && events.isNotEmpty()) View.VISIBLE else View.GONE
            label.text = events.joinToString("\n") { "• $it" }
        }
    }
    private fun addStoredWorkflowCard(progress: JSONObject, codePercent: Int, parent: LinearLayout = body) {
        val progressCard = card(parent)
        val percent = progress.optInt("percent", 0).coerceIn(0, 100)
        text("IPA → APK workflow · $percent%", 15f, Color.WHITE, true, progressCard)
        val bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            isIndeterminate = false
            this.progress = percent
            progressTintList = android.content.res.ColorStateList.valueOf(accent)
            progressBackgroundTintList = android.content.res.ColorStateList.valueOf(Color.rgb(45, 59, 78))
        }
        progressCard.addView(bar, LinearLayout.LayoutParams(-1, dp(8)).apply { bottomMargin = dp(8) })
        text("${progress.optString("stage")} · ${progress.optString("status")}", 13f, statusColor(progress.optString("status")), true, progressCard)
        text(progress.optString("message"), 13f, muted, parent = progressCard)
        text("This is workflow-stage completion, not a playable-game percentage. Runnable code emitted: ${codePercent.coerceIn(0, 100)}%.", 11f, muted, parent = progressCard)
    }
    private fun statusColor(state: String) = when (state) { "READY", "STUB_CREATED", "ANALYSIS_COMPLETE" -> accent; "FAILED", "BLOCKED" -> Color.rgb(255, 157, 139); else -> Color.rgb(245, 203, 116) }
    private fun formatBytes(n: Long) = "%.1f MiB".format(n / 1048576.0)
    private fun home() {
        selected = null; screen()
        val brandRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        text("R / OFFLINE CONVERSION LAB", 11f, accent, true, brandRow)
        val settingsButton = Button(this).apply {
            text = "⚙ Settings"
            isAllCaps = false
            textSize = 13f
            setTextColor(Color.WHITE)
            background = rounded(panel)
            setOnClickListener { settings() }
        }
        brandRow.addView(settingsButton, LinearLayout.LayoutParams(-2, dp(42)).apply { leftMargin = dp(8) })
        body.addView(brandRow, LinearLayout.LayoutParams(-1, -2))
        text("RadekConverter", 30f, Color.WHITE, true)
        text("A clear, honest path from IPA analysis to Android output.", 15f, muted)
        val info = card()
        text("No emulation. No hidden success.", 17f, Color.WHITE, true, info)
        text("Choose an authorized IPA to start a visible analysis workflow automatically. The Android app can build a signed icon-branded placeholder, but does not compile the iOS game; full game/API translation remains limited.", 14f, muted, parent = info)
        val add = button("+ ADD IPA", true) { authorize() }; add.isEnabled = !Jobs.busy
        if (Jobs.busy) {
            wasBusy = true
            addLiveProgressCard()
        } else if (Jobs.message.startsWith("Failed:")) text(Jobs.message, 14f, statusColor("FAILED"))
        text("Game Library", 23f, Color.WHITE, true)
        val entries = library.entries()
        text("${entries.size} imported ${if (entries.size == 1) "application" else "applications"} · private device storage", 12f, muted)
        if (entries.isEmpty()) {
            val empty = card(); text("Your library starts here", 19f, Color.WHITE, true, empty)
            text("Select an .ipa you own or have permission to convert. Encrypted and FairPlay-protected binaries are never decrypted.", 14f, muted, parent = empty)
        }
        entries.forEach { (dir, report) ->
            val item = card(); val app = report.optJSONObject("application") ?: JSONObject()
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }; item.addView(row)
            val iconPath = File(dir, "icon.png")
            val iconBitmap = if (iconPath.isFile) android.graphics.BitmapFactory.decodeFile(iconPath.path) else null
            val icon = ImageView(this).apply {
                if (iconBitmap != null) setImageBitmap(iconBitmap) else setImageResource(dev.radek.conventor.R.drawable.ic_launcher)
                contentDescription = if (iconBitmap != null) "Application icon"
                else report.optJSONObject("icon")?.optString("reason")?.takeIf { it.isNotBlank() } ?: "Icon unavailable"
            }
            row.addView(icon, LinearLayout.LayoutParams(dp(56), dp(56)).apply { rightMargin = dp(14) })
            val labels = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }; row.addView(labels, LinearLayout.LayoutParams(0, -2, 1f))
            text(app.optString("name", "Import failed"), 19f, Color.WHITE, true, labels)
            text(app.optString("bundleId", "No metadata available"), 12f, muted, parent = labels)
            app.optString("minimumIOSVersion").takeIf { it.isNotBlank() }?.let { text("Minimum iOS $it", 11f, muted, parent = labels) }
            val state = report.optString("state", "FAILED")
            text(state, 11f, statusColor(state), true, item)
            report.optJSONObject("workflowProgress")?.let { progress ->
                val percent = progress.optInt("percent", 0).coerceIn(0, 100)
                text("IPA → APK workflow: $percent% · ${progress.optString("stage")}", 11f, statusColor(progress.optString("status")), true, item)
                item.addView(ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
                    max = 100
                    isIndeterminate = false
                    this.progress = percent
                    progressTintList = android.content.res.ColorStateList.valueOf(accent)
                    progressBackgroundTintList = android.content.res.ColorStateList.valueOf(Color.rgb(45, 59, 78))
                }, LinearLayout.LayoutParams(-1, dp(6)))
            }
            report.optJSONObject("portProgress")?.let { port ->
                text("Playable Android code emitted: ${port.optInt("percent", 0)}%", 11f, statusColor("BLOCKED"), true, item)
            }
            report.optJSONObject("apiMapping")?.let { mapping ->
                val mapped = mapping.optInt("mappedNameCandidates", 0)
                val total = mapping.optInt("distinctImportSymbols", 0)
                val coverage = mapping.optInt("candidateCoveragePercent", 0)
                val summary = if (total == 0) "API-name candidates: N/A (no imported symbols) · not a working-port score"
                    else "API-name candidates: $coverage% ($mapped/$total) · not a working-port score"
                text(summary, 11f, muted, parent = item)
            }
            report.optJSONObject("automaticPackage")?.let { automatic ->
                val artifact = automatic.optString("artifact").takeIf { it.isNotBlank() }?.let { " · $it" }.orEmpty()
                text("Automatic icon-stub APK build: ${automatic.optInt("progressPercent", 0)}% · ${automatic.optString("status")}$artifact · not playable", 11f, muted, parent = item)
            }
            report.optJSONObject("forceConversion")?.let { forced ->
                val artifact = forced.optString("artifact").takeIf { it.isNotBlank() }?.let { " · $it" }.orEmpty()
                text("Force APK build: ${forced.optInt("progressPercent", 0)}% · ${forced.optString("status")}$artifact", 11f, muted, parent = item)
            }
            text("v${app.optString("version", "—")}  ·  ${architectures(report)}  ·  ${formatBytes(app.optLong("fileSize"))}", 12f, muted, parent = item)
            item.isClickable = true; item.setOnClickListener { detail(dir) }; item.contentDescription = "View ${app.optString("name")} details"
        }
        text("v${packageManager.getPackageInfo(packageName, 0).versionName}  /  ARM64 Android  /  offline inspection", 11f, muted)
    }
    private fun architectures(report: JSONObject): String {
        val slices = report.optJSONObject("machO")?.optJSONArray("slices") ?: return "unknown CPU"
        return (0 until slices.length()).joinToString(" / ") { slices.getJSONObject(it).getString("architecture") }
    }
    private fun authorize() {
        AlertDialog.Builder(this).setTitle("Authorized files only")
            .setMessage("Confirm that you own this IPA or have permission to convert it. Protection mechanisms will not be bypassed. The source IPA is retained in app-private storage for analysis and an IPA-named Android APK until you delete this library entry. The APK uses the recovered icon and may include bounded non-executable bundle assets, but it contains no converted game code and is not playable.")
            .setNegativeButton("Cancel", null).setPositiveButton("I have permission") { _, _ ->
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "*/*"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerIpa)
            }.show()
    }
    private fun settingToggle(title: String, description: String, key: String, defaultValue: Boolean) {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val labels = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        text(title, 15f, Color.WHITE, true, labels)
        text(description, 12f, muted, parent = labels)
        row.addView(labels, LinearLayout.LayoutParams(0, -2, 1f))
        val control = Switch(this).apply { isChecked = settingsPrefs.getBoolean(key, defaultValue) }
        control.setOnCheckedChangeListener { _, checked -> settingsPrefs.edit().putBoolean(key, checked).apply() }
        row.addView(control, LinearLayout.LayoutParams(-2, -2).apply { leftMargin = dp(12) })
        val panelCard = card()
        panelCard.addView(row)
    }

    private fun settings() {
        screen()
        button("← Back to library") { home() }
        text("Settings", 30f, Color.WHITE, true)
        text("Small, local controls. Nothing is uploaded.", 14f, muted)

        text("Import workflow", 19f, Color.WHITE, true)
        settingToggle(
            "Auto-build installable APK after analysis",
            "ON by default. Creates a signed icon APK with metadata and bounded safe assets; no game code is converted. Force is always available.",
            "auto_build_apk",
            true,
        )
        settingToggle(
            "Show detailed live build steps",
            "Adds recent extraction, analysis, signing and validation events to the live progress card.",
            "detailed_steps",
            true,
        )

        text("Host converter", 19f, Color.WHITE, true)
        val preferredAbi = settingsPrefs.getString("target_abi", "auto") ?: "auto"
        val abiLabel = when (preferredAbi) {
            "armeabi-v7a" -> "32-bit ARM (armeabi-v7a)"
            "arm64-v8a" -> "64-bit ARM (arm64-v8a)"
            else -> "Automatic (32-bit ARM IPA → 32-bit APK; ARM64 IPA → ARM64 APK)"
        }
        button("Preferred host APK ABI\n$abiLabel") {
            val options = arrayOf(
                "Automatic (select from IPA slices)",
                "32-bit ARM (armeabi-v7a)",
                "64-bit ARM (arm64-v8a)",
            )
            val selectedIndex = when (preferredAbi) { "armeabi-v7a" -> 1; "arm64-v8a" -> 2; else -> 0 }
            AlertDialog.Builder(this)
                .setTitle("Host converter ABI")
                .setSingleChoiceItems(options, selectedIndex) { dialog, which ->
                    settingsPrefs.edit().putString("target_abi", when (which) { 1 -> "armeabi-v7a"; 2 -> "arm64-v8a"; else -> "auto" }).apply()
                    dialog.dismiss()
                    settings()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
        text("This only affects the copied host-converter command. On-device Force still creates an architecture-neutral Java stub; it does not compile ARM game code.", 12f, muted)

        text("Installation", 19f, Color.WHITE, true)
        button(if (packageManager.canRequestPackageInstalls()) "APK install permission: enabled" else "Allow APK installs from this app") {
            if (!packageManager.canRequestPackageInstalls()) {
                startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            } else Toast.makeText(this, "Android APK install permission is already enabled", Toast.LENGTH_SHORT).show()
        }

        val honesty = card()
        text("What this app can do", 16f, Color.WHITE, true, honesty)
        text("It inspects IPA metadata and native binaries, reports candidate APIs, and can package a signed icon stub with bounded non-executable resources. It does not translate UIKit, Swift, game engines, graphics, audio or general app behavior. Workflow progress is not a playability score.", 13f, muted, parent = honesty)
        button("Delete all imported entries", parent = body) {
            if (!Jobs.busy) AlertDialog.Builder(this)
                .setTitle("Delete all local IPA entries?")
                .setMessage("This removes retained IPAs, reports, icons and generated APKs from this device.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Delete all") { _, _ -> library.entries().forEach { it.first.deleteRecursively() }; home() }
                .show()
        }
    }

    private fun detail(dir: File) {
        selected = dir; screen()
        val report = JSONObject(File(dir, "report.json").readText()); val app = report.optJSONObject("application") ?: JSONObject()
        button("← Game Library") { home() }
        val iconPath = File(dir, "icon.png")
        if (iconPath.isFile) {
            val bitmap = android.graphics.BitmapFactory.decodeFile(iconPath.path)
            if (bitmap != null) {
                val icon = ImageView(this).apply {
                    setImageBitmap(bitmap)
                    contentDescription = "${app.optString("name", "Application")} icon"
                    scaleType = ImageView.ScaleType.FIT_CENTER
                }
                body.addView(icon, LinearLayout.LayoutParams(dp(84), dp(84)).apply { gravity = Gravity.CENTER_HORIZONTAL; topMargin = dp(10) })
            }
        }
        text(app.optString("name", "Application details"), 28f, Color.WHITE, true)
        val state = report.optString("state"); text(state, 12f, statusColor(state), true)
        if (Jobs.busy) addLiveProgressCard()
        val metadata = card()
        for ((name, value) in listOf(
            "Bundle ID" to app.optString("bundleId"),
            "Version / build" to "${app.optString("version")} / ${app.optString("build")}",
            "Minimum iOS" to app.optString("minimumIOSVersion").ifBlank { "Not declared in Info.plist" },
            "Architecture" to architectures(report),
            "IPA size" to formatBytes(app.optLong("fileSize")),
            "Executable" to app.optString("executable"),
        )) {
            text(name.uppercase(), 10f, muted, true, metadata); text(value, 15f, parent = metadata)
        }
        report.optJSONObject("apiMapping")?.let { mapping ->
            val mapped = mapping.optInt("mappedNameCandidates", 0)
            val total = mapping.optInt("distinctImportSymbols", 0)
            val coverage = mapping.optInt("candidateCoveragePercent", 0)
            val mappingCard = card()
            val summary = if (total == 0) "Android API-name candidates: N/A (no imports)" else "Android API-name candidates: $coverage% ($mapped/$total)"
            text(summary, 16f, Color.WHITE, true, mappingCard)
            text(mapping.optString("measure") + " Same-name candidates are not rewritten or linked, and do not predict gameplay compatibility or stability.", 13f, muted, parent = mappingCard)
        }
        report.optJSONObject("portProgress")?.let { port ->
            val portCard = card()
            text("On-device runnable port emitted: ${port.optInt("percent", 0)}%", 16f, statusColor("BLOCKED"), true, portCard)
            text(port.optString("basis"), 13f, muted, parent = portCard)
        }
        if (!Jobs.busy) report.optJSONObject("workflowProgress")?.let { progress ->
            addStoredWorkflowCard(progress, report.optJSONObject("portProgress")?.optInt("percent", 0) ?: 0)
        }
        report.optJSONObject("automaticPackage")?.let { automatic ->
            val packageCard = card()
            val built = automatic.optString("status") == "GAME_STUB_CREATED"
            text("Automatic APK packaging: ${automatic.optInt("progressPercent", 0)}% · ${automatic.optString("status")}", 15f, statusColor(if (built) "READY" else state), true, packageCard)
            val content = report.optJSONObject("stubContent")
            text("This installable stub uses the recovered game icon and may include up to 64 MiB of safe, non-executable bundle assets (${content?.optInt("fileCount", 0) ?: 0} file(s)). It excludes the IPA and converted game code; playable-code output remains ${report.optJSONObject("portProgress")?.optInt("percent", 0) ?: 0}%.", 12f, muted, parent = packageCard)
            content?.takeIf { it.optString("status") in setOf("PARTIAL", "UNAVAILABLE") }?.let {
                text("Safe-resource status: ${it.optString("status")} · ${it.optString("notice")}", 12f, statusColor("FAILED"), parent = packageCard)
            }
            if ((content?.optInt("skippedCount") ?: 0) > 0) text("${content?.optInt("skippedCount")} allowlisted resource file(s) were skipped by the safety/size limits.", 12f, muted, parent = packageCard)
            automatic.optString("artifact").takeIf { it.isNotBlank() }?.let { text("Output filename: $it", 12f, muted, parent = packageCard) }
            automatic.optString("error").takeIf { it.isNotBlank() }?.let { text(it, 13f, statusColor("FAILED"), parent = packageCard) }
        }
        report.optJSONObject("forceConversion")?.let { forced ->
            val forceCard = card()
            val built = forced.optString("status") == "APK_CREATED"
            text("Force APK build: ${forced.optInt("progressPercent", 0)}% · ${forced.optString("status")}", 15f, statusColor(if (built) "READY" else state), true, forceCard)
            text("The output is a signed installable icon-branded stub, not a converted or playable game. Force opens Android's installer; you must confirm installation.", 12f, muted, parent = forceCard)
            forced.optString("artifact").takeIf { it.isNotBlank() }?.let { text("Output filename: $it", 12f, muted, parent = forceCard) }
            forced.optString("error").takeIf { it.isNotBlank() }?.let { text(it, 13f, statusColor("FAILED"), parent = forceCard) }
        }
        text("Compatibility report", 22f, Color.WHITE, true)
        val blockers = report.optJSONArray("blockers")
        if (blockers != null) for (i in 0 until blockers.length()) text(blockers.getString(i), 15f, statusColor(state))
        if (report.has("error")) text(report.getString("error"), 15f, statusColor("FAILED"))
        text("Icon: ${report.optJSONObject("icon")?.optString("reason") ?: "not extracted"}", 13f, muted)
        val edges = report.optJSONObject("dependencies")?.optJSONArray("edges")
        if (edges != null) for (i in 0 until edges.length()) {
            val dep = edges.getJSONObject(i)
            val classification = dep.optString("classification", "unverified").uppercase()
            text("$classification · ${dep.getString("installName")}", 13f, muted)
            text(dep.optString("reason"), 11f, muted)
        }
        button("View full machine-readable report") { showText("Conversion report", report.toString(2)) }
        button("View real conversion logs") { showText("Logs", File(dir, "conversion.jsonl").takeIf { it.isFile }?.readText() ?: "No logs") }
        text("Native build result", 22f, Color.WHITE, true)
        text("Use the repository's host converter to build the verified leaf subset. The Android app does not contain an SDK/NDK toolchain. UIKit, Swift, graphics, audio and general game conversion are not implemented.", 14f, muted)
        button("Copy host build command") {
            val abi = settingsPrefs.getString("target_abi", "auto") ?: "auto"
            val suffix = if (abi == "auto") "" else " --target-abi $abi"
            val command = "python3 -m radek convert input.ipa --authorized --output workspace/result$suffix"
            (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(android.content.ClipData.newPlainText("Host build command", command))
            Toast.makeText(this, "Host command copied", Toast.LENGTH_SHORT).show()
        }
        if (app.has("sha256")) button("Attach host-built APK") {
            if (!Jobs.busy) startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "application/vnd.android.package-archive"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerApk)
        }
        val outputName = GameStubBuilder.fileName(report)
        val outputFile = File(dir, outputName).takeIf { it.isFile }
            ?: File(dir, "RadekiOSConventor-debug.apk").takeIf { it.isFile }
        val hostAttached = report.optJSONObject("hostConversion")?.optString("status") == "ATTACHED"
        if (outputFile != null) {
            if (hostAttached) {
                text("Host-built APK attached. Package identity and source provenance matched; Android verifies the signature during installation.", 13f, accent)
            } else {
                text("Installable icon-branded APK generated with metadata and any safely retained bundle resources. It omits the IPA and game code, so it is not playable.", 13f, muted)
            }
            button("Install ${outputFile.name}", true) { installArtifact(dir, outputFile.name) }
            button("Share ${outputFile.name}") { shareResultApk(dir, outputFile.name) }
            if (hostAttached && app.has("sha256")) button("Open installed converted program") {
                val pkg = "dev.radek.converted.p" + app.getString("sha256").take(20)
                val intent = packageManager.getLaunchIntentForPackage(pkg)
                if (intent == null) Toast.makeText(this, "Converted program is not installed or not visible to Android", Toast.LENGTH_LONG).show() else startActivity(intent)
            }
        }
        if (File(dir, "source.ipa").isFile) {
            text("The automatic output is an installable icon-branded APK with any safely retained bundle resources, not a game conversion. Force rebuilds the same IPA-named APK and opens Android's installer; Android will still ask you to confirm.", 13f, muted)
            dangerButton(if (outputFile == null) "Force convert to .apk" else "Force rebuild & install .apk") { confirmForceConversion(dir) }
        }
        button("Delete library entry") {
            if (!Jobs.busy) AlertDialog.Builder(this).setTitle("Delete imported entry?").setMessage("Removes the retained IPA, reports, icon-branded game-stub APK and any attached host APK from this device.")
                .setNegativeButton("Cancel", null).setPositiveButton("Delete") { _, _ -> dir.deleteRecursively(); home() }.show()
        }
    }
    private fun confirmForceConversion(dir: File) {
        AlertDialog.Builder(this)
            .setTitle("Important: this will not be a playable game")
            .setMessage("Force creates an IPA-named, signed Android APK using the recovered game icon. It may include bounded non-executable bundle assets, but no original IPA or converted game code; the installed app is not playable. The live workflow shows packaging progress, not game conversion progress. Android's installer will open when packaging finishes and still requires confirmation.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Force convert to .apk") { _, _ -> startForceConversion(dir) }
            .show()
    }

    private fun startForceConversion(dir: File) {
        if (Jobs.busy) return
        returnToDetailAfterJob = dir
        wasBusy = true
        Jobs.run {
            val library = Library(applicationContext)
            val reportFile = File(dir, "report.json")
            val report = JSONObject(reportFile.readText())
            val forced = JSONObject()
                .put("status", "PACKAGING")
                .put("progressPercent", 0)
                .put("startedAt", java.time.Instant.now().toString())
                .put("artifact", GameStubBuilder.fileName(report))
                .put("installableAndroidPackage", true)
                .put("gameCodeConverted", false)
            report.put("forceConversion", forced)
            library.save(dir, report)
            Jobs.update(0, "Checking IPA", "Verifying the retained file before building the Android package")
            try {
                val apk = GameStubBuilder.create(applicationContext, dir, report) { percent, message ->
                    forced.put("progressPercent", percent)
                        .put("message", message)
                        .put("updatedAt", java.time.Instant.now().toString())
                    library.save(dir, report)
                    Jobs.update(percent, "Building signed icon APK", message)
                }
                forced.put("status", "APK_CREATED")
                    .put("progressPercent", 100)
                    .put("artifact", apk.name)
                    .put("completedAt", java.time.Instant.now().toString())
                    .put("installableAndroidPackage", true)
                    .put("gameCodeConverted", false)
                library.save(dir, report)
                installAfterJob = dir to apk.name
                Jobs.update(100, "APK ready", "Opening Android installer; this APK is a nonplayable icon stub")
            } catch (error: Exception) {
                installAfterJob = null
                forced.put("status", "FAILED")
                    .put("error", "${error.javaClass.simpleName}: ${error.message}")
                    .put("failedAt", java.time.Instant.now().toString())
                library.save(dir, report)
                Jobs.update(forced.optInt("progressPercent", 0), "APK build failed", error.message ?: error.javaClass.simpleName)
            }
        }
        selected = dir
        detail(dir)
    }

    private fun shareResultApk(dir: File, name: String) {
        val file = File(dir, name)
        if (!file.isFile) {
            Toast.makeText(this, "APK result not found", Toast.LENGTH_LONG).show()
            return
        }
        val uri = Uri.Builder().scheme("content").authority("dev.radek.conventor.results")
            .appendPath(dir.name).appendPath(file.name).build()
        val share = Intent(Intent.ACTION_SEND).apply {
            type = "application/vnd.android.package-archive"
            putExtra(Intent.EXTRA_STREAM, uri)
            clipData = android.content.ClipData.newUri(contentResolver, file.name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(share, "Share IPA-named APK result"))
    }

    private fun showText(title: String, value: String) {
        val view = TextView(this).apply { text = value; setTextIsSelectable(true); textSize = 12f; typeface = Typeface.MONOSPACE; setPadding(dp(16), dp(16), dp(16), dp(16)) }
        AlertDialog.Builder(this).setTitle(title).setView(ScrollView(this).apply { addView(view) }).setPositiveButton("Close", null).show()
    }
    @Deprecated("Framework result API retained to avoid third-party dependencies")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data?.data == null) return
        val uri = data.data!!
        if (Jobs.busy) return
        val context = applicationContext
        if (requestCode == pickerIpa) {
            Jobs.run {
                val library = Library(context)
                val (dir, report) = library.import(uri) { Jobs.update(it) }
                if (File(dir, "source.ipa").isFile) {
                    val automatic = JSONObject()
                        .put("status", if (settingsPrefs.getBoolean("auto_build_apk", true)) "PACKAGING" else "SKIPPED_BY_SETTING")
                        .put("progressPercent", 0)
                        .put("startedAt", java.time.Instant.now().toString())
                        .put("artifact", GameStubBuilder.fileName(report))
                        .put("installableAndroidPackage", settingsPrefs.getBoolean("auto_build_apk", true))
                        .put("gameCodeConverted", false)
                    report.put("automaticPackage", automatic)
                    if (!settingsPrefs.getBoolean("auto_build_apk", true)) {
                        val workflow = WorkflowProgress(65, "Analysis complete", "Automatic APK creation is off in Settings; no playable game code was emitted", "ANALYSIS_COMPLETE")
                        report.put("workflowProgress", workflow.toJson())
                        library.save(dir, report)
                        Jobs.update(workflow)
                    } else {
                        val analysisBase = report.optJSONObject("workflowProgress")?.optInt("percent", 65)?.coerceIn(0, 65) ?: 65
                        library.save(dir, report)
                        try {
                            val apk = GameStubBuilder.create(context, dir, report) { percent, message ->
                                val overall = analysisBase + (percent.coerceIn(0, 100) * (100 - analysisBase) / 100)
                                val workflow = WorkflowProgress(overall, "Building signed icon APK", message)
                                automatic.put("progressPercent", percent)
                                    .put("message", message)
                                    .put("updatedAt", java.time.Instant.now().toString())
                                report.put("workflowProgress", workflow.toJson())
                                library.save(dir, report)
                                Jobs.update(workflow)
                            }
                            automatic.put("status", "GAME_STUB_CREATED")
                                .put("progressPercent", 100)
                                .put("artifact", apk.name)
                                .put("completedAt", java.time.Instant.now().toString())
                            report.put("workflowProgress", WorkflowProgress(100, "Stub ready", "Installable icon APK created; playable game code is still 0%", "STUB_CREATED").toJson())
                            library.save(dir, report)
                            Jobs.update(WorkflowProgress(100, "Stub ready", "Installable icon APK created; playable game code is still 0%", "STUB_CREATED"))
                        } catch (error: Exception) {
                            automatic.put("status", "FAILED")
                                .put("error", "${error.javaClass.simpleName}: ${error.message}")
                                .put("failedAt", java.time.Instant.now().toString())
                            val last = report.optJSONObject("workflowProgress")?.optInt("percent", 65) ?: 65
                            val failed = WorkflowProgress(last, "APK build failed", error.message ?: error.javaClass.simpleName, "FAILED")
                            report.put("workflowProgress", failed.toJson())
                            library.save(dir, report)
                            Jobs.update(failed)
                        }
                    }
                }
            }
            wasBusy = true; home()
        } else if (requestCode == pickerApk) {
            val dir = selected ?: return
            Jobs.run { attachApk(context, uri, dir); Jobs.update("Host APK attached") }
            wasBusy = true; home()
        }
    }
    private fun installArtifact(dir: File, name: String) {
        if (!File(dir, name).isFile) {
            Toast.makeText(this, "APK result not found", Toast.LENGTH_LONG).show()
            return
        }
        if (!packageManager.canRequestPackageInstalls()) {
            pendingInstall = dir to name
            startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            return
        }
        pendingInstall = null
        val uri = Uri.Builder().scheme("content").authority("dev.radek.conventor.results")
            .appendPath(dir.name).appendPath(name).build()
        startActivity(Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, "application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
    }
    companion object {
        private fun attachApk(context: android.content.Context, uri: Uri, dir: File) {
            val temporary = File(dir, "result.pending")
            try {
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input); temporary.outputStream().use { output ->
                        val buffer = ByteArray(65536); var total = 0L
                        while (true) { val n = input.read(buffer); if (n < 0) break; total += n; require(total <= SafeZip.MAX_ARCHIVE); output.write(buffer, 0, n) }
                    }
                }
                val report = JSONObject(File(dir, "report.json").readText())
                val hash = report.getJSONObject("application").getString("sha256")
                val expectedPackage = "dev.radek.converted.p" + hash.take(20)
                var targetAbi = "arm64-v8a"
                ZipFile(temporary).use { zip ->
                    val entry = zip.getEntry("assets/conversion.json") ?: error("conversion provenance missing")
                    require(entry.size in 1..4194304)
                    val metadata = JSONObject(zip.getInputStream(entry).bufferedReader().use { it.readText() })
                    require(metadata.getString("package") == expectedPackage && metadata.getJSONObject("source").getString("sha256") == hash) { "APK does not match this IPA" }
                    targetAbi = metadata.optString("targetAbi", "arm64-v8a")
                    require(targetAbi in setOf("arm64-v8a", "armeabi-v7a")) { "unsupported converted APK ABI" }
                    require(zip.getEntry("lib/$targetAbi/libconverted.so") != null) { "converted native library missing for $targetAbi" }
                }
                @Suppress("DEPRECATION")
                val pkg = context.packageManager.getPackageArchiveInfo(temporary.path, android.content.pm.PackageManager.GET_ACTIVITIES or android.content.pm.PackageManager.GET_SIGNATURES)
                    ?: error("Android cannot parse APK")
                require(pkg.packageName == expectedPackage && pkg.activities.orEmpty().any { it.name == "dev.radek.generated.MainActivity" }) { "Android package/entry mismatch" }
                @Suppress("DEPRECATION")
                require(!pkg.signatures.isNullOrEmpty()) { "APK signer missing" }
                val result = File(dir, GameStubBuilder.fileName(report))
                if (result.exists()) require(result.delete()) { "cannot replace existing generated APK" }
                require(temporary.renameTo(result))
                report.put("hostConversion", JSONObject()
                    .put("status", "ATTACHED")
                    .put("artifact", result.name)
                    .put("package", expectedPackage)
                    .put("targetAbi", targetAbi)
                    .put("installableAndroidPackage", true)
                    .put("completedAt", java.time.Instant.now().toString()))
                Library(context).save(dir, report)
            } finally { temporary.delete() }
        }
    }
}
