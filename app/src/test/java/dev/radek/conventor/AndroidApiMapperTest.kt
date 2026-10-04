package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class AndroidApiMapperTest {
    @Test fun reportsDirectAndSemanticCandidatesWithoutClaimingGeneratedCode() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "_malloc"))
            .put(JSONObject().put("name", "_objc_msgSend"))
            .put(JSONObject().put("name", "_OBJC_CLASS_" + '$' + "_UIView"))
            .put(JSONObject().put("name", "_malloc"))
        val slice = JSONObject().put("imports", imports)
        val analysis = JSONObject().put("slices", JSONArray().put(slice))
        val nodes = JSONArray().put(JSONObject().put("analysis", analysis))

        val mapping = AndroidApiMapper.analyze(nodes)

        assertEquals(3, mapping.getInt("distinctImportSymbols"))
        assertEquals(1, mapping.getInt("mappedNameCandidates"))
        assertEquals(33, mapping.getInt("candidateCoveragePercent"))
        assertEquals(1, mapping.getInt("semanticRewriteCandidates"))
        assertEquals(0, mapping.getInt("generatedTranslationCount"))
        val symbols = mapping.getJSONArray("symbols")
        val decoded = (0 until symbols.length()).map { symbols.getJSONObject(it) }
        val malloc = decoded.single { it.getString("sourceSymbol") == "_malloc" }
        val objc = decoded.single { it.getString("sourceSymbol") == "_objc_msgSend" }
        val view = decoded.single { it.getString("sourceSymbol").contains("UIView") }
        assertEquals("libc.so", malloc.getString("targetLibrary"))
        assertFalse(malloc.getBoolean("linkedOrRewritten"))
        assertEquals("UNMAPPED", objc.getString("classification"))
        assertEquals("SEMANTIC_REWRITE_CANDIDATE", view.getString("classification"))
        assertEquals("android.view.View", view.getString("targetApi"))
        assertFalse(view.getBoolean("codeGenerated"))
        assertTrue(mapping.getString("measure").contains("Neither count represents generated or playable Android code"))
    }

    @Test fun emptyImportSetDoesNotClaimPerfectCoverage() {
        val mapping = AndroidApiMapper.analyze(JSONArray())
        assertEquals(0, mapping.getInt("candidateCoveragePercent"))
        assertEquals(0, mapping.getInt("distinctImportSymbols"))
        assertEquals(0, mapping.getInt("generatedTranslationCount"))
    }
}
