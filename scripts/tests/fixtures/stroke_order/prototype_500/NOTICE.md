# 500-character 笔划 prototype notice

This directory is a **prototype**, not a release 字库, not official certification,
and not commercially reviewed.

## Character membership

Membership is the first 500 numbered entries (0001-0500) of the 一级字表 in
《通用规范汉字表》 (国发〔2013〕23号):

- page: https://www.gov.cn/zwgk/2013-08/19/content_2469793.htm
- PDF: https://www.gov.cn/gzdt/att/att/site1/20130819/tygfhzb.pdf
- PDF SHA-256: `af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9`
- PDF bytes: 100606660

A structured JSON checkout of `https://github.com/leonsilicon/table-of-general-standard-chinese-characters` at commit
`f9786a82be6e1672bdc60f85760a9e4a3791d1f1` was used only as a prototype transcription aid. That
checkout has no explicit license in-tree. It is **not** an official digital
annex and **not** a commercial authorization source. Dual-person page-by-page
official PDF verification has **not** been completed.

## Stroke graphics

Converted from `chanind/hanzi-writer-data` commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`
(upstream `skishore/makemeahanzi`) using only `strokes` and `medians`. The
complete Arphic Public License is in `ARPHICPL.TXT`.

Modification notice: on `2026-09-12`, converter `stroke-order-converter/1`
produced schema `SOB1 v1 + SPY1 v1` by flattening curves, rounding coordinates,
flipping y into SOB1, clamping MMAH control points that sit slightly outside
0..1024, and downsampling long outlines to stay inside the unchanged SOB1 point
and 1 MiB file limits. This reproducible date/version describes how and when the
converted files were produced; it is not a legal conclusion. Dictionary text,
`all.json`, and the full upstream checkout are not vendored.

## Pinyin

SPY1 readings come from Unicode 16.0.0 `Unihan_Readings.txt` (`kMandarin`
required, `kHanyuPinyin` union when it normalizes safely). The Unicode license
is `UNICODE-LICENSE.txt`. Device storage is group id and rank, not strings.

Do not describe this pack as officially certified or commercially releasable.
Legal review of Arphic Public License compliance is still required before any
external or commercial distribution; this notice does not provide legal advice
or an authorization conclusion.
