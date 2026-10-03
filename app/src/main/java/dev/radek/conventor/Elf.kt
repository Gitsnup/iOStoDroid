package dev.radek.conventor

/**
 * Minimal real ELF64 image writer for the converted native routine.
 *
 * The hand-emitted ARM64 bytes are wrapped in a loadable `ET_DYN` shared object
 * with a build-id note, a SYSV `.hash`, a dynamic symbol table exporting exactly
 * one JNI function, a `PT_DYNAMIC` segment and section headers, so the Android
 * linker can dlopen it and `System.loadLibrary` can resolve the entry symbol.
 *
 * Every `DT_*` value and section address is a virtual address that resolves
 * through a `PT_LOAD` mapping (inside the first segment vaddr == file offset),
 * which is what the linker actually walks. A proved closed leaf has no imports
 * and no relocations by construction, matching `radek/apk.py:validate_apk`.
 */
object Elf {
    private const val PAGE = 0x4000              // 16 KiB: loads on 4 KiB and 16 KiB devices
    private const val EM_AARCH64 = 183
    private const val EHDR_SIZE = 64
    private const val PHDR_SIZE = 56
    private const val SHDR_SIZE = 64
    private const val PHNUM = 6
    private const val SHNUM = 8
    private const val SEC_SHSTRTAB = 7
    private const val SEC_SYMTAB = 3
    private const val SEC_STRTAB = 4
    private const val SEC_TEXT = 5

    private const val PT_LOAD = 1L
    private const val PT_DYNAMIC = 2L
    private const val PT_NOTE = 4L
    private const val PT_GNU_STACK = 0x6474E551L
    private const val PF_X = 1L
    private const val PF_W = 2L
    private const val PF_R = 4L

    private const val DT_NULL = 0L
    private const val DT_HASH = 4L
    private const val DT_STRTAB = 5L
    private const val DT_SYMTAB = 6L
    private const val DT_STRSZ = 10L
    private const val DT_SYMENT = 11L
    private const val DT_SONAME = 14L

    private const val SHT_NULL = 0L
    private const val SHT_PROGBITS = 1L
    private const val SHT_HASH = 5L
    private const val SHT_DYNAMIC = 6L
    private const val SHT_NOTE = 7L
    private const val SHT_DYNSYM = 11L
    private const val SHT_STRTAB = 3L
    private const val SHF_WRITE = 1L
    private const val SHF_ALLOC = 2L
    private const val SHF_EXECINSTR = 4L

    const val SONAME = "libconverted.so"

    /** The only symbol the converted library exports. */
    const val ENTRY_SYMBOL = "Java_dev_radek_runtime_MainActivity_runNative"

    private fun alignUp(value: Int, boundary: Int): Int = (value + boundary - 1) / boundary * boundary

    private fun put16(out: ByteArray, at: Int, value: Int) {
        out[at] = (value and 0xFF).toByte()
        out[at + 1] = ((value ushr 8) and 0xFF).toByte()
    }

    private fun put32(out: ByteArray, at: Int, value: Long) {
        val v = value.toInt()
        out[at] = (v and 0xFF).toByte()
        out[at + 1] = ((v ushr 8) and 0xFF).toByte()
        out[at + 2] = ((v ushr 16) and 0xFF).toByte()
        out[at + 3] = ((v ushr 24) and 0xFF).toByte()
    }

    private fun put64(out: ByteArray, at: Int, value: Long) {
        for (shift in 0..56 step 8) out[at + shift / 8] = ((value ushr shift) and 0xFFL).toByte()
    }

    private fun put(out: ByteArray, at: Int, bytes: ByteArray) {
        bytes.copyInto(out, at)
    }

    private fun cstring(value: String): ByteArray = (value + "\u0000").toByteArray(Charsets.UTF_8)

    /**
     * @param code ARM64 instruction bytes produced by [Ir.lift]; must be a nonzero
     *   multiple of four bytes
     * @param buildId SHA-1 (20 bytes) of the generated payload, embedded verbatim
     */
    fun build(code: ByteArray, buildId: ByteArray): ByteArray {
        if (code.isEmpty() || code.size % 4 != 0) throw IllegalArgumentException("ARM64 code must be a nonzero multiple of 4 bytes")
        if (buildId.size != 20) throw IllegalArgumentException("build id must be 20 bytes")

        // .dynstr: NUL, soname, symbol name.
        val sonameOffset = 1
        val symbolOffset = 1 + SONAME.length + 1
        val dynstr = ByteArray(1 + SONAME.length + 1 + ENTRY_SYMBOL.length + 1)
        cstring(SONAME).copyInto(dynstr, sonameOffset)
        cstring(ENTRY_SYMBOL).copyInto(dynstr, symbolOffset)

        val noteOffset = alignUp(EHDR_SIZE + PHNUM * PHDR_SIZE, 16)
        val note = ByteArray(16 + 4 + buildId.size)
        put32(note, 0, 4)                                   // namesz
        put32(note, 4, buildId.size.toLong())               // descsz
        put32(note, 8, 3)                                   // NT_GNU_BUILD_ID
        put32(note, 12, 0)
        put(note, 16, cstring("GNU").copyOf(4))
        put(note, 20, buildId)

        val hashOffset = alignUp(noteOffset + note.size, 8)
        val hash = ByteArray(20)                            // nbuckets=1, nchain=2, bucket[0]=1, chain=0,0
        put32(hash, 0, 1); put32(hash, 4, 2); put32(hash, 8, 1); put32(hash, 12, 0); put32(hash, 16, 0)

        val symOffset = alignUp(hashOffset + hash.size, 8)
        val dynsym = ByteArray(48)                          // index 0 is the mandatory null entry
        put32(dynsym, 24, symbolOffset.toLong())            // st_name
        dynsym[28] = 0x12                                   // STB_GLOBAL << 4 | STT_FUNC
        dynsym[29] = 0                                      // st_other
        put16(dynsym, 30, SEC_TEXT)                         // st_shndx
        put64(dynsym, 32, PAGE.toLong())                    // st_value == .text virtual address
        put64(dynsym, 40, code.size.toLong())               // st_size

        val strOffset = symOffset + dynsym.size
        val roEnd = strOffset + dynstr.size
        val textOffset = PAGE
        val dynamicOffset = 2 * PAGE
        val dynamic = ByteArray(7 * 16)
        fun tag(index: Int, value: Long, payload: Long) {
            put64(dynamic, index * 16, value)
            put64(dynamic, index * 16 + 8, payload)
        }
        tag(0, DT_SONAME, sonameOffset.toLong())
        tag(1, DT_HASH, hashOffset.toLong())
        tag(2, DT_STRTAB, strOffset.toLong())
        tag(3, DT_SYMTAB, symOffset.toLong())
        tag(4, DT_STRSZ, dynstr.size.toLong())
        tag(5, DT_SYMENT, 24)
        tag(6, DT_NULL, 0)

        val shstrtab = ByteArray(1 + listOf(
            ".note.gnu.build-id", ".hash", ".dynsym", ".dynstr", ".text", ".dynamic", ".shstrtab"
        ).sumOf { it.length + 1 })
        val shx = mutableMapOf<String, Int>()
        var cursor = 1
        for (name in listOf(".note.gnu.build-id", ".hash", ".dynsym", ".dynstr", ".text", ".dynamic", ".shstrtab")) {
            shx[name] = cursor
            cstring(name).copyInto(shstrtab, cursor)
            cursor += name.length + 1
        }
        val shstrtabOffset = 3 * PAGE
        val sectionHeadersOffset = alignUp(shstrtabOffset + shstrtab.size, 8)
        val total = sectionHeadersOffset + SHDR_SIZE * SHNUM
        if (roEnd > textOffset || textOffset + code.size > dynamicOffset ||
            dynamicOffset + dynamic.size > shstrtabOffset
        ) {
            throw IllegalStateException("generated ELF sections overflow their segments")
        }

        val out = ByteArray(total)
        // ---- ELF header ----
        put(out, 0, byteArrayOf(0x7F, 0x45, 0x4C, 0x46, 2, 1, 1, 0))
        put16(out, 16, 3)                                   // e_type = ET_DYN
        put16(out, 18, EM_AARCH64)
        put32(out, 20, 1)                                   // e_version
        put64(out, 24, 0)                                   // e_entry: a library has no process entry
        put64(out, 32, EHDR_SIZE.toLong())                  // e_phoff
        put64(out, 40, sectionHeadersOffset.toLong())       // e_shoff
        put32(out, 48, 0)                                   // e_flags
        put16(out, 52, EHDR_SIZE)
        put16(out, 54, PHDR_SIZE)
        put16(out, 56, PHNUM)
        put16(out, 58, SHDR_SIZE)
        put16(out, 60, SHNUM)
        put16(out, 62, SEC_SHSTRTAB)

        // ---- program headers ----
        fun phdr(
            index: Int, type: Long, flags: Long, offset: Long, vaddr: Long,
            filesz: Long, memsz: Long, alignment: Long
        ) {
            val at = EHDR_SIZE + index * PHDR_SIZE
            put32(out, at, type); put32(out, at + 4, flags)
            put64(out, at + 8, offset); put64(out, at + 16, vaddr); put64(out, at + 24, vaddr)
            put64(out, at + 32, filesz); put64(out, at + 40, memsz); put64(out, at + 48, alignment)
        }
        phdr(0, PT_LOAD, PF_R, 0, 0, roEnd.toLong(), roEnd.toLong(), PAGE.toLong())
        phdr(1, PT_LOAD, PF_R or PF_X, textOffset.toLong(), PAGE.toLong(), code.size.toLong(), code.size.toLong(), PAGE.toLong())
        phdr(2, PT_LOAD, PF_R or PF_W, dynamicOffset.toLong(), (2 * PAGE).toLong(),
            dynamic.size.toLong(), dynamic.size.toLong(), PAGE.toLong())
        phdr(3, PT_DYNAMIC, PF_R or PF_W, dynamicOffset.toLong(), (2 * PAGE).toLong(),
            dynamic.size.toLong(), dynamic.size.toLong(), 8)
        phdr(4, PT_NOTE, PF_R, noteOffset.toLong(), noteOffset.toLong(), note.size.toLong(), note.size.toLong(), 4)
        phdr(5, PT_GNU_STACK, PF_R or PF_W, 0, 0, 0, 0, 16)

        // ---- contents ----
        put(out, noteOffset, note)
        put(out, hashOffset, hash)
        put(out, symOffset, dynsym)
        put(out, strOffset, dynstr)
        put(out, textOffset, code)
        put(out, dynamicOffset, dynamic)
        put(out, shstrtabOffset, shstrtab)

        // ---- section headers ----
        fun shdr(
            index: Int, name: Int, type: Long, flags: Long, addr: Long, offset: Long,
            size: Long, link: Int, info: Int, alignment: Long, entsize: Long
        ) {
            val at = sectionHeadersOffset + index * SHDR_SIZE
            put32(out, at, name.toLong()); put32(out, at + 4, type)
            put64(out, at + 8, flags); put64(out, at + 16, addr); put64(out, at + 24, offset)
            put64(out, at + 32, size); put32(out, at + 40, link.toLong()); put32(out, at + 44, info.toLong())
            put64(out, at + 48, alignment); put64(out, at + 56, entsize)
        }
        shdr(0, 0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0)
        shdr(1, shx[".note.gnu.build-id"]!!, SHT_NOTE, SHF_ALLOC, noteOffset.toLong(), noteOffset.toLong(),
            note.size.toLong(), 0, 0, 4, 0)
        shdr(2, shx[".hash"]!!, SHT_HASH, SHF_ALLOC, hashOffset.toLong(), hashOffset.toLong(),
            hash.size.toLong(), SEC_SYMTAB, 0, 8, 4)
        shdr(3, shx[".dynsym"]!!, SHT_DYNSYM, SHF_ALLOC, symOffset.toLong(), symOffset.toLong(),
            dynsym.size.toLong(), SEC_STRTAB, 1, 8, 24)
        shdr(4, shx[".dynstr"]!!, SHT_STRTAB, SHF_ALLOC, strOffset.toLong(), strOffset.toLong(),
            dynstr.size.toLong(), 0, 0, 1, 0)
        shdr(5, shx[".text"]!!, SHT_PROGBITS, SHF_ALLOC or SHF_EXECINSTR, PAGE.toLong(), textOffset.toLong(),
            code.size.toLong(), 0, 0, 16, 0)
        shdr(6, shx[".dynamic"]!!, SHT_DYNAMIC, SHF_WRITE or SHF_ALLOC, (2 * PAGE).toLong(),
            dynamicOffset.toLong(), dynamic.size.toLong(), SEC_STRTAB, 0, 8, 16)
        shdr(7, shx[".shstrtab"]!!, SHT_STRTAB, 0, 0, shstrtabOffset.toLong(), shstrtab.size.toLong(), 0, 0, 1, 0)
        return out
    }
}
