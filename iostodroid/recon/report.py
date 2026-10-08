"""Human readable rendering of a reconstruction report.

The markdown report is written next to the machine readable JSON so both can be
reviewed without tooling. Every section states what was proven and what remains
uncertain.
"""

from __future__ import annotations

from typing import Any


def _table(rows: list[tuple[str, str]]) -> str:
    if not rows:
        return "_none_\n"
    return "\n".join(f"| {name} | {value} |" for name, value in rows)


def _short(value: str, limit: int = 80) -> str:
    value = value.replace("\n", " ")
    return value if len(value) <= limit else value[: limit - 1] + "…"


def markdown(reconstruction: dict, application: dict | None = None) -> str:
    lines: list[str] = []
    lines.append("# IPA reconstruction report")
    lines.append("")
    lines.append(
        "Static reconstruction produced by `iostodroid`. No imported code was executed by the analyzer. "
        "This is not the original source code: names, types and "
        "control flow are recovered where metadata allows and marked uncertain elsewhere."
    )
    lines.append("")
    if application:
        lines.append("## Application")
        lines.append("")
        lines.append("| Field | Value |")
        lines.append("|---|---|")
        lines.append(_table([(k, str(v)) for k, v in application.items()]))
        lines.append("")

    for image in reconstruction.get("images", []):
        lines.append(f"## Image `{image['path']}`")
        lines.append("")
        for slice_data in image.get("slices", []):
            lines.append(f"### Architecture {slice_data['architecture']}")
            lines.append("")
            if slice_data.get("error"):
                lines.append(f"- reconstruction error: {slice_data['error']}")
            stats = slice_data.get("disassembly") or {}
            rows = [
                ("entry point", slice_data.get("entryPoint") or "unknown"),
                ("__text bytes", str(stats.get("textBytes", 0))),
                ("decoded bytes", str(stats.get("decodedBytes", 0))),
                ("instructions", str(stats.get("instructions", 0))),
                ("unknown encodings", str(stats.get("unknownInstructions", 0))),
                ("functions recovered", str(stats.get("functions", 0))),
                ("basic blocks", str(stats.get("blocks", 0))),
                ("budget truncated", "yes" if stats.get("truncated") else "no"),
            ]
            lines.append("| Metric | Value |")
            lines.append("|---|---|")
            lines.append(_table(rows))
            lines.append("")

            coverage = 0
            if stats.get("textBytes"):
                coverage = 100 * stats.get("decodedBytes", 0) / stats["textBytes"]
            lines.append(f"Reconstructed coverage of `__text`: **{coverage:.1f}%**.")
            lines.append("")

            objc_data = slice_data.get("objectiveC")
            if objc_data and (
                objc_data.get("classCount")
                or objc_data.get("categoryCount")
                or objc_data.get("protocolCount")
                or objc_data.get("selectorCount")
            ):
                lines.append("#### Objective-C")
                lines.append("")
                lines.append(
                    f"- classes: {objc_data['classCount']}, categories: {objc_data['categoryCount']}, "
                    f"protocols: {objc_data['protocolCount']}, selectors: {objc_data['selectorCount']}, "
                    f"message refs: {objc_data['messageSelectorCount']}"
                )
                for item in objc_data.get("classes", [])[:40]:
                    methods = ", ".join(m["selector"] for m in item.get("methods", [])[:12])
                    suffix = "…" if len(item.get("methods", [])) > 12 else ""
                    lines.append(
                        f"- `{item['name']}`"
                        + (f" : {item['superclass']}" if item.get("superclass") else "")
                        + (f" (Swift: {item['swift']})" if item.get("swift") else "")
                        + (f" -> {methods}{suffix}" if methods else "")
                    )
                for item in objc_data.get("categories", [])[:20]:
                    lines.append(f"- category `{item['name']}` on `{item.get('target') or '?'}`")
                for note in objc_data.get("notes", [])[:10]:
                    lines.append(f"- note: {note}")
                lines.append("")

            swift_data = slice_data.get("swift")
            if swift_data and swift_data.get("present"):
                lines.append("#### Swift")
                lines.append("")
                lines.append(
                    f"- sections: {', '.join(swift_data.get('sections', [])) or 'none'}; "
                    f"types: {swift_data.get('typeCount')}; protocols: {swift_data.get('protocolCount')}; "
                    f"mangled symbols: {swift_data.get('symbolCount')}"
                )
                for item in swift_data.get("types", [])[:40]:
                    lines.append(f"- {item['kind']} `{item['name']}` ({item['fieldCount']} fields)")
                for note in swift_data.get("notes", [])[:5]:
                    lines.append(f"- note: {note}")
                lines.append("")

            sections = slice_data.get("sections") or []
            if sections:
                lines.append("#### Sections")
                lines.append("")
                lines.append("| Segment | Section | Address | Size |")
                lines.append("|---|---|---|---|")
                for item in sections[:40]:
                    lines.append(
                        f"| {item['segment']} | {item['name']} | {item['address']} | {item['size']} |"
                    )
                if len(sections) > 40:
                    lines.append(f"\n_… {len(sections) - 40} further sections_")
                lines.append("")

            strings = slice_data.get("strings") or []
            if strings:
                lines.append("#### Strings referenced by reconstructed code")
                lines.append("")
                for value in strings[:40]:
                    lines.append(f"- {_short(repr(value), 120)}")
                if len(strings) > 40:
                    lines.append(f"- … {len(strings) - 40} further strings")
                lines.append("")

            if slice_data.get("exportCount"):
                exports = slice_data.get("exports") or []
                sample = ", ".join(f"`{name}`" for name in exports[:10])
                lines.append(
                    f"#### Exports: {slice_data['exportCount']} symbol(s)"
                    + (f" (e.g. {sample})" if sample else "")
                )
                lines.append("")

            apis_data = slice_data.get("apis") or {}
            if apis_data:
                lines.append("#### Observed API callsites (not proof of entry reachability)")
                lines.append("")
                summary = apis_data.get("summary", {})
                lines.append(
                    f"- declared imports: {apis_data.get('importCount', 0)}; symbols targeted by recovered direct calls: "
                    f"{apis_data.get('usedImportCount', 0)}; imports with no recovered direct call: "
                    f"{apis_data.get('unusedImportCount', 0)}"
                )
                lines.append(
                    f"- same-name Android native candidates: {summary.get('native', 0)}; "
                    f"requires compatibility rewrites: {summary.get('compatibility', 0)}; "
                    f"unmapped/blocked: {summary.get('blocked', 0)}"
                )
                for name, entry in (apis_data.get("linkedFrameworks") or {}).items():
                    lines.append(
                        f"- linked `{name}` (weak: {entry.get('weak')}) - "
                        f"{entry.get('symbolsUsed', 0)} statically referenced symbol(s); {entry.get('installName')}"
                    )
                for area, entry in (apis_data.get("capabilities") or {}).items():
                    lines.append(f"- capability {area}: {entry.get('status')} ({', '.join(entry['frameworks'])})")
                lines.append("")

                entry_graph = apis_data.get("entryReachability") or {}
                if entry_graph:
                    entry_function = entry_graph.get("entryFunction") or {}
                    lines.append("#### Recovered direct-call reachability from the Mach-O entry")
                    lines.append("")
                    lines.append(
                        f"- status: {entry_graph.get('status')}; entry function: "
                        f"`{entry_function.get('name', '?')}` at {entry_function.get('address', 'unknown')}; "
                        f"functions: {entry_graph.get('functionCount', 0)}; imports: {entry_graph.get('importCount', 0)}"
                    )
                    lines.append(f"- limitation: {entry_graph.get('note', 'not available')}")
                    lines.append("")

                entry_trace = apis_data.get("entryImportTrace") or {}
                if entry_trace.get("firstApplicationImportCall"):
                    image_entry = entry_trace.get("imageEntry") or {}
                    app_entry = entry_trace.get("applicationEntry") or {}
                    handoff = entry_trace.get("entryToApplicationEntry") or {}
                    first_call = entry_trace["firstApplicationImportCall"]
                    arguments = first_call.get("arguments") or {}
                    r0 = arguments.get("r0") or {}
                    r1 = arguments.get("r1") or {}
                    lines.append("#### First statically observed import call in `_main`")
                    lines.append("")
                    lines.append(
                        f"- image entry: `{image_entry.get('symbol', '?')}` at {image_entry.get('address', 'unknown')}; "
                        f"application entry: `{app_entry.get('symbol', '?')}` at {app_entry.get('address', 'unknown')}"
                    )
                    lines.append(f"- handoff: {handoff.get('status')}; {handoff.get('note', '')}")
                    lines.append(
                        f"- first direct import call: `{first_call.get('symbol')}` from "
                        f"`{first_call.get('caller')}` at {first_call.get('callsite')}, "
                        f"via stub {first_call.get('stubAddress')}"
                    )
                    if r0.get("name"):
                        lines.append(f"- statically resolved r0 receiver: `{r0['name']}`")
                    if r1.get("name"):
                        lines.append(f"- statically resolved r1 selector: `{r1['name']}`")
                    lines.append(f"- scope: {entry_trace.get('scopeNote', '')}")
                    lines.append("")

                ranked = apis_data.get("rankedImports") or []
                if ranked:
                    lines.append("#### Ranked direct import callsites across reconstructed functions")
                    lines.append("")
                    lines.append(
                        f"Counts are unique decoded call instructions across the image ({coverage:.1f}% reconstructed `__text` coverage); "
                        "undecoded/unrecovered code may change the order. This is not entry reachability or linkage evidence."
                    )
                    lines.append("")
                    lines.append("| Rank | Import | Call sites | Area | Feasibility |")
                    lines.append("|---:|---|---:|---|---|")
                    for use in ranked[:20]:
                        lines.append(
                            f"| {use.get('rank')} | `{use.get('name')}` | {use.get('callSiteCount', 0)} | "
                            f"{use.get('area')} | {use.get('feasibility')} |"
                        )
                    lines.append("")

                for use in (apis_data.get("used") or [])[:40]:
                    lines.append(
                        f"- `{use['name']}` -> {use['framework']} / {use['area']} / {use['feasibility']} "
                        f"({use.get('callSiteCount', 0)} direct callsite(s))"
                        + (f"; called from {', '.join(use['callers'][:3])}" if use.get("callers") else "")
                    )
                lines.append("")
                unused = apis_data.get("unused") or []
                if unused:
                    lines.append("#### Declared imports with no recovered direct function call")
                    lines.append("")
                    lines.append("Data relocations, including Objective-C class references, are reported separately and may still be present.")
                    for name in unused[:20]:
                        lines.append(f"- `{name}`")
                    if len(unused) > 20:
                        lines.append(f"- … {len(unused) - 20} further imports without direct calls")
                    lines.append("")

            functions = slice_data.get("functions") or []
            if functions:
                lines.append("#### Reconstructed functions")
                lines.append("")
                lines.append("```text")
                for function in functions[:40]:
                    lines.append(f"{function['signature']}  // {function['instructions']} instructions, "
                                 f"{function['blocks']} blocks, confidence {function['confidence']}")
                    for line in function["listing"][:24]:
                        lines.append(_short(line, 110))
                    if len(function["listing"]) > 24:
                        lines.append(f"    … {len(function['listing']) - 24} more statements")
                    lines.append("")
                lines.append("```")
                lines.append("")
    return "\n".join(lines) + "\n"


def summary(reconstruction: dict) -> dict:
    """Compact machine readable summary used inside the pipeline report."""
    architectures: list[str] = []
    functions = 0
    unknown = 0
    decoded = 0
    text_bytes = 0
    classes = 0
    selectors = 0
    swift_types = 0
    used_apis = 0
    entry_reachable_apis = 0
    blocked_apis = 0
    native_apis = 0
    for image in reconstruction.get("images", []):
        for slice_data in image.get("slices", []):
            architectures.append(slice_data["architecture"])
            stats = slice_data.get("disassembly") or {}
            functions += stats.get("functions", 0)
            unknown += stats.get("unknownInstructions", 0)
            decoded += stats.get("decodedBytes", 0)
            text_bytes += stats.get("textBytes", 0)
            objc_data = slice_data.get("objectiveC") or {}
            classes += objc_data.get("classCount", 0)
            selectors += objc_data.get("selectorCount", 0)
            swift_types += (slice_data.get("swift") or {}).get("typeCount", 0)
            apis_data = slice_data.get("apis") or {}
            used_apis += apis_data.get("usedImportCount", 0)
            entry_reachable_apis += (apis_data.get("entryReachability") or {}).get("importCount", 0)
            summary_apis = apis_data.get("summary", {})
            blocked_apis += summary_apis.get("blocked", 0)
            native_apis += summary_apis.get("native", 0)
    return {
        "architectures": architectures,
        "functionCount": functions,
        "unknownInstructions": unknown,
        "decodedBytes": decoded,
        "textBytes": text_bytes,
        "coverage": round(100 * decoded / text_bytes, 2) if text_bytes else 0.0,
        "objectiveCClasses": classes,
        "objectiveCSelectors": selectors,
        "swiftTypes": swift_types,
        "usedApis": used_apis,
        "observedCallImports": used_apis,
        "entryReachableApis": entry_reachable_apis,
        "blockedApis": blocked_apis,
        "nativeApis": native_apis,
    }


def blockers(reconstruction: dict) -> list[str]:
    """Surface target gaps among statically observed imports without claiming reachability."""
    messages: list[str] = []
    for image in reconstruction.get("images", []):
        for slice_data in image.get("slices", []):
            apis_data = slice_data.get("apis") or {}
            blocked = apis_data.get("byFeasibility", {}).get("blocked", [])
            compatibility = apis_data.get("byFeasibility", {}).get("compatibility", [])
            scope = f"{image['path']} ({slice_data['architecture']})"
            if blocked:
                messages.append(
                    f"{scope}: {len(blocked)} statically called symbol(s) have no Android mapping, e.g. "
                    + ", ".join(blocked[:5])
                )
            if compatibility:
                messages.append(
                    f"{scope}: {len(compatibility)} statically called symbol(s) require an unimplemented "
                    "compatibility layer, e.g. " + ", ".join(compatibility[:5])
                )
    return messages
