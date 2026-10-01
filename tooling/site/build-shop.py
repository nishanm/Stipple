#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the script shop page from scripts/.

    python3 tooling/site/build-shop.py

The shop is the directory. There is no database, no upload form and no
server: a script is a file in the repository, a submission is a pull request,
and this turns the one into a page. That is not a limitation worked around -
it is what lets every published script be compiled and run by the test suite
before it reaches anybody, which a form could never promise.

Metadata lives in comment lines at the top of each script, so a file stays a
single thing you can download and paste into the editor without stripping a
header off it first:

    # name: Big Clock
    # summary: One sentence, shown in the listing.
    # author: Who wrote it
    # tags: clock, time
    # panel: 52x16

Deliberately no template engine and no Markdown library. This runs in CI,
which must not install anything (ADR 0012's reasoning applied to tooling), and
the page is one shape repeated - a templating dependency would be bought for
nothing.
"""

import html
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
OUT = ROOT / "site" / "shop" / "index.html"


FIELD = re.compile(r"^#\s*([a-z]+):\s*(.+?)\s*$")


def parse(path):
    """Metadata and body for one script."""
    text = path.read_text(encoding="utf-8")
    meta = {"file": path.name}

    for line in text.splitlines():
        if not line.startswith("#"):
            # The header ends at the first line that is not a comment. A
            # `# tags:` written halfway down the file is a comment about the
            # code there, not metadata, and picking it up would be a guess.
            if line.strip() == "":
                continue
            break
        match = FIELD.match(line)
        if match:
            meta[match.group(1)] = match.group(2)

    meta["source"] = text
    meta["lines"] = len(text.splitlines())
    meta["bytes"] = len(text.encode("utf-8"))
    meta["tags"] = [t.strip().lower()
                    for t in meta.get("tags", "").split(",") if t.strip()]
    return meta


def required(meta, field):
    if field not in meta or not meta[field]:
        raise SystemExit(
            "build-shop: %s has no '# %s:' line" % (meta["file"], field))
    return meta[field]


def card(meta):
    name = required(meta, "name")
    summary = required(meta, "summary")
    author = meta.get("author", "unattributed")
    tags = meta["tags"]
    anchor = Path(meta["file"]).stem

    chips = "".join('<li>%s</li>' % html.escape(tag) for tag in tags)

    # Everything the filter matches on, on the element it filters.
    #
    # The alternative was a JSON blob of metadata beside the markup, which
    # means the page carries every field twice and the two can disagree. A
    # data attribute cannot drift from the card it is written on.
    haystack = " ".join([name, summary, author] + tags).lower()

    return """
      <article class="card" id="{anchor}"
               data-tags="{tagattr}" data-find="{find}">
        <img class="card__preview" src="{anchor}.gif" width="312" height="96"
             loading="lazy" alt="{name} running on a 52 by 16 pixel panel">
        <header class="card__head">
          <h3>{name}</h3>
          <p class="card__by">{author} · {lines} lines</p>
        </header>
        <p class="card__summary">{summary}</p>
        <ul class="card__tags">{chips}</ul>
        <button class="card__read" type="button" data-script="{anchor}">Read it</button>
        <template id="src-{anchor}"><pre><code>{source}</code></pre></template>
        <p class="card__get">
          <a href="https://github.com/galadril/Stipple/blob/main/scripts/{file}">View on GitHub</a>
          ·
          <a href="https://raw.githubusercontent.com/galadril/Stipple/main/scripts/{file}">Raw</a>
        </p>
      </article>
""".format(
        anchor=html.escape(anchor),
        name=html.escape(name),
        author=html.escape(author),
        lines=meta["lines"],
        summary=html.escape(summary),
        chips=chips,
        tagattr=html.escape(" ".join(tags)),
        find=html.escape(haystack),
        source=html.escape(meta["source"]),
        file=html.escape(meta["file"]),
    )


def filters(counts):
    """The tag chips, most-used first.

    Ordered by how many scripts carry each tag rather than alphabetically,
    because the useful filters are the ones with something behind them and an
    alphabetical list buries `game` under `astronomy`, `celebration` and
    `city`. Ties break alphabetically so the order is stable between builds -
    Counter.most_common is insertion-ordered for ties, and insertion order
    here is directory order, which would reshuffle the page every time
    somebody renamed a file.
    """
    ordered = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    return "".join(
        '<button class="chip" type="button" data-tag="{tag}" aria-pressed="false">'
        '{label}<span class="chip__n">{n}</span></button>'.format(
            tag=html.escape(tag), label=html.escape(tag), n=n)
        for tag, n in ordered)


PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Scripts — Stipple</title>
<meta name="description" content="Berry scripts for the Stipple firmware. Every one is compiled and run by the test suite before it is published.">
<link rel="icon" href="../favicon.svg" type="image/svg+xml">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Archivo:wght@400;500;600;800&display=swap" rel="stylesheet">
<link rel="stylesheet" href="../stipple.css">
<link rel="stylesheet" href="shop.css">
</head>
<body>

<a class="skip" href="#main">Skip to content</a>

<header class="bar">
  <a class="bar__brand" href="../">
    <span class="bar__mark" aria-hidden="true"></span>
    <span class="bar__name">Stipple</span>
  </a>
  <nav class="bar__nav" aria-label="Main">
    <a href="../#install">Install</a>
    <a href="../shop/" aria-current="page">Library</a>
    <a href="../scripting/">Scripting</a>
    <a href="../mqtt/">MQTT</a>
    <a href="../api/">API</a>
    <a href="https://github.com/galadril/Stipple">Source</a>
  </nav>
</header>

<main id="main" class="shop">

  <h1 class="sr-only">Script library</h1>

  <!-- The filter is progressive: without JavaScript every card is visible and
       the controls are hidden, which is the honest failure for a page whose
       whole job is listing things. `hidden` comes off in script. -->
  <section class="sift" id="sift" hidden aria-label="Filter scripts">
    <div class="sift__row">
      <label class="sift__search">
        <span class="sr-only">Search scripts</span>
        <input type="search" id="find" placeholder="Search name, summary or tag"
               autocomplete="off" spellcheck="false">
      </label>
      <p class="sift__count" id="count" role="status" aria-live="polite"></p>
      <button class="sift__clear" type="button" id="clear" hidden>Clear</button>
    </div>
    <div class="sift__tags" id="tags">__CHIPS__</div>
  </section>

  <section class="shop__grid" id="grid">
__CARDS__
  </section>

  <p class="shop__empty" id="empty" hidden>
    Nothing matches that. <button type="button" class="linkish" id="reset">Show everything</button>
  </p>

  <section class="shop__submit">
    <h2>Add one</h2>
    <p>
      The button opens GitHub's editor with the file named and a working
      script already in it. Change it to yours, commit, and GitHub offers you
      the pull request &mdash; nothing to clone, nothing to install.
    </p>
    <p class="shop__cta">
      <a class="shop__button" href="https://github.com/galadril/Stipple/new/main?filename=scripts/my-script.be&value=%23%20name%3A%20My%20Script%0A%23%20summary%3A%20One%20sentence%2C%20shown%20in%20the%20listing.%0A%23%20author%3A%20your-github-handle%0A%23%20tags%3A%20clock%2C%20animation%0A%23%20panel%3A%2052x16%0A%0Aclass%20App%0A%20%20def%20draw%28%29%0A%20%20%20%20clear%28rgb%280%2C%200%2C%200%29%29%0A%20%20%20%20text%282%2C%205%2C%20%22hello%22%2C%20rgb%280%2C%20190%2C%20255%29%29%0A%20%20end%0Aend%0A%0Areturn%20App%28%29%0A">Write a script</a>
    </p>
    <p>
      That is the whole submission. The test suite builds its list from this
      directory, so your script is compiled, run and checked for leaks by the
      same pull request that adds it &mdash; and this page rebuilds itself
      once that merges. The
      <a href="../scripting/">scripting reference</a> covers what it will
      hold you to.
    </p>
  </section>

</main>

<!-- One dialog, reused. Thirty scripts inline would be a page carrying
     several thousand lines of Berry whether or not anybody opens one; the
     sources sit in <template> elements, which browsers parse but do not
     render, and are moved in on demand. -->
<dialog id="reader" class="reader" aria-label="Script source">
  <div class="reader__bar">
    <h2 class="reader__title" id="reader-title"></h2>
    <div class="reader__actions">
      <button type="button" id="reader-copy" class="reader__btn">Copy</button>
      <button type="button" id="reader-close" class="reader__btn" aria-label="Close">Close</button>
    </div>
  </div>
  <div class="reader__body" id="reader-body"></div>
</dialog>

<script>
(function () {
    // --- the reader ------------------------------------------------------

    var dialog = document.getElementById('reader');
    // No <dialog> means no lightbox. The card falls back to the GitHub link
    // beside it, which is a worse experience and not a broken one.
    if (dialog && typeof dialog.showModal === 'function') {
        var title = document.getElementById('reader-title');
        var body = document.getElementById('reader-body');
        var copy = document.getElementById('reader-copy');

        var open = function (anchor, name) {
            var template = document.getElementById('src-' + anchor);
            if (!template) { return; }
            body.textContent = '';
            body.appendChild(template.content.cloneNode(true));
            title.textContent = name;
            copy.textContent = 'Copy';
            dialog.showModal();
        };

        Array.prototype.forEach.call(document.querySelectorAll('.card__read'), function (button) {
            button.addEventListener('click', function () {
                var card = button.closest('.card');
                var heading = card ? card.querySelector('h3') : null;
                open(button.getAttribute('data-script'), heading ? heading.textContent : 'Script');
            });
        });

        document.getElementById('reader-close').addEventListener('click', function () {
            dialog.close();
        });

        // Clicking the backdrop closes it. The dialog element reports those
        // clicks as landing on itself rather than on any child, which is the
        // only way to tell the two apart without wrapping the contents in
        // another box.
        dialog.addEventListener('click', function (event) {
            if (event.target === dialog) { dialog.close(); }
        });

        copy.addEventListener('click', function () {
            var code = body.querySelector('code');
            if (!code || !navigator.clipboard) { return; }
            navigator.clipboard.writeText(code.textContent).then(function () {
                copy.textContent = 'Copied';
            }, function () {
                // Clipboard access can be refused, and saying so beats a
                // button that silently did nothing.
                copy.textContent = 'Press Ctrl+C';
            });
        });
    }

    // --- the filter ------------------------------------------------------
    //
    // Text and tags together, both narrowing. Selecting `audio` and `tool`
    // means scripts that are both, not either: with thirty scripts the useful
    // question is "a tool that makes a noise", and an OR would answer it with
    // everything that does either.

    var sift = document.getElementById('sift');
    var grid = document.getElementById('grid');
    if (!sift || !grid) { return; }

    var cards = Array.prototype.slice.call(grid.querySelectorAll('.card'));
    var chips = Array.prototype.slice.call(document.querySelectorAll('.chip'));
    var find = document.getElementById('find');
    var count = document.getElementById('count');
    var clear = document.getElementById('clear');
    var empty = document.getElementById('empty');
    var reset = document.getElementById('reset');

    // The controls only exist for people who can use them.
    sift.hidden = false;

    var chosen = [];

    var matches = function (card) {
        var i;
        for (i = 0; i < chosen.length; i += 1) {
            if ((' ' + card.getAttribute('data-tags') + ' ').indexOf(' ' + chosen[i] + ' ') < 0) {
                return false;
            }
        }
        var text = find.value.trim().toLowerCase();
        if (text === '') { return true; }
        // Every word has to appear somewhere, so "clock audio" narrows
        // instead of widening. Splitting on whitespace rather than treating
        // the box as one phrase is what makes it feel like search.
        var words = text.split(/\\s+/);
        var haystack = card.getAttribute('data-find');
        for (i = 0; i < words.length; i += 1) {
            if (haystack.indexOf(words[i]) < 0) { return false; }
        }
        return true;
    };

    var apply = function () {
        var shown = 0;
        cards.forEach(function (card) {
            var ok = matches(card);
            card.hidden = !ok;
            if (ok) { shown += 1; }
        });

        var filtering = chosen.length > 0 || find.value.trim() !== '';
        count.textContent = filtering
            ? shown + ' of ' + cards.length
            : cards.length + ' scripts';
        clear.hidden = !filtering;
        empty.hidden = shown !== 0;

        // A tag that would leave nothing is worth showing as unavailable
        // rather than letting somebody click into an empty page.
        chips.forEach(function (chip) {
            var tag = chip.getAttribute('data-tag');
            if (chosen.indexOf(tag) >= 0) {
                chip.disabled = false;
                return;
            }
            var possible = cards.some(function (card) {
                return !card.hidden &&
                    (' ' + card.getAttribute('data-tags') + ' ').indexOf(' ' + tag + ' ') >= 0;
            });
            chip.disabled = !possible;
        });

        // The filter goes in the URL, so a link to "every audio script" is a
        // thing somebody can send. replaceState rather than pushState: each
        // keystroke would otherwise be a back-button step.
        var query = [];
        if (chosen.length) { query.push('tags=' + encodeURIComponent(chosen.join(','))); }
        if (find.value.trim()) { query.push('q=' + encodeURIComponent(find.value.trim())); }
        history.replaceState(null, '', query.length ? '?' + query.join('&') : location.pathname);
    };

    chips.forEach(function (chip) {
        chip.addEventListener('click', function () {
            var tag = chip.getAttribute('data-tag');
            var at = chosen.indexOf(tag);
            if (at >= 0) { chosen.splice(at, 1); } else { chosen.push(tag); }
            chip.setAttribute('aria-pressed', at >= 0 ? 'false' : 'true');
            apply();
        });
    });

    find.addEventListener('input', apply);

    var clearAll = function () {
        chosen = [];
        chips.forEach(function (chip) { chip.setAttribute('aria-pressed', 'false'); });
        find.value = '';
        apply();
        find.focus();
    };

    clear.addEventListener('click', clearAll);
    if (reset) { reset.addEventListener('click', clearAll); }

    // Escape clears, but only when the search box has the focus - taking the
    // key globally would break the dialog's own close.
    find.addEventListener('keydown', function (event) {
        if (event.key === 'Escape') { clearAll(); }
    });

    // Restore whatever the URL asked for, so a shared link opens filtered.
    var params = new URLSearchParams(location.search);
    var wanted = (params.get('tags') || '').split(',').filter(Boolean);
    wanted.forEach(function (tag) {
        var chip = chips.filter(function (c) { return c.getAttribute('data-tag') === tag; })[0];
        if (chip) {
            chosen.push(tag);
            chip.setAttribute('aria-pressed', 'true');
        }
    });
    if (params.get('q')) { find.value = params.get('q'); }

    apply();
}());
</script>

<footer class="foot">
  <p>
    Stipple is GPL-3.0-or-later. Scripts here are published under the same
    licence unless their header says otherwise.
  </p>
</footer>

</body>
</html>
"""


def main():
    if not SCRIPTS.is_dir():
        raise SystemExit("build-shop: no scripts/ directory")

    files = sorted(SCRIPTS.glob("*.be"))
    if not files:
        raise SystemExit("build-shop: scripts/ is empty")

    # Every published script is one the tests run, by construction: the test's
    # list is generated from this same directory at configure time. This used
    # to check that each name appeared in test_shop_scripts.cpp by hand, which
    # was a real guard right up until the list stopped being written by hand.

    entries = [parse(path) for path in files]
    cards = "".join(card(meta) for meta in entries)

    counts = Counter()
    for meta in entries:
        counts.update(meta["tags"])

    # Substituted rather than formatted. The page carries a script full of
    # JavaScript object and function braces, and str.format would need every
    # one of them doubled - a rule that holds right up until somebody adds a
    # line and forgets, at which point the page breaks somewhere unrelated to
    # the edit.
    page = (PAGE.replace("__CARDS__", cards)
                .replace("__CHIPS__", filters(counts)))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    # Newlines pinned to LF. Without it Python translates to CRLF on Windows,
    # so the file a Windows developer regenerates differs from the one Linux
    # CI regenerates - and the diff check that exists to catch real drift
    # would fail on every line instead.
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(page)
    print("build-shop: %d scripts, %d tags -> %s"
          % (len(files), len(counts), OUT.relative_to(ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
