---
status: accepted
---

# Use bounded vector data for stroke-order animation

Each selected character is represented by an ordered list of strokes in a restricted, versioned vector format. A stroke contains a closed outline for its visible shape and an ordered median path for progressive reveal; coordinates are bounded integers normalized to a 0–1024 design space and scaled to the active grid by the device.

Arbitrary SVG, scripts, external resources, frame images, and inferred font outlines are not accepted. This preserves theme coloring, replay, pause, and step controls while keeping parsing deterministic and independent of display resolution. A shared `StrokeOrderView` owns layout and animation; boards expose coordinate input but do not implement feature UI.

## Amendment — host-flattened SOB1

The device consumes only the compact integer format produced by
`scripts/convert_stroke_order.py` (see `docs/stroke-order-data.md`). Curve
flattening and y-axis conversion happen on the host. Synthetic test fixtures and
prototype bins are not a published 字库. The original ~500-character target was
superseded by the eight-shard 2000-character strategy in ADR 0005; both retain
explicit charsets, pinned commits, license files, and prototype-only boundaries.
