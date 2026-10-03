package dev.radek.conventor

import java.io.ByteArrayOutputStream

/**
 * Binary AndroidManifest.xml (AXML) writer.
 *
 * A generated APK has no `resources.arsc`, so the manifest inlines its label as
 * a string, references a *framework* theme by id (`@android:style/...`) and
 * omits `android:icon`; the launcher icon is delivered through the runtime
 * package instead. Attribute names carry their real `android.R.attr` ids so
 * framework parsers resolve them without a resource table.
 */
object Axml {
    private const val STRING_POOL = 0x0001
    private const val RES_XML = 0x0003
    private const val RES_MAP = 0x0180
    private const val NS_START = 0x0100
    private const val NS_END = 0x0101
    private const val EL_START = 0x0102
    private const val EL_END = 0x0103
    private const val NO_ENTRY = -1

    const val TYPE_REFERENCE = 0x01
    const val TYPE_STRING = 0x03
    const val TYPE_INT_DEC = 0x10
    const val TYPE_INT_HEX = 0x11
    const val TYPE_INT_BOOLEAN = 0x12

    const val ANDROID_NS = "http://schemas.android.com/apk/res/android"

    /** `android.R.attr` ids; only framework attributes are used, never local ones. */
    val ATTR = mapOf(
        "versionCode" to 0x0101021B,
        "versionName" to 0x0101021C,
        "minSdkVersion" to 0x0101020C,
        "targetSdkVersion" to 0x01010270,
        "name" to 0x01010003,
        "label" to 0x01010001,
        "theme" to 0x01010000,
        "allowBackup" to 0x01010280,
        "extractNativeLibs" to 0x010104EA,
        "exported" to 0x01010010,
        "hardwareAccelerated" to 0x010102D3,
        "configChanges" to 0x0101001F,
        "screenOrientation" to 0x0101001E
    )

    /** `@android:style/Theme.NoTitleBar.Fullscreen` */
    const val THEME_NO_TITLE_BAR_FULLSCREEN = 0x01030248

    class Attr(val name: String, val androidId: Int?, val type: Int, val number: Int, val text: String?)

    class Element(val name: String, val attrs: List<Attr> = emptyList(), val children: List<Element> = emptyList())

    fun string(name: String, androidId: Int?, value: String) = Attr(name, androidId, TYPE_STRING, 0, value)
    fun dec(name: String, androidId: Int?, value: Int) = Attr(name, androidId, TYPE_INT_DEC, value, null)
    fun hex(name: String, androidId: Int?, value: Int) = Attr(name, androidId, TYPE_INT_HEX, value, null)
    fun bool(name: String, androidId: Int?, value: Boolean) = Attr(name, androidId, TYPE_INT_BOOLEAN, if (value) -1 else 0, null)
    fun ref(name: String, androidId: Int?, value: Int) = Attr(name, androidId, TYPE_REFERENCE, value, null)

    private fun put16(out: ByteArrayOutputStream, value: Int) {
        out.write(value and 0xFF); out.write((value ushr 8) and 0xFF)
    }

    private fun put32(out: ByteArrayOutputStream, value: Int) {
        out.write(value and 0xFF); out.write((value ushr 8) and 0xFF)
        out.write((value ushr 16) and 0xFF); out.write((value ushr 24) and 0xFF)
    }

    /** UTF-8 string pool: 1-or-2 byte char length, 1-or-2 byte byte length, NUL. */
    private fun pool(strings: List<String>): ByteArray {
        val out = ByteArrayOutputStream()
        val data = ByteArrayOutputStream()
        val offsets = mutableListOf<Int>()
        for (text in strings) {
            offsets.add(data.size())
            val raw = text.toByteArray(Charsets.UTF_8)
            fun sized(length: Int): ByteArray =
                if (length < 0x80) byteArrayOf(length.toByte())
                else byteArrayOf((0x80 or (length shr 8)).toByte(), (length and 0xFF).toByte())
            data.write(sized(text.length))
            data.write(sized(raw.size))
            data.write(raw)
            data.write(0)
        }
        val header = 28 + 4 * offsets.size
        val bytes = data.toByteArray()
        val padded = ByteArray((bytes.size + 3) / 4 * 4)
        bytes.copyInto(padded)
        put16(out, STRING_POOL); put16(out, 28)
        put32(out, header + padded.size); put32(out, offsets.size); put32(out, 0)
        put32(out, 1 shl 8)                                 // UTF-8 flag
        put32(out, header); put32(out, 0)
        for (offset in offsets) put32(out, offset)
        out.write(padded)
        return out.toByteArray()
    }

    private class Strings {
        val list = mutableListOf<String>()
        val index = mutableMapOf<String, Int>()
        val attributeIds = mutableListOf<Int>()

        fun add(text: String): Int {
            val existing = index[text]
            if (existing != null) return existing
            index[text] = list.size
            list.add(text)
            return list.size - 1
        }
    }

    fun write(root: Element): ByteArray {
        val strings = Strings()

        // Resource-map entries describe the *leading* attribute-name strings, so
        // every namespaced attribute name must be interned before anything else.
        fun declare(node: Element) {
            for (attr in node.attrs) {
                if (attr.androidId != null && attr.name !in strings.index) {
                    strings.attributeIds.add(attr.androidId)
                    strings.add(attr.name)
                }
            }
            node.children.forEach { declare(it) }
        }
        declare(root)
        val prefix = strings.add("android")
        val uri = strings.add(ANDROID_NS)

        val body = ByteArrayOutputStream()
        fun emit(node: Element) {
            for (attr in node.attrs) {
                strings.add(attr.name)
                if (attr.type == TYPE_STRING) strings.add(attr.text ?: "")
            }
            val title = strings.add(node.name)
            // Element names are never namespaced in a manifest; only attributes in
            // the android: namespace carry the namespace URI.
            put16(body, EL_START); put16(body, 16)
            put32(body, 36 + 20 * node.attrs.size); put32(body, 1); put32(body, NO_ENTRY)
            put32(body, NO_ENTRY); put32(body, title)
            put16(body, 20); put16(body, 20); put16(body, node.attrs.size)
            put16(body, 0); put16(body, 0); put16(body, 0)
            for (attr in node.attrs) {
                val raw = if (attr.type == TYPE_STRING) strings.index[attr.text ?: ""]!! else NO_ENTRY
                put32(body, attr.androidId?.let { uri } ?: NO_ENTRY)
                put32(body, strings.index[attr.name]!!)
                put32(body, raw)
                put16(body, 8)                              // Res_value size
                body.write(0)                               // res0
                body.write(attr.type and 0xFF)
                put32(body, if (attr.type == TYPE_STRING) raw else attr.number)
            }
            node.children.forEach { emit(it) }
            // type, headerSize, size, lineNumber, comment, namespace, name
            put16(body, EL_END); put16(body, 16); put32(body, 24); put32(body, 1); put32(body, NO_ENTRY)
            put32(body, NO_ENTRY); put32(body, title)
        }
        emit(root)

        val payload = ByteArrayOutputStream()
        payload.write(pool(strings.list))
        if (strings.attributeIds.isNotEmpty()) {
            put16(payload, RES_MAP); put16(payload, 8)
            put32(payload, 8 + 4 * strings.attributeIds.size)
            strings.attributeIds.forEach { put32(payload, it) }
        }
        put16(payload, NS_START); put16(payload, 16); put32(payload, 24); put32(payload, 1)
        put32(payload, NO_ENTRY); put32(payload, prefix); put32(payload, uri)
        payload.write(body.toByteArray())
        put16(payload, NS_END); put16(payload, 16); put32(payload, 24); put32(payload, 1)
        put32(payload, NO_ENTRY); put32(payload, prefix); put32(payload, uri)

        val bytes = payload.toByteArray()
        val out = ByteArrayOutputStream()
        put16(out, RES_XML); put16(out, 8); put32(out, 8 + bytes.size)
        out.write(bytes)
        return out.toByteArray()
    }

    /** The manifest of a converted APK. */
    fun manifest(
        packageName: String,
        label: String,
        entryActivity: String,
        versionName: String,
        versionCode: Int,
        minSdk: Int,
        targetSdk: Int
    ): ByteArray = write(
        Element(
            "manifest",
            listOf(
                dec("versionCode", ATTR["versionCode"], versionCode),
                string("versionName", ATTR["versionName"], versionName),
                string("package", null, packageName)
            ),
            listOf(
                Element(
                    "uses-sdk", listOf(
                        dec("minSdkVersion", ATTR["minSdkVersion"], minSdk),
                        dec("targetSdkVersion", ATTR["targetSdkVersion"], targetSdk)
                    )
                ),
                Element(
                    "application",
                    listOf(
                        string("label", ATTR["label"], label),
                        ref("theme", ATTR["theme"], THEME_NO_TITLE_BAR_FULLSCREEN),
                        bool("allowBackup", ATTR["allowBackup"], false),
                        bool("extractNativeLibs", ATTR["extractNativeLibs"], true),
                        bool("hardwareAccelerated", ATTR["hardwareAccelerated"], true)
                    ),
                    listOf(
                        Element(
                            "activity",
                            listOf(
                                string("name", ATTR["name"], entryActivity),
                                bool("exported", ATTR["exported"], true),
                                string("label", ATTR["label"], label),
                                hex("configChanges", ATTR["configChanges"], 0x04A0),
                                hex("screenOrientation", ATTR["screenOrientation"], 0)
                            ),
                            listOf(
                                Element(
                                    "intent-filter", emptyList(), listOf(
                                        Element("action", listOf(string("name", ATTR["name"], "android.intent.action.MAIN"))),
                                        Element("category", listOf(string("name", ATTR["name"], "android.intent.category.LAUNCHER")))
                                    )
                                )
                            )
                        )
                    )
                )
            )
        )
    )
}
