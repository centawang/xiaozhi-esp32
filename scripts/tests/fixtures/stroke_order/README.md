# Stroke-order test fixtures

This directory has two deliberately separate fixture classes:

- `handwritten_sob1_v1.hex` is an independently hand-authored 84-byte SOB1
  golden. It contains U+4E00, boundary coordinates 0 and 1024, and fixed
  known-answer CRCs: header `0x7ce14721`, index `0x6e6cb232`, record
  `0x3bdfc874`. It was not produced by the Python SOB1 generator.
- `upstream_smoke/` contains only the real upstream JSON for 一、人、口, copied
  verbatim from the fixed commit and covered by its complete Arphic Public
  License plus `NOTICE.md`. It is a host-only smoke corpus, not a release 字库.

Most other test inputs are generated in temporary clean git repositories. Their
rectangles and simple curves are synthetic and are not upstream glyphs. The C++
harness consumes both a Python-generated binary and the independent golden, and
also loads a blob built from the real smoke corpus.

Do not copy `all.json` or an entire third-party checkout here. Any additional
real glyph must update the explicit character list, fixed commit, per-file
SHA-256 values, aggregate digest, license, and modification notice.

- `prototype_500/` is the reviewed 500-character CoreS3 prototype pack (SOB1 +
  SPY1 + manifests/source locks/coverage/licenses). It is not official
  certification and not a commercial release. Firmware builds copy it offline.
- `handwritten_spy1_v1.hex` is a 114-byte SPY1 golden with fixed header CRC
  `0x65605A07` and body CRC `0xC92B5D14`. It is independent of the 500 pack.
