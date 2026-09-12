# hanzi-writer-data smoke corpus notice

This directory contains exactly three graphics JSON files copied **verbatim**
from `chanind/hanzi-writer-data` at commit
`68d10a4b21150cae5e1ebbd223eed289cf32d90c`:

- repository: <https://github.com/chanind/hanzi-writer-data>
- upstream graphics project: `skishore/makemeahanzi`
- license: Arphic Public License; the complete, unmodified upstream
  `ARPHICPL.TXT` is included beside this notice
- purpose: a tiny host-only parser/converter smoke corpus, not a release 字库

Original files and SHA-256 digests:

| Character | Original path | SHA-256 |
|---|---|---|
| 一 | `data/一.json` | `ed728673d86fb9aa559c5b150e59ffbee666b085d4a55597c524079a91ebc8ce` |
| 人 | `data/人.json` | `18ffb9fb727576b0c3e7dc44914be7ffd43164de9ba121710824b83f27bd3bb1` |
| 口 | `data/口.json` | `ac208865e3166fc35214cec326f00a4b6788a8cac9b4c5a6a37745077e32ddab` |

The aggregate selected-file digest is
`c0f0eb725e23c91029ed59a5955c077fc4a1643606d21f9a935505b2088d78d6`.
It is SHA-256 over UTF-8 lines sorted by codepoint, each encoded as
`U+XXXX NUL relative/path NUL file-sha256 LF`.

No glyph JSON in this directory was modified. During a smoke conversion the
host tool flattens curves, rounds coordinates, flips the y axis, and serializes
only outlines and medians into SOB1. That generated binary is a modified form;
it is not committed here and remains governed by the included license. No
`all.json`, dictionary text, or full upstream checkout is vendored.

Reproduce against a verified clean checkout without downloading from the
converter itself:

```sh
git clone https://github.com/chanind/hanzi-writer-data.git /tmp/hanzi-writer-data
git -C /tmp/hanzi-writer-data checkout --detach 68d10a4b21150cae5e1ebbd223eed289cf32d90c
printf '一\n人\n口\n' >/tmp/stroke-smoke-charset.txt
python3 scripts/convert_stroke_order.py \
  --hanzi-writer-data /tmp/hanzi-writer-data \
  --charset /tmp/stroke-smoke-charset.txt \
  --source-commit 68d10a4b21150cae5e1ebbd223eed289cf32d90c \
  --output-dir /tmp/stroke-smoke-out
```

The converter verifies the checkout HEAD, origin URL, clean worktree, each
selected source-file digest, and the aggregate digest before writing output.
