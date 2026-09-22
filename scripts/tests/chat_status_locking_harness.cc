// Real production headers, DisplayLockGuard and extracted method bodies; real LVGL.
// Hardware construction, unrelated virtual methods, NVS and initial layout are stubs.
#include "dynamic_glyph_cache.h"
#include "lcd_display.h"
#include "lvgl_theme.h"
#include "oled_display.h"

#include <cassert>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

namespace Lang::Strings {
constexpr const char* LISTENING = "Listening";
constexpr const char* SPEAKING = "Speaking";
}  // namespace Lang::Strings

namespace {
std::timed_mutex ui_mutex;  // Deliberately NOT recursive.
thread_local bool owns_ui = false;
std::mutex gate_mutex;
std::condition_variable gate_cv;
bool pause_writer = false, writer_paused = false, contender_attempted = false;
const char* paused_status = nullptr;
int acquisitions = 0;
std::function<void()> observe_commit;

bool LockUi() {
    if (owns_ui) {
        return false;  // Production DisplayLockGuard must abort, not hide recursion.
    }
    {
        std::lock_guard<std::mutex> gate(gate_mutex);
        if (writer_paused) {
            contender_attempted = true;
            gate_cv.notify_all();
        }
    }
    if (!ui_mutex.try_lock_for(std::chrono::seconds(3))) {
        return false;
    }
    owns_ui = true;
    ++acquisitions;
    return true;
}
void UnlockUi() {
    assert(owns_ui);
    if (observe_commit) {
        observe_commit();  // Observe every committed transaction, while still locked.
    }
    owns_ui = false;
    ui_mutex.unlock();
}
void TracedLabelSetText(lv_obj_t* label, const char* text) {
    assert(owns_ui);
    lv_label_set_text(label, text);
    std::unique_lock<std::mutex> gate(gate_mutex);
    if (pause_writer && std::strcmp(text, paused_status) == 0) {
        pause_writer = false;
        writer_paused = true;
        gate_cv.notify_all();
        assert(gate_cv.wait_for(gate, std::chrono::seconds(3), [] { return contender_attempted; }));
        writer_paused = false;
    }
}
uint32_t ColorFor(const char* status) {
    return std::strcmp(status, Lang::Strings::LISTENING) == 0  ? 0x00FF00
           : std::strcmp(status, Lang::Strings::SPEAKING) == 0 ? 0xFF0000
                                                               : 0xFFFFFF;
}
}  // namespace

// No NVS access in host tests. The actual Display::SetTheme body is included below.
class Settings {
public:
    Settings(const char*, bool) {}
    void SetString(const char*, const std::string&) { assert(owns_ui); }
};

Display::Display() = default;
Display::~Display() = default;
void Display::SetStatus(const char*) {}
void Display::ShowNotification(const char*, int) {}
void Display::ShowNotification(const std::string&, int) {}
void Display::SetEmotion(const char*) {}
void Display::SetChatMessage(const char*, const char*) {}
void Display::ClearChatMessages() {}
void Display::UpdateStatusBar(bool) {}
void Display::SetPowerSaveMode(bool) {}

LvglDisplay::LvglDisplay() { display_ = lv_display_create(320, 240); }
LvglDisplay::~LvglDisplay() { lv_display_delete(display_); }
void LvglDisplay::ShowNotification(const char*, int) {}
void LvglDisplay::ShowNotification(const std::string&, int) {}
void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage>) {}
void LvglDisplay::UpdateStatusBar(bool) {}
void LvglDisplay::SetPowerSaveMode(bool) {}
bool LvglDisplay::SnapshotToJpeg(std::string&, int) { return false; }
bool LvglDisplay::AddTextGlyphs(const std::vector<TextGlyph>&, uint8_t) { return false; }
void LvglDisplay::ClearTextGlyphs() {}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t, esp_lcd_panel_handle_t, int, int) {}
LcdDisplay::~LcdDisplay() = default;
LvglGif::~LvglGif() = default;
const lv_img_dsc_t* LvglGif::image_dsc() const { return nullptr; }
SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel, int w,
                             int h, int, int, bool, bool, bool)
    : LcdDisplay(io, panel, w, h) {}
bool LcdDisplay::Lock(int) { return LockUi(); }
void LcdDisplay::Unlock() { UnlockUi(); }
void LcdDisplay::SetEmotion(const char*) {}
void LcdDisplay::ClearChatMessages() {}
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage>) {}
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    if (setup_ui_called_) {
        return;
    }
    Display::SetupUI();
    auto* screen = lv_display_get_screen_active(display_);
    container_ = lv_obj_create(screen);
    top_bar_ = lv_obj_create(screen);
    status_bar_ = lv_obj_create(top_bar_);
    content_ = lv_obj_create(container_);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    bottom_bar_ = lv_obj_create(screen);
    network_label_ = lv_label_create(top_bar_);
    status_label_ = lv_label_create(status_bar_);
    notification_label_ = lv_label_create(status_bar_);
    mute_label_ = lv_label_create(top_bar_);
    battery_label_ = lv_label_create(top_bar_);
    emoji_label_ = lv_label_create(screen);
    low_battery_popup_ = lv_obj_create(screen);
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    chat_message_label_ = lv_label_create(bottom_bar_);
#endif
}
static int chat_calls = 0;
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    ++chat_calls;
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    auto* bubble = lv_obj_create(content_);
    lv_obj_set_user_data(bubble, const_cast<char*>(role));
    chat_message_label_ = lv_label_create(bubble);
#endif
    lv_label_set_text(chat_message_label_, content);
}

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t, esp_lcd_panel_handle_t, int, int, bool, bool) {}
OledDisplay::~OledDisplay() = default;
bool OledDisplay::Lock(int) { return LockUi(); }
void OledDisplay::Unlock() { UnlockUi(); }
void OledDisplay::SetupUI() {}
void OledDisplay::SetChatMessage(const char*, const char*) {}
void OledDisplay::SetEmotion(const char*) {}

// No changes to the extracted production method bodies. Interpose only the LVGL
// label write to force contention in the middle of the REAL SetStatus transaction.
#define lv_label_set_text TracedLabelSetText
#include PRODUCTION_INCLUDE
#undef lv_label_set_text

template <class Base>
class Probe : public Base {
public:
    using Base::Base;
    void SelectInitialTheme(Theme* theme) { this->current_theme_ = theme; }
    lv_obj_t* Screen() { return lv_display_get_screen_active(this->display_); }
};
class PlainLcdProbe : public Probe<SpiLcdDisplay> {
public:
    using Probe::Probe;
    void CheckThemeColors(const LvglTheme& theme) {
        assert(lv_color_eq(lv_obj_get_style_bg_color(container_, LV_PART_MAIN),
                           theme.background_color()));
        assert(lv_color_eq(lv_obj_get_style_text_color(status_label_, LV_PART_MAIN),
                           theme.text_color()));
    }
};

class ChatProbe : public Probe<CoreS3ChatDisplay> {
public:
    ChatProbe() : Probe(nullptr, nullptr, 320, 240, 0, 0, false, false, false) {}
    void CheckColors(uint32_t expected) {
        for (auto* obj : {Screen(), container_, top_bar_, content_, bottom_bar_}) {
            assert(
                lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), lv_color_hex(expected)));
            assert(lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == LV_OPA_COVER);
            assert(lv_obj_get_style_bg_image_src(obj, LV_PART_MAIN) == nullptr);
        }
        assert(lv_obj_get_style_bg_opa(status_bar_, LV_PART_MAIN) == LV_OPA_TRANSP);
        for (auto* label : {network_label_, status_label_, notification_label_, mute_label_,
                            battery_label_, emoji_label_}) {
            assert(lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_black()));
        }
    }
    void CheckCommit(std::vector<std::string>& committed) {
        assert(owns_ui);
        const char* status = lv_label_get_text(status_label_);
        if (!lv_color_eq(lv_obj_get_style_bg_color(container_, LV_PART_MAIN),
                         lv_color_hex(ColorFor(status)))) {
            std::cerr << "inconsistent status commit\n";
            std::abort();
        }
        CheckColors(ColorFor(status));
        assert(!lv_obj_has_flag(status_label_, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN));
        assert(last_status_update_time_.time_since_epoch().count() > 0);
        committed.emplace_back(status);
    }
    const char* ChatText() { return lv_label_get_text(chat_message_label_); }
    void ShowNotificationForTest() {
        lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
};

static LvglTheme light("light"), dark("dark");
static void InitializeThemes() {
    for (auto* theme : {&light, &dark}) {
        auto font = std::make_shared<LvglBuiltInFont>(LV_FONT_DEFAULT);
        theme->set_text_font(font);
        theme->set_icon_font(std::make_shared<LvglBuiltInFont>(LV_FONT_DEFAULT));
        theme->set_large_icon_font(std::make_shared<LvglBuiltInFont>(LV_FONT_DEFAULT));
        theme->set_background_color(lv_color_hex(0x123456));
        theme->set_text_color(lv_color_white());
        theme->set_border_color(lv_color_black());
        theme->set_system_bubble_color(lv_color_hex(0x234567));
        theme->set_system_text_color(lv_color_white());
        theme->set_low_battery_color(lv_color_black());
        LvglThemeManager::GetInstance().RegisterTheme(theme->name(), theme);
    }
}

template <class T>
static void CheckFont(T& display) {
    display.SelectInitialTheme(&light);
    display.SetupUI();
    std::weak_ptr<LvglFont> previous = light.text_font();
    auto font = std::make_shared<LvglBuiltInFont>(LV_FONT_DEFAULT);
    int before = acquisitions;
    // The actual SetTextFont holds the outer nonrecursive guard and dispatches
    // through the actual CoreS3/LCD or OLED hook, including every LVGL style write.
    assert(display.SetTextFont(font));
    assert(acquisitions == before + 1);
    assert(!owns_ui);
    assert(previous.expired());
    assert(lv_obj_get_style_text_font(display.Screen(), LV_PART_MAIN) == font->font());
    assert(light.text_font() == font && dark.text_font() == font);
    before = acquisitions;
    Display* api = &display;
    api->SetTheme(&dark);
    assert(acquisitions == before + 1);
    assert(!display.SetTextFont(nullptr));
}
static void TestFonts() {
    {
        ChatProbe display;
        CheckFont(display);
        display.SetStatus(Lang::Strings::LISTENING);
        auto font = std::make_shared<LvglBuiltInFont>(LV_FONT_DEFAULT);
        assert(display.SetTextFont(font));
        display.CheckColors(0x00FF00);
    }
    InitializeThemes();
    {
        PlainLcdProbe display(nullptr, nullptr, 320, 240, 0, 0, false, false, false);
        CheckFont(display);
        display.CheckThemeColors(dark);
    }
    InitializeThemes();
    {
        Probe<OledDisplay> display(nullptr, nullptr, 128, 64, false, false);
        CheckFont(display);
    }
}

static void TestStatus() {
    ChatProbe display;
    display.SetupUI();
    // Both orders, repeatedly, without sleeps or relying on scheduler luck.
    for (int iteration = 0; iteration < 20; ++iteration) {
        const char* first = iteration % 2 ? Lang::Strings::SPEAKING : Lang::Strings::LISTENING;
        const char* second = iteration % 2 ? Lang::Strings::LISTENING : Lang::Strings::SPEAKING;
        display.ShowNotificationForTest();
        std::vector<std::string> committed;
        observe_commit = [&] { display.CheckCommit(committed); };
        {
            std::lock_guard<std::mutex> gate(gate_mutex);
            paused_status = first;
            pause_writer = true;
            writer_paused = contender_attempted = false;
        }
        int before = acquisitions;
        std::thread a([&] { display.SetStatus(first); });
        {
            std::unique_lock<std::mutex> gate(gate_mutex);
            assert(gate_cv.wait_for(gate, std::chrono::seconds(3), [] { return writer_paused; }));
        }
        // B must attempt the very same mutex while A has written the text but
        // has not yet hidden notification / timestamped / applied the colors.
        std::thread b([&] { display.SetStatus(second); });
        a.join();
        b.join();
        observe_commit = nullptr;
        assert(contender_attempted);
        assert(acquisitions == before + 2);
        assert((committed == std::vector<std::string>{first, second}));
        display.CheckColors(ColorFor(second));
    }
}

static void TestPolicy() {
    ChatProbe display;
    display.SetStatus(nullptr);
    display.SetupUI();
    display.CheckColors(0xFFFFFF);
    display.SetChatMessage("system", "activation/warning");
    const int before = chat_calls;
    for (int i = 0; i < 30; ++i) {
        display.SetChatMessage("user", "hidden");
        display.SetChatMessage("assistant", "hidden");
        display.SetChatMessage(nullptr, nullptr);
    }
    assert(chat_calls == before);
    assert(std::strcmp(display.ChatText(), "activation/warning") == 0);
    auto* overlay = lv_obj_create(display.Screen());
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x654321), 0);
    lv_obj_set_style_text_color(overlay, lv_color_hex(0x123456), 0);
    auto* label = lv_label_create(overlay);
    for (const char* status :
         {"Listening", "Speaking", "Standby", "Connecting", "Error", "12:34"}) {
        display.SetStatus(status);
        display.CheckColors(ColorFor(status));
        for (auto* theme : {&light, &dark}) {
            display.SetTheme(theme);
            display.CheckColors(ColorFor(status));
            assert(lv_color_eq(lv_obj_get_style_bg_color(overlay, LV_PART_MAIN),
                               lv_color_hex(0x654321)));
            assert(lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN),
                               lv_color_hex(0x123456)));
        }
    }
    display.SetChatMessage("system", nullptr);
    assert(std::strcmp(display.ChatText(), "") == 0);
    display.SetStatus(nullptr);
    display.CheckColors(0xFFFFFF);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    lv_init();
    InitializeThemes();
    if (std::strcmp(argv[1], "font") == 0) {
        TestFonts();
    } else if (std::strcmp(argv[1], "status") == 0) {
        TestStatus();
    } else {
        TestPolicy();
    }
    lv_deinit();
    std::cout << "PASS style=" << CONFIG_USE_WECHAT_MESSAGE_STYLE << " case=" << argv[1] << "\n";
}
