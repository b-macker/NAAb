"""Flame and icicle graphs as standalone SVG, from folded stacks.

    render(folded, kind="flame" | "icicle", title=..., subtitle=..., highlight=set_of_names)

folded maps a stack (tuple of frame names, root first) to its cost. A frame's
width is its inclusive cost; frames are ordered alphabetically within a parent
(so two renders of the same run are identical and diffable), not by time. A
flame graph puts the root at the bottom, an icicle graph at the top -- same
data. Frames in `highlight` (code the index knows) are drawn in warm colours,
everything else (libraries, the system) in grey, so "our code vs the rest"
reads at a glance. Hover any frame for its full name, cost and share.

No third-party script: written here so there is nothing to download or pin.
"""

import hashlib
from xml.sax.saxutils import escape

WIDTH = 1200
FRAME_H = 16
PAD_TOP = 54
PAD_BOTTOM = 10
MIN_WIDTH_PX = 0.3


def _tree(folded):
    root = {"name": "all", "total": 0, "kids": {}}
    for stack, cost in folded.items():
        root["total"] += cost
        node = root
        for fr in stack:
            kid = node["kids"].get(fr)
            if kid is None:
                kid = node["kids"][fr] = {"name": fr, "total": 0, "kids": {}}
            kid["total"] += cost
            node = kid
    return root


def _depth(node):
    return 1 + max((_depth(k) for k in node["kids"].values()), default=0)


def _colour(name, known):
    h = int(hashlib.sha1(name.encode("utf-8", "replace")).hexdigest()[:6], 16)
    if not known:
        g = 190 + h % 40
        return "rgb(%d,%d,%d)" % (g, g, g + 5)
    r = 205 + h % 50
    g = 80 + (h >> 8) % 120
    b = 40 + (h >> 16) % 40
    return "rgb(%d,%d,%d)" % (r, g, b)


def render(folded, kind="flame", title="", subtitle="", highlight=None):
    highlight = highlight or set()
    root = _tree(folded)
    total = root["total"] or 1
    levels = _depth(root)
    height = PAD_TOP + levels * FRAME_H + PAD_BOTTOM
    scale = (WIDTH - 20) / float(total)
    rects, elided = [], 0

    def y_of(level):
        if kind == "icicle":
            return PAD_TOP + level * FRAME_H
        return height - PAD_BOTTOM - (level + 1) * FRAME_H

    def walk(node, x, level):
        nonlocal elided
        w = node["total"] * scale
        if w < MIN_WIDTH_PX:
            elided += 1
            return
        name = node["name"]
        pct = 100.0 * node["total"] / total
        label = name
        max_chars = int(w / 6.5)
        if max_chars < 3:
            label = ""
        elif len(label) > max_chars:
            label = label[: max_chars - 2] + ".."
        known = name in highlight or name == "all"
        fill = "rgb(240,240,240)" if name == "all" else _colour(name, known)
        y = y_of(level)
        tip = "%s (%s Ir, %.2f%%)" % (name, "{:,}".format(node["total"]), pct)
        rects.append('<g class="f"><title>%s</title><rect x="%.2f" y="%d" width="%.2f" height="%d" '
                     'fill="%s" rx="1"/>%s</g>'
                     % (escape(tip), 10 + x, y, max(w - 0.5, 0.1), FRAME_H - 1, fill,
                        '<text x="%.2f" y="%d">%s</text>' % (10 + x + 3, y + FRAME_H - 5, escape(label))
                        if label else ""))
        cx = x
        for kid_name in sorted(node["kids"]):
            kid = node["kids"][kid_name]
            walk(kid, cx, level + 1)
            cx += kid["total"] * scale

    walk(root, 0.0, 0)
    head = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
        'font-family="Verdana, sans-serif" font-size="11">' % (WIDTH, height, WIDTH, height),
        '<style>.f text{fill:#111;pointer-events:none} .f:hover rect{stroke:#000;stroke-width:0.8}</style>',
        '<rect width="100%" height="100%" fill="#fdfdfd"/>',
        '<text x="10" y="20" font-size="15" fill="#111">%s</text>' % escape(title),
        '<text x="10" y="38" fill="#444">%s</text>' % escape(subtitle +
                                                          ("  (%d frames narrower than %.1fpx not drawn)"
                                                           % (elided, MIN_WIDTH_PX) if elided else "")),
    ]
    return "\n".join(head + rects + ["</svg>"]) + "\n"
