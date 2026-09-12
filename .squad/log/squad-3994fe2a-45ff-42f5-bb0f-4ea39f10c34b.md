

<!-- pi-squad:a10fd668ec1e98ba02037b5b88a5b91d6332fde3edf4f89dd560672e6ce02bf2 -->
# squad-3994fe2a-45ff-42f5-bb0f-4ea39f10c34b
completed; 1/1 successful jobs.
- stroke-500-assets-pinyin-retry: Completed the timed-out stroke-500-assets-pinyin work in place (no rollback/commit). SPY1 C++ parser is bounded (LE/unaligned, CRC, overflow, ≤64KiB, rank/readings/groups/members); missing/corrupt SPY1 degrades to exact-only without unbinding SOB1. Controller/View use the provider (primary first, polyphone union, rank, dedup, store/loadable, max 6; out-of-store primary may still yield homophones). MQTT uses TopRanked(6) and never opens voice. Firmware extra-files now consume the pre-generated 500 pack (SOB1 1,046,788 / SPY1 18,152) mapped to stroke_order.bin/stroke_pinyin.bin plus selection/coverage/source locks/manifests/SHA256SUMS/APL/Unicode/NOTICE. ESP owned copies are PSRAM-only (MALLOC_CAP_SPIRAM|8BIT) with no DRAM fallback. Host suite 110/110. Isolated ESP-IDF v6.0.2 CoreS3 CONFIG_STROKE_ORDER_LOCAL=y build produced xiaozhi.bin 2,907,712 bytes (SHA-256 911a0615…) and generated_assets.bin 2,877,288 bytes; ~30% app partition free. Not flashed; type=stroke untouched.
