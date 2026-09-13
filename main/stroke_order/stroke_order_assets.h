#pragma once

#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_pinyin.h"

#include <cstddef>
#include <cstdint>

/**
 * Transactionally bind or suspend every runtime asset used by local 笔划.
 *
 * RebindStrokeOrderAssets leaves both consumers unbound unless SPY1, SCB1,
 * every declared SOB1 shard, and the catalog/pinyin codepoint+rank mapping all
 * validate. SuspendStrokeOrderAssets synchronously drops all owned metadata,
 * decoded glyphs, and the shard-source reference before Assets unmaps flash.
 */
bool RebindStrokeOrderAssets(StrokeOrderController* controller,
                             StrokeOrderPinyinIndex* pinyin_index, const uint8_t* catalog_data,
                             size_t catalog_size, StrokeOrderShardSource* shard_source,
                             const uint8_t* pinyin_data, size_t pinyin_size);
void SuspendStrokeOrderAssets(StrokeOrderController* controller,
                              StrokeOrderPinyinIndex* pinyin_index);
