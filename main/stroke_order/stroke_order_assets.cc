#include "stroke_order/stroke_order_assets.h"

void SuspendStrokeOrderAssets(StrokeOrderController* controller,
                              StrokeOrderPinyinIndex* pinyin_index) {
    if (controller != nullptr) {
        controller->Unbind();
    }
    if (pinyin_index != nullptr) {
        pinyin_index->Unbind();
    }
}

bool RebindStrokeOrderAssets(StrokeOrderController* controller,
                             StrokeOrderPinyinIndex* pinyin_index, const uint8_t* catalog_data,
                             size_t catalog_size, StrokeOrderShardSource* shard_source,
                             const uint8_t* pinyin_data, size_t pinyin_size) {
    if (controller == nullptr || pinyin_index == nullptr) {
        return false;
    }
    SuspendStrokeOrderAssets(controller, pinyin_index);
    if (catalog_data == nullptr || shard_source == nullptr || pinyin_data == nullptr ||
        !pinyin_index->Bind(pinyin_data, pinyin_size) ||
        !controller->BindCatalog(catalog_data, catalog_size, shard_source) ||
        !controller->MatchesPinyinIndex(*pinyin_index)) {
        SuspendStrokeOrderAssets(controller, pinyin_index);
        return false;
    }
    return true;
}
