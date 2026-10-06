"""Pointer discovery and target audit for the game image.

`disasm` proposes pointer candidates from code flow; this module disposes.
Every in-image value that code treats as an address is audited against a map
of known pointer targets (symbols, strings, pools, code entries, relocation
slots, structured sections). The outcome per site is exactly one of:

- SLIDE: an absolute address that must move with the image rebasing delta.
- IMPORT: a dyld-owned slot (lazy/non-lazy pointers, stub data words) that
  the converter's loader rewrites with the Android address; never slid.
- VALUE: an integer/offset that must be left alone.
- REVIEW: undecidable from structure; blocks conversion until a human
  extends the structure knowledge or records a demotion with evidence.

Fail-closed: anything that looks like a pointer but aims nowhere plausible
is reported, never silently slid or dropped. Strong uses (dereference or
branch through the value) prove addressness by themselves — a faulting game
would not run — so a strong use with an implausible target is an internal
error, while weak uses (store, call argument, return) are demoted to VALUE
when their target is implausible.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from . import disasm, lsda, macho, objc_meta


@dataclass
class Site:
    address: int  # address of the pointer word
    value: int  # word content at parse time
    kind: str  # SLIDE | IMPORT | VALUE
    evidence: str = ""


@dataclass
class Review:
    address: int
    value: int
    context: str
    evidence: str = ""


@dataclass
class PointerModel:
    sites: dict = field(default_factory=dict)  # address -> Site
    reviews: list = field(default_factory=list)  # Review, must end empty
    demotions: list = field(default_factory=list)  # (address, value, evidence)
    stats: dict = field(default_factory=dict)


def collect_literals(functions: dict) -> dict:
    """Deduplicate literal loads by instruction address.

    Landing pads overlap their parents, so one load can be analyzed twice;
    both analyses must agree.
    """
    literals = {}
    for func in functions.values():
        for literal in func.literals:
            prior = literals.get(literal.address)
            if prior is None:
                literals[literal.address] = literal
            elif (prior.verdict, prior.value, prior.literal_address) != (
                literal.verdict, literal.value, literal.literal_address
            ):
                raise RuntimeError(
                    f"literal at {literal.address:#x} analyzed twice with "
                    f"different results: {prior.verdict} vs {literal.verdict}"
                )
    return literals


# ---------------------------------------------------------------------------
# Known-target map

@dataclass
class TargetMap:
    symbols: dict = field(default_factory=dict)  # addr -> name
    cstrings: set = field(default_factory=set)
    pools: set = field(default_factory=set)  # referenced literal-pool words
    entries: dict = field(default_factory=dict)  # addr -> function name
    branch_targets: set = field(default_factory=set)
    code_words: set = field(default_factory=set)
    stubs: dict = field(default_factory=dict)  # addr -> import name
    reloc_slots: set = field(default_factory=set)
    jump_entries: set = field(default_factory=set)  # confirmed table entries
    # Defined non-stab symbols grouped by section index: [(value, name)].
    by_section: dict = field(default_factory=dict)
    except_tab: tuple = ()  # (low, high) of __gcc_except_tab


def containing_symbol(image: macho.Image, targets: TargetMap, addr: int):
    """Nearest defined symbol at or below `addr` in the same section.

    Returns (name, delta, size) where size runs to the next defined symbol
    in the section (or the section end), or None when `addr` is past the
    end of the last object or in no known section.
    """
    section = image.section_at(addr)
    if section is None:
        return None
    syms = targets.by_section.get(section.index, [])
    prev = None
    for value, name in syms:
        if value > addr:
            break
        prev = (value, name)
    if prev is None:
        return None
    value, name = prev
    following = [value2 for value2, _ in syms if value2 > value]
    end = min(following) if following else section.address + section.size
    if addr >= end:
        return None
    return (name, addr - value, end - value)


def build_target_map(image: macho.Image, functions: dict) -> TargetMap:
    targets = TargetMap()
    for symbol in image.symbols:
        if not symbol.is_stab and symbol.value:
            targets.symbols.setdefault(symbol.value, symbol.name)
            if symbol.n_sect:
                targets.by_section.setdefault(symbol.n_sect, []).append(
                    (symbol.value, symbol.name))
    for syms in targets.by_section.values():
        syms.sort()
    try:
        except_tab = image.section_named("__DATA", "__gcc_except_tab")
        inner = [
            symbol for symbol in image.symbols
            if not symbol.is_stab and symbol.value
            and except_tab.address <= symbol.value
            < except_tab.address + except_tab.size
        ]
        foreign = [symbol for symbol in inner
                   if not symbol.name.startswith("GCC_except_table")]
        if foreign:
            raise RuntimeError(
                "except_tab carries non-LSDA symbols: "
                + ", ".join(symbol.name for symbol in foreign[:5]))
        targets.except_tab = (except_tab.address,
                              except_tab.address + except_tab.size)
    except macho.MachOError:
        pass
    cstring = image.section_named("__TEXT", "__cstring")
    blob = image.read(cstring.address, cstring.size)
    start = 0
    for i, byte in enumerate(blob):
        if byte == 0:
            if i > start:
                targets.cstrings.add(cstring.address + start)
            start = i + 1
    for func in functions.values():
        targets.entries.setdefault(func.address, func.name)
        targets.branch_targets |= func.branches | func.calls
        targets.code_words |= func.code_words
        for literal in func.literals:
            if literal.literal_address % 4 == 0:
                targets.pools.add(literal.literal_address)
        for base, count in func.jump_tables:
            for i in range(count):
                targets.jump_entries.add(base + 4 * i)
    try:
        targets.stubs = disasm.stub_targets(image)
    except macho.MachOError:
        pass
    for reloc in image.external_relocations:
        if reloc.length == 2 and not reloc.pcrel:
            targets.reloc_slots.add(reloc.address)
    return targets


def classify_target(image: macho.Image, targets: TargetMap, value: int) -> str:
    """Classify a pointer target. Plausible classes sort first in the name."""
    if value in targets.symbols:
        return f"ok:symbol:{targets.symbols[value][:40]}"
    if value in targets.cstrings:
        return "ok:cstring"
    if value in targets.entries:
        return f"ok:entry:{targets.entries[value][:40]}"
    if value in targets.branch_targets:
        return "ok:branch-target"
    if value in targets.stubs:
        return f"ok:stub:{targets.stubs[value]}"
    if value in targets.pools:
        return "ok:pool-word"
    if value in targets.reloc_slots:
        return "ok:reloc-slot"
    if value in targets.jump_entries:
        return "ok:jump-entry"
    # Itanium vtable pointers aim 8 past the vtable symbol (past offset-to-top
    # and the RTTI slot); SjLj context LSDA slots aim just past the LSDA label.
    for back in (8, 3, 2):
        name = targets.symbols.get(value - back, "")
        if name.startswith(("_ZTV", "_ZTI", "_ZTS", "_ZTC", "_ZTH")):
            return f"ok:vtable:{name[:44]}"
        if name.startswith("GCC_except_table"):
            return f"ok:lsda:{name[:40]}"
    if targets.except_tab and targets.except_tab[0] <= value < targets.except_tab[1]:
        # Proven above to carry LSDAs only.
        return "ok:lsda-region"
    section = image.section_at(value)
    if section is not None and section.name != "__text":
        # Mid-object data pointers (array elements, table fields, string
        # tails) are legitimate, so containment in a known data object
        # proves the target. Inside __text the same shape would be a jump
        # into the middle of an instruction stream, which is never valid.
        contained = containing_symbol(image, targets, value)
        if contained is not None:
            name, delta, _ = contained
            return f"ok:global:{name[:40]}+{delta:#x}"
    if section is None:
        return "bad:outside-image"
    if section.segment == "__DATA" or (
        section.segment == "__TEXT" and section.name == "__const"
    ):
        return f"maybe:data:{section.segment}.{section.name}"
    if section.name == "__cstring":
        return "ok:cstring-mid"
    if value in targets.code_words:
        return "bad:code-mid"
    if section.name == "__symbol_stub4":
        return "bad:stub-mid"
    if section.name == "__text":
        return "maybe:text-gap"
    return f"bad:{section.segment}.{section.name}"


# ---------------------------------------------------------------------------
# Literal audit

def _use_strength(literal) -> str:
    evidence = literal.evidence
    if "dereferenced" in evidence or "branched" in evidence:
        return "strong"
    if "stored to memory" in evidence or "passed to call" in evidence \
            or "returned to caller" in evidence:
        return "weak"
    return "none"


def _bucket_outcome(bucket: dict, kind: str, payload) -> None:
    bucket[kind].append(payload)


def audit_literals(image: macho.Image, functions: dict, targets: TargetMap,
                   model: PointerModel) -> None:
    """Audit every deduplicated literal load.

    Loads sharing one pool word resolve by precedence: any load proving
    an address (SLIDE) wins over reviews, which win over VALUE. Demotions
    are recorded only for pools finalizing as VALUE, so a strong load is
    never overwritten by a later weak read of the same word.
    """
    buckets = {}

    def bucket_for(pool: int) -> dict:
        return buckets.setdefault(
            pool, {"value": None, "slide": [], "review": [],
                   "unsure": [], "demote": []})

    for literal in sorted(collect_literals(functions).values(),
                          key=lambda entry: entry.address):
        value = literal.value
        pool = literal.literal_address
        bucket = bucket_for(pool)
        bucket["value"] = value
        target = classify_target(image, targets, value)
        # An exact object start is an address no matter how it is used:
        # compared/computed addresses (sentinel checks like `node ==
        # &dummynode`, registration tables) must relocate exactly like
        # dereferenced ones. Only small pool-word hits keep their
        # use-based verdict (proven integer coincidences: 0x8d40, 0x4180).
        if _is_exact_target(target) and not (
                target == "ok:pool-word" and value < 0x10000):
            _bucket_outcome(
                bucket, "slide",
                f"exact-address:{target}; {literal.evidence[:100]}")
            continue
        if literal.verdict in ("OUT_OF_RANGE", "VALUE"):
            _bucket_outcome(
                bucket, "unsure",
                f"use:{literal.verdict}:{literal.evidence[:80]}")
            continue
        strength = _use_strength(literal)
        if literal.verdict == "AMBIGUOUS" or strength == "none":
            # A value used as a plain (commutative, unscaled) address index
            # is either the array base or an infeasible index: only a proven
            # data-table target makes it a base. Code-range targets cannot
            # be reached this way (no PC-relative scaled form exists), so
            # they demote.
            if "plain address index" in literal.evidence:
                section = image.section_at(value)
                in_data = section is not None and (
                    section.segment == "__DATA"
                    or (section.segment == "__TEXT" and section.name == "__const")
                )
                if in_data and target.startswith("ok:"):
                    _bucket_outcome(
                        bucket, "slide",
                        f"indexed-table-base:{target}; {literal.evidence[:80]}")
                elif target.startswith("ok:"):
                    _bucket_outcome(
                        bucket, "review",
                        (f"ambiguous-use@{literal.address:#x}",
                         f"target={target}; {literal.evidence[:120]}"))
                else:
                    _bucket_outcome(
                        bucket, "unsure",
                        f"demoted:plain-index+unproven-target:{target}; "
                        f"{literal.evidence[:100]}")
                    _bucket_outcome(
                        bucket, "demote",
                        f"plain-index@{literal.address:#x}:{target}")
                continue
            # "ok:pool-word"/"ok:branch-target" are soft evidence: small
            # integers collide with pool addresses and labels by chance in
            # a dense __text (proven by 0x8d40 = GL_RENDERBUFFER and 0x84c0
            # = GL_TEXTURE0). Only hard target proof keeps an ambiguous
            # value under review; anything else is an integer overlapping
            # the image range.
            if (value < 0x10000
                    and target in ("ok:pool-word", "ok:branch-target")):
                _bucket_outcome(
                    bucket, "unsure",
                    f"demoted:small-int-coincidence:{target}; "
                    f"{literal.evidence[:100]}")
                _bucket_outcome(
                    bucket, "demote",
                    f"small-int@{literal.address:#x}:{target}")
            elif target.startswith("ok:") and target != "ok:pool-word":
                _bucket_outcome(
                    bucket, "review",
                    (f"ambiguous-use@{literal.address:#x}",
                     f"target={target}; {literal.evidence[:120]}"))
            else:
                # A value with conflicting use evidence is slid only when
                # its target is proven; anything else is an integer that
                # happens to overlap the image range.
                _bucket_outcome(
                    bucket, "unsure",
                    f"demoted:conflicting-use+unproven-target:{target}; "
                    f"{literal.evidence[:100]}")
                _bucket_outcome(
                    bucket, "demote",
                    f"ambiguous@{literal.address:#x}:{target}")
            continue
        if strength == "strong":
            if target.startswith("bad:"):
                _bucket_outcome(
                    bucket, "review",
                    (f"STRONG-USE-BAD-TARGET@{literal.address:#x}",
                     f"target={target}; {literal.evidence[:120]}"))
            else:
                _bucket_outcome(
                    bucket, "slide", f"{target}; {literal.evidence[:100]}")
            continue
        # Weak use: slide only at plausible targets, demote otherwise.
        # A weak use aiming at a pool word is reviewed, not auto-slid: the
        # coincidence rate with small integers is too high (see above).
        if target == "ok:pool-word" and value < 0x10000:
            _bucket_outcome(
                bucket, "unsure",
                f"demoted:small-int-coincidence:{target}; "
                f"{literal.evidence[:100]}")
            _bucket_outcome(
                bucket, "demote",
                f"small-int@{literal.address:#x}:{target}")
        elif target == "ok:branch-target" and value < 0x10000:
            _bucket_outcome(
                bucket, "unsure",
                f"demoted:small-int-coincidence:{target}; "
                f"{literal.evidence[:100]}")
            _bucket_outcome(
                bucket, "demote",
                f"small-int@{literal.address:#x}:{target}")
        elif target == "ok:pool-word":
            _bucket_outcome(
                bucket, "review",
                (f"weak-use-pool-target@{literal.address:#x}",
                 f"target={target}; {literal.evidence[:120]}"))
        elif target.startswith("ok:"):
            _bucket_outcome(
                bucket, "slide", f"{target}; {literal.evidence[:100]}")
        elif target.startswith("maybe:data:"):
            _bucket_outcome(
                bucket, "review",
                (f"weak-use-data-target@{literal.address:#x}",
                 f"target={target}; {literal.evidence[:120]}"))
        else:
            _bucket_outcome(
                bucket, "unsure",
                f"demoted:weak-use+implausible-target:{target}; "
                f"{literal.evidence[:100]}")
            _bucket_outcome(
                bucket, "demote",
                f"weak@{literal.address:#x}:{target}")
    for pool, bucket in buckets.items():
        value = bucket["value"]
        if bucket["slide"]:
            evidence = sorted(
                bucket["slide"],
                key=lambda item: (0 if "dereferenced" in item
                                  or "branched" in item else 1))[0]
            model.sites[pool] = Site(pool, value, "SLIDE", evidence)
        elif bucket["review"]:
            for context, evidence in bucket["review"]:
                model.reviews.append(Review(pool, value, context, evidence))
        else:
            model.sites[pool] = Site(pool, value, "VALUE", bucket["unsure"][0])
            for note in bucket["demote"]:
                model.demotions.append((pool, value, note))


# ---------------------------------------------------------------------------
# Gap and section scans

def scan_text_gaps(image: macho.Image, functions: dict, targets: TargetMap,
                   model: PointerModel) -> None:
    """Classify every __text word that is neither code nor a known pool word."""
    text = image.section_named("__TEXT", "__text")
    code = set()
    for func in functions.values():
        code |= func.code_words
    pools = set(targets.pools)
    addr = text.address
    end = text.address + text.size
    runs = []  # (start, [words]) of consecutive in-image-valued gap words
    current = None
    while addr < end:
        if addr in code or addr in pools or addr in model.sites:
            if current:
                runs.append(current)
                current = None
        else:
            try:
                word = image.read_u32(addr)
            except macho.MachOError:
                word = None
            if word is not None and disasm._points_into_image(image, word):
                if current is None:
                    current = (addr, [])
                current[1].append((addr, word))
            else:
                if current:
                    runs.append(current)
                    current = None
        addr += 4
    if current:
        runs.append(current)
    model.stats["gap_runs"] = len(runs)
    model.stats["gap_words"] = sum(len(words) for _, words in runs)
    for start, words in runs:
        # Jump-table run: consecutive words that are all code entries.
        if len(words) >= 2 and all(
            classify_target(image, targets, word).startswith(
                ("ok:entry", "ok:branch-target"))
            for _, word in words
        ):
            for slot, word in words:
                model.sites[slot] = Site(
                    slot, word, "SLIDE", f"jump-table-run@{start:#x}")
                targets.jump_entries.add(slot)
            continue
        for slot, word in words:
            if slot in model.sites:
                continue
            if slot in targets.jump_entries:
                model.sites[slot] = Site(slot, word, "SLIDE",
                                         f"jump-table-entry@{start:#x}")
                continue
            target = classify_target(image, targets, word)
            if _is_exact_target(target) or target.startswith("maybe:data:"):
                model.sites[slot] = Site(slot, word, "SLIDE",
                                         f"gap-pool:{target}")
            elif target.startswith("bad:"):
                model.sites[slot] = Site(slot, word, "VALUE",
                                         f"gap-data:implausible-target:{target}")
            elif target == "maybe:text-gap" and abs(word - slot) > 0x10000:
                # Dead pool integer: no mechanism produces far code
                # pointers into gaps (callbacks are entries/symbols,
                # tables are near), and nothing references this word.
                model.sites[slot] = Site(slot, word, "VALUE",
                                         f"gap-dead-int:{target}")
            elif target.startswith("ok:branch-target"):
                model.reviews.append(Review(
                    slot, word, f"lone-code-pointer@{start:#x}",
                    f"target={target}"))
            else:
                model.reviews.append(Review(
                    slot, word, f"text-gap@{start:#x}", f"target={target}"))


def scan_except_tab(image: macho.Image, model: PointerModel) -> None:
    """Classify `__gcc_except_tab` from the parsed LSDA tables.

    SjLj call-site/action tables are uleb/sleb byte streams (landing-pad
    *indices*, action offsets, filters) that only coincide with image
    addresses when read as words; they never slide. The only absolute
    pointers are absolute-encoded LPStarts and type-table typeinfo slots.
    """
    section = image.section_named("__DATA", "__gcc_except_tab")
    tables, _, problems = lsda.parse_all(image)
    for address, error in problems:
        model.reviews.append(Review(address, 0, "lsda-unparsed", error))
        return
    pointer_words = {}
    for table in tables.values():
        pointer_words.update(table.type_entries)
        if table.lpstart_addr is not None:
            pointer_words[table.lpstart_addr] = image.read_u32(table.lpstart_addr)
    addr = section.address
    end = section.address + section.size
    while addr < end:
        word = image.read_u32(addr)
        if addr in pointer_words:
            model.sites[addr] = Site(addr, word, "SLIDE", "lsda-typeinfo")
        else:
            model.sites[addr] = Site(addr, word, "VALUE", "lsda-index/action-bytes")
        addr += 4
    model.stats["lsda_tables"] = len(tables)


def _is_exact_target(target: str) -> bool:
    """True when `target` names an exact object start (a proven pointer).

    Mid-object (`+delta`), mid-code and branch-target shapes are packed
    table integers until a holder proves otherwise; `ok:cstring-mid` stays
    exact because `&str[i]` (even the empty string at its NUL) is a real
    pointer idiom (`luaL_libsToOpen[0].name`).
    """
    return (target.startswith("ok:") and "+" not in target
            and "branch-target" not in target)


def _holder_stats(image: macho.Image, targets: TargetMap, sections) -> dict:
    """Map holder name -> (words, exact_count) over generic-scan sections."""
    stats = {}
    for section in sections:
        for off in range(0, section.size, 4):
            slot = section.address + off
            contained = containing_symbol(image, targets, slot)
            if contained is None:
                continue
            name, _, size = contained
            words, exact = stats.get(name, (size // 4, 0))
            word = image.read_u32(slot)
            if disasm._points_into_image(image, word) and _is_exact_target(
                    classify_target(image, targets, word)):
                exact += 1
            stats[name] = (words, exact)
    return stats


def scan_data_sections(image: macho.Image, targets: TargetMap,
                       model: PointerModel) -> None:
    """Classify pointer-sized words in data sections by structure."""
    stubs = image.section_named("__TEXT", "__symbol_stub4")
    stride = stubs.reserved2 or 12
    stub_names = targets.stubs
    for i in range(stubs.size // stride):
        slot = stubs.address + i * stride + 8
        model.sites[slot] = Site(
            slot, image.read_u32(slot), "IMPORT",
            f"stub-data-word:{stub_names.get(stubs.address + i * stride, '?')}")
    model.stats["stub_data_words"] = stubs.size // stride
    scan_except_tab(image, model)
    objc = objc_meta.parse(image)
    for address, message in objc.problems:
        model.reviews.append(Review(address, 0, "objc-unparsed", message))
    imps = {imp for _, _, _, imp in objc.methods if imp}
    for address, (kind, evidence) in objc.sites.items():
        value = image.read_u32(address)
        if kind == "SLIDE" and value in imps:
            target = classify_target(image, targets, value)
            if not target.startswith(("ok:symbol", "ok:entry", "ok:branch-target")):
                model.reviews.append(Review(
                    address, value, "objc-imp", f"target={target}; {evidence}"))
                continue
        model.sites[address] = Site(address, value, kind, f"objc:{evidence}")
    model.stats["objc_methods"] = len(objc.methods)
    generic_sections = []
    for section in image.sections:
        if section.segment == "__DATA":
            if section.name in ("__bss", "__common", "__gcc_except_tab"):
                continue  # loader-zeroed sections have no file content
            if "__objc" in section.name:
                continue  # structurally parsed above
        elif section.segment == "__TEXT":
            # This old binary keeps loader tables in __TEXT; __text pools
            # and stubs/cstrings are claimed by earlier passes instead.
            if section.name in ("__text", "__symbol_stub4", "__cstring"):
                continue
        else:
            continue
        generic_sections.append(section)
    holders = _holder_stats(image, targets, generic_sections)
    for section in generic_sections:
        if section.name in ("__nl_symbol_ptr", "__la_symbol_ptr"):
            for off in range(0, section.size, 4):
                slot = section.address + off
                model.sites[slot] = Site(
                    slot, image.read_u32(slot), "IMPORT",
                    f"{section.name}[{off // 4}]")
            continue
        if section.name == "__mod_init_func":
            for off in range(0, section.size, 4):
                slot = section.address + off
                word = image.read_u32(slot)
                target = classify_target(image, targets, word)
                if target.startswith("ok:") or word in targets.code_words:
                    model.sites[slot] = Site(slot, word, "SLIDE",
                                             f"mod-init:{target}")
                else:
                    model.reviews.append(Review(
                        slot, word, "mod-init", f"target={target}"))
            continue
        if section.name == "__cfstring":
            for off in range(0, section.size, 16):
                base = section.address + off
                isa = image.read_u32(base)
                cstr = image.read_u32(base + 8)
                model.sites[base] = Site(base, isa, "IMPORT", "cfstring.isa")
                model.sites[base + 8] = Site(base + 8, cstr, "SLIDE",
                                             "cfstring.chars")
            continue
        # Generic section: reloc slots are loader-bound pointers and exact
        # object starts are proven pointers (no packed-int exact hit exists
        # in this binary). Any other in-image word is a packed table integer
        # unless its holder is an exact-rich struct. Proven struct holders:
        # FileInputStream::sm_fileBundle (11 exact) and libmad's _C.68.5500
        # (8 exact); pure tables (_tabNN, inflate/huffman/ctype/gr tables,
        # typeinfo names) carry 0-2 exact words.
        for off in range(0, section.size, 4):
            slot = section.address + off
            if slot in model.sites:
                continue
            word = image.read_u32(slot)
            if not disasm._points_into_image(image, word):
                continue
            target = classify_target(image, targets, word)
            if slot in targets.reloc_slots:
                model.sites[slot] = Site(slot, word, "SLIDE",
                                         f"reloc-slot:{target}")
            elif _is_exact_target(target):
                model.sites[slot] = Site(slot, word, "SLIDE",
                                         f"data:{section.name}:exact:{target}")
            elif target.startswith("bad:"):
                model.sites[slot] = Site(slot, word, "VALUE",
                                         f"data:implausible-target:{target}")
            else:
                contained = containing_symbol(image, targets, slot)
                if contained is None:
                    model.reviews.append(Review(
                        slot, word, section.name,
                        f"holderless:{target}"))
                    continue
                words, exact = holders.get(contained[0], (0, 0))
                if words >= 8 and exact >= 3:
                    model.sites[slot] = Site(
                        slot, word, "SLIDE",
                        f"data:{section.name}:struct-mid:{target}")
                else:
                    model.sites[slot] = Site(
                        slot, word, "VALUE",
                        f"data:table-int:{target}")


def build_model(image: macho.Image, functions: dict) -> PointerModel:
    """Run the full pointer pipeline: audit, gap scan, section scan."""
    model = PointerModel()
    targets = build_target_map(image, functions)
    audit_literals(image, functions, targets, model)
    scan_text_gaps(image, functions, targets, model)
    scan_data_sections(image, targets, model)
    from collections import Counter
    model.stats["sites"] = dict(Counter(
        site.kind for site in model.sites.values()))
    model.stats["reviews"] = len(model.reviews)
    model.stats["demotions"] = len(model.demotions)
    return model
