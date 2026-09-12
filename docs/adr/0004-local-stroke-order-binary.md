---
status: accepted
---

# Local SOB1 binary for the CoreS3 prototype

The first implementable 笔划 slice is a CoreS3 **local-first** prototype. Stroke
outlines and medians are converted offline from a verified, clean, pinned
`hanzi-writer-data` checkout into a versioned little-endian `stroke_order.bin`
(magic `SOB1`). The device parser is a bounded read-only index plus
single-character loader. Header, index, and individual record metadata have
CRC-32 integrity checks. Point access explicitly decodes little-endian bytes and
works with unaligned input. It does not parse SVG, allocate unbounded buffers,
or persist a full Flash 字库.

This does not replace the future networked `type=stroke` contract. Local STT
text is handled on-device after the current listen round; `hello` capability
`stroke_order` remains reserved for the later server path.

## Consequences

Host tests lock the converter, handwritten SOB1 golden, Python validator, and a
compiled independent C++ harness. Frontend/CoreS3 UI may later mmap or otherwise
bind a generated blob through `StrokeOrderStore` once
`CONFIG_STROKE_ORDER_LOCAL` is enabled; it must use decoded point accessors, not
typed pointers into serialized bytes. Shipping ~500 common characters still
requires an explicit charset, license files, and review; fixtures used here are
not a release library.
