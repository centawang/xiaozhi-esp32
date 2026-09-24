#ifndef ASSETS_H
#define ASSETS_H

#include <functional>
#include <memory>
#include <string>

#include <esp_partition.h>
#include <cJSON.h>
#include <model_path.h>
#include <map>
#include <string>

#if HAVE_LVGL
#include <spi_flash_mmap.h>
#endif

#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
#include <mutex>
class StrokeOrderBundleOwner;
#endif

struct Asset {
    size_t size;
    size_t offset;
};

struct TextFontCapability {
    bool glyph_push = false;
    std::string bundle;
    std::string charset;
    int size = 0;
    int bpp = 0;
};

class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets();

    bool Download(std::string url,
                  std::function<void(int progress, size_t speed)> progress_callback);
    bool Apply(bool refresh_display_theme = true);
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size);
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    // Pins exactly catalog/pinyin/six shards until the final immutable owner dies.
    std::shared_ptr<const StrokeOrderBundleOwner> LeaseStrokeBundle(uint64_t* generation);
    uint64_t StrokeAssetsGeneration() const;
#endif

    inline bool partition_valid() const { return partition_valid_; }
    inline std::string default_assets_url() const { return default_assets_url_; }
    inline TextFontCapability text_font_capability() const { return text_font_capability_; }

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    bool InitializePartition();
    bool UnApplyPartition();
    static bool FindPartition(Assets* assets);
    static bool LoadSrmodelsFromIndex(Assets* assets, cJSON* root = nullptr);
    void UseBuiltInTextFontCapability();
    void DisableTextFontGlyphPush();

    class AssetStrategy {
    public:
        virtual ~AssetStrategy() = default;
        virtual bool Apply(Assets* assets, bool refresh_display_theme = true) = 0;
        virtual bool InitializePartition(Assets* assets) = 0;
        virtual void UnApplyPartition(Assets* assets) = 0;
        virtual bool GetAssetData(Assets* assets, const std::string& name, void*& ptr,
                                  size_t& size) = 0;
    };

    class LvglStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets, bool refresh_display_theme = true) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr,
                          size_t& size) override;

    private:
        static uint32_t CalculateChecksum(const char* data, uint32_t length);
        std::map<std::string, Asset> assets_;
        esp_partition_mmap_handle_t mmap_handle_ = 0;
        const char* mmap_root_ = nullptr;
        bool checksum_valid_ = false;
    };

    class EmoteStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets, bool refresh_display_theme = true) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr,
                          size_t& size) override;
    };

#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    mutable std::mutex stroke_mapping_mutex_;
    std::shared_ptr<const uint64_t> stroke_mapping_pin_;
    uint64_t stroke_mapping_generation_ = 0;
    bool stroke_mapping_accepting_ = false;
#endif

    // Strategy instance
    std::unique_ptr<AssetStrategy> strategy_;

protected:
    const esp_partition_t* partition_ = nullptr;
    bool partition_valid_ = false;
    std::string default_assets_url_;
    TextFontCapability text_font_capability_;
    srmodel_list_t* models_list_ = nullptr;
};

#endif
