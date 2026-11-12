#!/usr/bin/env python3
"""Builds emoji-data.h: every emoji, kaomoji and symbol SuperDot shows, as ONE string literal.

One string (not an array of pointers) keeps the binary small: no relocation entry per item.

  tools/gen-data.py emoji-test.txt cldr-annotations-en.xml cldr-derived-en.xml > emoji-data.h

Sources (Unicode License v3):
  https://unicode.org/Public/emoji/latest/emoji-test.txt
  https://github.com/unicode-org/cldr/blob/main/common/annotations/en.xml
  https://github.com/unicode-org/cldr/blob/main/common/annotationsDerived/en.xml
Kaomoji and symbols come from data/kaomoji.txt and data/symbols.txt in this repository.

Output: records of NUL-terminated fields (so the program reads them in place, without a copy):
  #<tab>, title, icon                     a category (tabs: e = emoji, k = kaomoji, s = symbols)
  <tab><group>, text, tone, name, keywords
where <group> is one byte ('0' + index of the category within its tab) and tone is the
emoji with \\x01 where a skin-tone modifier goes (empty when it has no skin tones).
"""
import os
import re
import sys
import unicodedata
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TONES = {0x1F3FB, 0x1F3FC, 0x1F3FD, 0x1F3FE, 0x1F3FF}
# Unicode group -> (title, icon) in the picker's category bar
GROUPS = {
    "Smileys & Emotion": ("Smileys & emotion", "😀"),
    "People & Body": ("People", "👋"),
    "Animals & Nature": ("Animals & nature", "🐻"),
    "Food & Drink": ("Food & drink", "🍔"),
    "Travel & Places": ("Travel & places", "🚗"),
    "Activities": ("Activities", "⚽"),
    "Objects": ("Objects", "💡"),
    "Symbols": ("Symbols", "💕"),
    "Flags": ("Flags", "🏁"),
}


def cps_to_str(cps):
    return "".join(chr(c) for c in cps)


def load_annotations(paths):
    kw = {}
    for p in paths:
        for a in ET.parse(p).getroot().iter("annotation"):
            if a.get("type") == "tts" or not a.text:
                continue
            kw.setdefault(a.get("cp"), a.text)
    return kw


def keywords_for(text, name, ann):
    raw = ann.get(text) or ann.get(text.replace("️", "")) or ""
    words = set(re.findall(r"[\w'’-]+", name.lower()))
    out = []
    for k in (k.strip().lower() for k in raw.split("|")):
        if not k or k in out:
            continue
        # drop keywords that are just words of the name (search looks at the name anyway)
        if all(w in words for w in re.findall(r"[\w'’-]+", k)):
            continue
        out.append(k)
    return " | ".join(out)


def emoji_records(test_path, ann):
    entries = []          # (group title, cps, name)
    toned = {}            # key without FE0F/light tone -> template
    group = None
    for line in open(test_path, encoding="utf-8"):
        m = re.match(r"# group: (.*)", line)
        if m:
            group = m.group(1).strip()
            continue
        if not line.strip() or line.startswith("#"):
            continue
        cps_s, rest = line.split(";", 1)
        status = rest.split("#", 1)[0].strip()
        if status != "fully-qualified" or group not in GROUPS:
            continue
        cps = [int(c, 16) for c in cps_s.split()]
        name = re.sub(r"^\S+\s+E\d+\.\d+\s+", "", rest.split("#", 1)[1].strip())
        tones_used = [c for c in cps if c in TONES]
        if tones_used:
            # one template per emoji: built from its "all light skin tone" variant
            if set(tones_used) == {0x1F3FB}:
                key = tuple(c for c in cps if c not in (0xFE0F, 0x1F3FB))
                toned.setdefault(key, cps_to_str(cps).replace("\U0001F3FB", "\x01"))
            continue
        entries.append((group, cps, name))
    out = []
    order = list(GROUPS)
    for group, cps, name in entries:
        text = cps_to_str(cps)
        tone = toned.get(tuple(c for c in cps if c != 0xFE0F), "")
        out.append(("e", order.index(group), text, tone, name, keywords_for(text, name, ann)))
    return out


def sections(path):
    title, icon, items = None, None, []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip():
            continue
        if line.startswith("## "):
            if title:
                yield title, icon, items
            title, icon = line[3:].split("\t") if "\t" in line else (line[3:], "")
            items = []
        elif not line.startswith("#"):
            items.append(line)
    if title:
        yield title, icon, items


def char_name(s):
    try:
        return " ".join(unicodedata.name(c).lower() for c in s)
    except ValueError:
        return ""


def main(argv):
    ann = load_annotations(argv[2:4])
    lines = []
    for title, icon in GROUPS.values():
        lines.append("#e\t%s\t%s" % (title, icon))
    for tab, g, text, tone, name, kw in emoji_records(argv[1], ann):
        lines.append("%s%c\t%s\t%s\t%s\t%s" % (tab, ord("0") + g, text, tone, name, kw))
    for tab, fname in (("k", "kaomoji.txt"), ("s", "symbols.txt")):
        for g, (title, icon, items) in enumerate(sections(os.path.join(ROOT, "data", fname))):
            lines.append("#%s\t%s\t%s" % (tab, title, icon))
            for item in items:
                if tab == "k":   # kaomoji: "text<TAB>keywords"
                    text, _, kw = item.partition("\t")
                    lines.append("k%c\t%s\t\t%s\t%s" % (ord("0") + g, text, title.lower(), kw))
                else:            # symbols: characters separated by spaces
                    for ch in item.split():
                        lines.append("s%c\t%s\t\t%s\t%s" % (ord("0") + g, ch, char_name(ch), title.lower()))
    for l in lines:
        assert "\n" not in l and l.count("\t") in (2, 4), repr(l)
    recs = [l.replace("\t", "\0") + "\0" for l in lines]
    blob = "".join(recs)
    print("/* Generated by tools/gen-data.py from Unicode emoji-test.txt and CLDR annotations"
          " (Unicode License v3). Do not edit. */")
    print("static const char EMOJI_DATA[] =")
    chunk = []
    for l in recs:
        enc = l.encode("utf-8")
        esc = "".join(chr(b) if 32 <= b < 127 and chr(b) not in '"\\?' else "\\%03o" % b for b in enc)
        chunk.append('"%s"' % esc)
    print("\n".join(chunk) + ";")
    print("#define EMOJI_DATA_LEN %d" % len(blob.encode("utf-8")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
