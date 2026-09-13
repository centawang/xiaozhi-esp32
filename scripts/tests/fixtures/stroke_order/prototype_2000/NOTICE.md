# 2000-character sharded 笔划 technical prototype

This is a prototype, not a release 字库, not official per-character certification,
and not commercially reviewed. Membership is 一级字表 0001-2000 from
《通用规范汉字表》 (国发〔2013〕23号). The official PDF SHA-256 is
`af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9` (100606660 bytes).

The pinned transcription aid is `https://github.com/leonsilicon/table-of-general-standard-chinese-characters` commit
`f9786a82be6e1672bdc60f85760a9e4a3791d1f1`. It has no explicit in-tree license and is not an
official digital annex or commercial authorization source. Dual-person
page-by-page PDF verification has not been completed.

Stroke graphics use only strokes/medians from chanind/hanzi-writer-data commit
`68d10a4b21150cae5e1ebbd223eed289cf32d90c` (upstream skishore/makemeahanzi), converted on
2026-09-12. The complete Arphic Public License is ARPHICPL.TXT.
The corpus is split by official rank into eight deterministic 250-character
SOB1 v1 shards; the device never copies the complete 5800492-byte
shard corpus into PSRAM. SCB1 v1 maps codepoint/rank to a shard and local index.

Pinyin uses Unicode 16.0.0 Unihan kMandarin plus bounded kHanyuPinyin union;
UNICODE-LICENSE.txt applies. Measured SPY1 v1 is 63618 bytes with
1047 groups and maximum group size
29; no homophone group is
truncated.

Commercial release still requires APL/transcription/Unicode legal review and
manual stroke-order accuracy review. No official per-character certification is
claimed.
