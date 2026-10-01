#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render the reference documentation into pages of the website.

    python3 tooling/site/build-docs.py

`docs/scripting.md` and `docs/mqtt.md` are the source of truth and stay that
way. They are what somebody reads in the repository, what a pull request
reviews, and what ships with the tarball; the website is a second audience for
the same words, not a second copy of them.

So the pages are generated, and like `site/shop/index.html` the output is not
committed. A generated file in the tree is a file somebody edits by hand once,
and from then on the two versions disagree about which is true.

**This is deliberately not a Markdown library.** ADR 0012 keeps the core free
of dependencies, and the same reasoning applies to anything CI runs: a site
build that pip-installs on every run is a site build that breaks the day an
index is down. What it understands is the subset these two documents actually
use - headings, paragraphs, fenced code, tables, flat lists, and inline code,
bold and links. Anything outside that subset is a parse error rather than
silently mangled output, because the failure mode of a lenient renderer is a
page that looks fine and says something else.
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Ownership is infrastructure, per the naming rule - the site derives it the
# same way CI does rather than baking a handle into a generator.
REPO = os.environ.get("GITHUB_REPOSITORY", "galadril/Stipple")

PAGES = [
    {
        "source": "docs/scripting.md",
        "out": "site/scripting/index.html",
        "nav": "scripting",
        "title": "Scripting",
        "blurb": "Berry on the panel: what a script can draw, read and reach, "
                 "and the budget it runs under.",
    },
    {
        "source": "docs/mqtt.md",
        "out": "site/mqtt/index.html",
        "nav": "mqtt",
        "title": "MQTT",
        "blurb": "Topics, commands and credentials for the broker bridge, "
                 "answered by the same router as HTTP.",
    },
]

# label, href relative to a page one directory down, nav key
NAV = [
    ("Install", "../#install", "install"),
    ("Library", "../shop/", "shop"),
    ("Scripting", "../scripting/", "scripting"),
    ("MQTT", "../mqtt/", "mqtt"),
    ("API", "../api/", "api"),
]


def escape(text):
    return (text.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;"))


def rewrite(href):
    """Point a repository-relative link at something the website has."""
    # The docs link to the shop by its published URL, which would send a
    # reader on a preview server out to the live site mid-sentence.
    live = "https://" + REPO.split("/")[0] + ".github.io/"
    if href.startswith(live):
        rest = href[len(live):].split("/", 1)
        if len(rest) == 2 and rest[1]:
            return "../" + rest[1]
    if href.startswith("../") or href.startswith("./"):
        target = href.lstrip("./")
        if target.endswith(".md"):
            target = target[:-3]
            for page in PAGES:
                if page["source"] == "docs/" + target + ".md":
                    return "../" + page["nav"] + "/"
            return "https://github.com/%s/blob/main/docs/%s.md" % (REPO, target)
        return "https://github.com/%s/tree/main/%s" % (REPO, target.rstrip("/"))
    return href


# Code spans, links and bold, matched in one pass.
#
# One pass rather than three, because all three nest. The scripting document
# has a link whose text is a code span - [`scripts/`](../scripts/) - and bold
# that runs across one: **Check `time_known()` first.** Handling them in
# sequence breaks whichever goes second, and it breaks it silently: taking
# code spans out first leaves the brackets and asterisks orphaned around
# gaps, and taking links out first eats backticks meant to stay.
#
# Both of those shipped. The first version did code-then-link and put a
# literal [...](...) on the page; the second did code-then-bold and printed
# "**Check  first.**" with the method name missing from between. Neither was
# noticed by reading the code - they were caught by the check at the end of
# render(), which is why it is there.
TOKEN = re.compile(
    r"(`[^`]+`)"                                # code
    r"|\[((?:[^\]`]|`[^`]+`)+)\]\(([^)]+)\)"     # [label](href)
    r"|\*\*((?:[^*`]|`[^`]+`)+)\*\*")             # **bold**


def spans(text):
    """Escape, rendering any code spans inside."""
    return "".join(
        "<code>" + escape(part[1:-1]) + "</code>" if i % 2 else escape(part)
        for i, part in enumerate(re.split(r"(`[^`]+`)", text)))


def inline(text):
    out = []
    at = 0
    for match in TOKEN.finditer(text):
        out.append(escape(text[at:match.start()]))
        code, label, href, bold = match.groups()
        if code is not None:
            out.append("<code>" + escape(code[1:-1]) + "</code>")
        elif label is not None:
            out.append('<a href="%s">%s</a>' % (escape(rewrite(href)), spans(label)))
        else:
            out.append("<strong>" + spans(bold) + "</strong>")
        at = match.end()
    out.append(escape(text[at:]))
    return "".join(out)


def slug(text):
    text = re.sub(r"<[^>]+>", "", text).lower()
    text = re.sub(r"[^a-z0-9]+", "-", text).strip("-")
    return text or "section"


def render(markdown):
    """Markdown subset to HTML, plus the headings for a contents list."""
    lines = markdown.replace("\r\n", "\n").split("\n")
    html = []
    contents = []
    seen = set()
    i = 0
    title = None

    def flush(buffer):
        if buffer:
            html.append("<p>" + inline(" ".join(buffer)) + "</p>")
        return []

    paragraph = []

    while i < len(lines):
        line = lines[i]

        if line.startswith("```"):
            paragraph = flush(paragraph)
            language = line[3:].strip()
            body = []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i])
                i += 1
            i += 1
            klass = ' class="lang-%s"' % language if language else ""
            html.append("<pre><code%s>%s</code></pre>"
                        % (klass, escape("\n".join(body))))
            continue

        if not line.strip():
            paragraph = flush(paragraph)
            i += 1
            continue

        if line.startswith("#"):
            paragraph = flush(paragraph)
            level = len(line) - len(line.lstrip("#"))
            text = line[level:].strip()
            if level == 1 and title is None:
                title = text
                i += 1
                continue
            anchor = slug(text)
            n = 2
            while anchor in seen:
                anchor = "%s-%d" % (slug(text), n)
                n += 1
            seen.add(anchor)
            if level in (2, 3):
                contents.append((level, anchor, text))
            html.append('<h%d id="%s">%s</h%d>'
                        % (level, anchor, inline(text), level))
            i += 1
            continue

        # A table: a row of cells, then a row of dashes.
        if (line.lstrip().startswith("|")
                and i + 1 < len(lines)
                and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1])):
            paragraph = flush(paragraph)

            def cells(row):
                return [c.strip() for c in row.strip().strip("|").split("|")]

            head = cells(line)
            i += 2
            rows = []
            while i < len(lines) and lines[i].lstrip().startswith("|"):
                rows.append(cells(lines[i]))
                i += 1
            html.append("<div class=\"scroll\"><table>")
            html.append("<thead><tr>"
                        + "".join("<th>" + inline(c) + "</th>" for c in head)
                        + "</tr></thead><tbody>")
            for row in rows:
                html.append("<tr>"
                            + "".join("<td>" + inline(c) + "</td>" for c in row)
                            + "</tr>")
            html.append("</tbody></table></div>")
            continue

        if re.match(r"^[-*] ", line):
            paragraph = flush(paragraph)
            items = []
            while i < len(lines) and re.match(r"^[-*] ", lines[i]):
                item = lines[i][2:].strip()
                i += 1
                # A wrapped item: indented continuation lines belong to it.
                while (i < len(lines) and lines[i].startswith("  ")
                       and lines[i].strip()
                       and not re.match(r"^[-*] ", lines[i].strip())):
                    item += " " + lines[i].strip()
                    i += 1
                items.append(item)
            html.append("<ul>"
                        + "".join("<li>" + inline(x) + "</li>" for x in items)
                        + "</ul>")
            continue

        if re.match(r"^-{3,}\s*$", line):
            paragraph = flush(paragraph)
            html.append("<hr>")
            i += 1
            continue

        if line.startswith(">"):
            paragraph = flush(paragraph)
            quote = []
            while i < len(lines) and lines[i].startswith(">"):
                quote.append(lines[i].lstrip(">").strip())
                i += 1
            html.append("<blockquote><p>" + inline(" ".join(quote)) + "</p></blockquote>")
            continue

        paragraph.append(line.strip())
        i += 1

    flush(paragraph)
    page = "\n".join(html)

    # Markup that survived into the output is markup that was not understood,
    # and it reaches the reader looking like a typo in the documentation
    # rather than a bug in this file. Checked outside code blocks only, where
    # a literal `](` or `**` is the text the author meant.
    outside = re.sub(r"<pre><code[^>]*>.*?</code></pre>", "", page, flags=re.S)
    outside = re.sub(r"<code>.*?</code>", "", outside, flags=re.S)
    for leftover in ("](", "**"):
        if leftover in outside:
            near = outside[max(0, outside.index(leftover) - 70):
                           outside.index(leftover) + 70]
            raise SystemExit("unrendered markup %r near: ...%s..."
                             % (leftover, near.replace("\n", " ")))

    return title, page, contents


SHELL = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} &mdash; Stipple</title>
<meta name="description" content="{blurb}">
<link rel="icon" href="../favicon.svg" type="image/svg+xml">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Archivo:wght@400;500;600;800&display=swap" rel="stylesheet">
<link rel="stylesheet" href="../stipple.css">
<link rel="stylesheet" href="../docs.css">
</head>
<body>

<a class="skip" href="#main">Skip to content</a>

<header class="bar">
  <a class="bar__brand" href="../">
    <span class="bar__mark" aria-hidden="true"></span>
    <span class="bar__name">Stipple</span>
  </a>
  <nav class="bar__nav" aria-label="Main">
{nav}
    <a href="https://github.com/{repo}">Source</a>
  </nav>
</header>

<div class="doc">

  <nav class="toc" aria-label="On this page">
    <p class="toc__head">On this page</p>
    <ol>
{contents}
    </ol>
  </nav>

  <main id="main" class="prose">
    <h1>{title}</h1>
    <p class="prose__lede">{blurb}</p>
{body}

    <p class="prose__source">
      This page is <a href="https://github.com/{repo}/blob/main/{source}">{source}</a>,
      rendered. Corrections go to the document.
    </p>
  </main>

</div>

</body>
</html>
"""


def build(page):
    source = os.path.join(ROOT, page["source"])
    with io.open(source, encoding="utf-8") as handle:
        markdown = handle.read()

    title, body, contents = render(markdown)

    toc = "\n".join(
        '      <li class="toc__%d"><a href="#%s">%s</a></li>' % (level, anchor, escape(text))
        for level, anchor, text in contents)

    nav = "\n".join(
        '    <a href="%s"%s>%s</a>'
        % (href, ' aria-current="page"' if key == page["nav"] else "", label)
        for label, href, key in NAV)

    html = SHELL.format(
        title=escape(title or page["title"]),
        blurb=escape(page["blurb"]),
        nav=nav,
        contents=toc,
        body=body,
        repo=REPO,
        source=page["source"])

    out = os.path.join(ROOT, page["out"])
    directory = os.path.dirname(out)
    if not os.path.isdir(directory):
        os.makedirs(directory)
    with io.open(out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(html)
    return len(contents)


def main():
    for page in PAGES:
        sections = build(page)
        print("%-24s -> %-28s %d sections"
              % (page["source"], page["out"], sections))
    return 0


if __name__ == "__main__":
    sys.exit(main())
