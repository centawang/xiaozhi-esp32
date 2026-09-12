# Frontend: project knowledge

做一个新的功能，叫“笔划”。用户说一个汉字的单字，找几个备选，用户点击屏幕选择。用动画演示正确的笔画顺序。

<!-- pi-squad:5e87d9e118c395852f53a9104376cd68fe07d4d581527492a55d53d0fcf18bae -->
## 2026-09-12T04:21:17.656Z — repo-ui

# repo-understanding-ui

Read-only survey of display, touch, and user-interaction capability in `/Users/mandyw/git/xiaozhi-esp32`. No files were modified; no tests were run.

## 1. UI tech stack

- **Widget engine:** LVGL 9.5 (`lvgl/lvgl ~9.5.0`) plus `esp_lvgl_port ~2.8.0`. LVGL is compiled out when `CONFIG_USE_EMOTE_MESSAGE_STYLE` is set (`HAVE_LVGL` is defined only if that Kconfig is off).
- **Panel HAL:** ESP-IDF `esp_lcd` (`esp_lcd_panel_io` / `esp_lcd_panel_ops`) with vendor controllers from `main/idf_component.yml` (ST7789/ILI9341/GC9A01/ST77916/ST7701/AXS15231B/SH8601/CO5300/JD9365/ILI9881C, etc.).
- **Display class tree:** `Display` → `NoDisplay` | `emote::EmoteDisplay` | `LvglDisplay` → `OledDisplay` | `LcdDisplay` → `SpiLcdDisplay` | `RgbLcdDisplay` | `MipiLcdDisplay`. Boards may subclass `SpiLcdDisplay`/`LcdDisplay` for padding, round-screen bars, e-paper flush, or custom flush callbacks.
- **Buses / color:** SPI RGB565 (typical 240x320 / 320x240), RGB LCD (double buffer, tearing avoidance), MIPI DSI. OLED is monochrome 128x64 or 128x32.
- **Fonts:** CMake `BUILTIN_TEXT_FONT` / `BUILTIN_ICON_FONT` (`font_noto_sans_basic_*` + `font_material_symbols_*`, size/bpp per board). Runtime CJK comes from assets `LvglCBinFont` plus `DynamicGlyphCache` (max 256 glyphs / 64 KB) fed by protocol `TextGlyphPayload`. Built-in Noto Sans Basic is not a full Han font.
- **Assets:** `assets` partition → `Assets::LvglStrategy` (fonts, emoji PNG/GIF, skins) or `Assets::EmoteStrategy` (`esp_emote_expression` mmap). Flash choice is `FLASH_DEFAULT_ASSETS` / `FLASH_CUSTOM_ASSETS` / `FLASH_EXPRESSION_ASSETS`.
- **Themes:** `LvglTheme` + `LvglThemeManager` (`light`/`dark`), persisted in NVS `display.theme`.
- **Animation today:** LVGL label scroll (`lv_anim` on chat text); GIF emotion via `LvglGif` + `gifdec` + `lv_timer`; camera/preview images; Emote path is a separate 30 fps gfx engine, not LVGL widgets.
- **MCP screen tools (LVGL only):** `self.screen.set_theme`, `set_brightness`, `get_info`, optional `snapshot` / `preview_image`.

Kconfig UI knobs live in `main/Kconfig.projbuild`: `DISPLAY_STYLE` (`USE_DEFAULT_MESSAGE_STYLE` | `USE_WECHAT_MESSAGE_STYLE` | `USE_EMOTE_MESSAGE_STYLE`), `USE_MULTILINE_CHAT_MESSAGE` (default style only), language, board type, per-board LCD variants.

CMake chain: `CONFIG_BOARD_TYPE_*` → `BOARD_DIR` + builtin fonts/emoji → `file(GLOB boards/${BOARD_DIR}/*.{cc,c})` → `DECLARE_BOARD` factory. Display sources in `main/` are always compiled; board `.cc` chooses `SpiLcdDisplay` / `OledDisplay` / `EmoteDisplay` / `NoDisplay`.

## 2. Draw / animation data flow

```
Board ctor
  → esp_lcd panel_io + panel (pins from board config.h)
  → new Spi/Rgb/MipiLcdDisplay | OledDisplay | EmoteDisplay
       Lcd/Oled: lv_init → lvgl_port_init → lvgl_port_add_disp[_rgb|_dsi]
Application::Initialize()
  → Display::SetupUI()          // must run after LVGL display exists
  → Assets::Apply()             // optional custom font / emoji / skin
Protocol JSON (stt/tts/llm)
  → Application::Schedule()
  → DisplayLockGuard (lvgl_port_lock)
  → AddTextGlyphs / SetChatMessage / SetEmotion / SetStatus
LVGL port task (priority 1, often CPU1) flushes RGB565 to panel
```

UI mutations must go through `Application::Schedule()` (or event bits) and `DisplayLockGuard`. Callbacks may run off the main task. `SetupUI()` is idempotent; calling display APIs before it logs and drops the update.

Default LCD layout: full-screen container, centered emoji (font or GIF), optional preview image, top status icons, overlapping status/notification labels, bottom chat bar (single-line circular scroll or multiline wrap). WeChat style is a flex column of chat bubbles. OLED is a two-pane 128x64 or single-row 128x32 layout.

There is **no** `lv_canvas` / `lv_line` / `lv_btn` usage in core UI. The only `lv_obj_add_event_cb` is `LV_EVENT_DELETE` for WeChat preview-image cleanup. Stroke-order path drawing would be new LVGL code.

## 3. Touch / input data flow

Three independent input paths; they are **not** unified behind `Display` or `Board`:

1. **GPIO / ADC buttons** (`boards/common/button.{h,cc}`, espressif `button ~4.2.0`). Typical mapping: boot click → `Application::ToggleChatState()`, press-down/up → listen, volume, Wi-Fi config. This is the default UX on most boards, including color LCDs without a touch IC.
2. **LVGL indev touch (coordinate):** board `InitializeTouch()` creates `esp_lcd_touch_*` (FT5x06, GT911, GT1151, CST816S, CST9217, AXS15231B, ST7123, …) then `lvgl_port_add_touch({.disp, .handle})`. Touch then belongs to LVGL. **Core UI never registers click handlers**, so these events currently do nothing visible except keep the indev alive.
3. **Board-local touch-as-button:** e.g. Freenove polls FT5x06 in a FreeRTOS task (single/double/long tap → ToggleChatState / StartListening / Wi-Fi); Spotpear CST816D timer maps any contact duration to the same chat toggle. Coordinates are read and discarded. These boards cannot drive LVGL widgets unless rewired to `lvgl_port_add_touch`.

Other inputs: `Knob` (rotary volume), capacitive `touch_button_sensor` on some S3 boards, `PowerSaveTimer` dimming/sleepy emoji. `Board::GetDisplay()` defaults to a static `NoDisplay`.

## 4. Reusable components (do not reinvent)

| Layer | Reuse |
|---|---|
| `Display` / `LvglDisplay` / `LcdDisplay` | Status, chat, emotion, theme, glyph cache, lock |
| `DisplayLockGuard` | Mandatory around LVGL |
| `LvglTheme` / fonts / `DynamicGlyphCache` | CJK candidate labels |
| `LvglGif` + `lv_timer` | Frame-sequence stroke playback if assets are GIFs |
| `lv_anim` | Stroke reveal / candidate press feedback |
| `Button` / `Backlight` / `Knob` | Non-touch fallback |
| `Application::Schedule` + protocol JSON | Feature state, not widgets |
| `TextGlyphPayload` pattern | Server-pushed CJK bitmaps |
| Board `InitializeTouch` + `lvgl_port_add_touch` | Coordinate hit-testing |

Board subclasses of `LcdDisplay` only tweak layout (round AMOLED padding, chat-bar width). Feature UI should not be copied into each board.

## 5. Board capability differences (for “笔划”)

Capability classes, not an exhaustive board list:

- **No screen:** `Board::GetDisplay()` → `NoDisplay`. No candidates, no animation.
- **OLED 128x32 / 128x64:** `OledDisplay`, monochrome, tiny. Can show 1–2 Han glyphs if font/glyphs exist; not suitable for multi-candidate tap + stroke demo.
- **Color LCD, buttons only:** `SpiLcdDisplay` etc. Can *draw* candidates and stroke animation, but selection needs GPIO/knob or voice, not tap.
- **Color LCD + LVGL touch indev:** Waveshare/Espressif/Lichuang/RYMCU and many `*-touch-*` boards. **Best target** for tap-to-select. Resolutions typically 240x240 → 320x240 → 480-class → 7" RGB. Lichuang S3 is a clean 320x240 ST7789 + FT5x06 + `lvgl_port_add_touch` reference.
- **Color LCD + touch-as-button only:** Freenove 2.8", Spotpear 1.54 CST816. Touch exists but is not an LVGL indev.
- **Emote style:** `EmoteDisplay` (`esp_emote_expression`). Replaces LVGL widgets; no `HAVE_LVGL` MCP snapshot/theme path. Unsuitable for interactive candidate grids unless the feature is LVGL-only and Emote is excluded.
- **E-paper / RGB matrix / robot emoji subclasses:** Slow or non-standard flush; treat as non-goals for v1.
- **PSRAM / font bpp:** Larger boards get 20px/30px 4bpp fonts and 64/128 emoji; compact boards stay 14px 1bpp. Stroke canvas + Han labels want PSRAM and ≥∼240px color LCD.

Touch controller and swap/mirror flags are **board-private** (`config.h` + `InitializeTouch`). Core code must not include a concrete `esp_lcd_touch_*` header.

## 6. Module boundaries for 汉字候选 + 笔顺动画

Recommended split (proposal, not policy):

1. **Protocol / Application (feature owner)**  
   Speech “user said one Han syllable” already arrives as `stt` → `SetChatMessage("user", …)` + optional glyphs. New JSON type (or MCP tool) should carry: candidate list `{char, maybe extra glyphs}` and stroke payload `{strokes: [{points or svg-like commands}]}`. Parse next to `TextGlyphPayload`. Drive the session with `Application::Schedule()`. Do not block the audio/main loop.

2. **LvglDisplay / new `StrokePracticeOverlay` under `main/display/` (UI owner)**  
   Overlay on `lv_screen_active()`: N tappable labels/buttons (`LV_EVENT_CLICKED`) + a canvas/line layer that animates strokes with `lv_anim` or polyline morph. All LVGL access under `DisplayLockGuard`. API on `Display` should stay narrow (`ShowHanCandidates`, `PlayStrokeOrder`, `DismissOverlay`) with no-ops on `NoDisplay`/`OledDisplay`/`EmoteDisplay`. Do **not** fork `LcdDisplay::SetupUI()` per board.

3. **Board layer (capability only)**  
   Keep creating panel + optional `lvgl_port_add_touch`. Optionally report `width/height` (already on `Display`) and whether an LVGL indev exists. Do not implement candidate layout in `boards/**`.

4. **Assets / fonts**  
   Candidate Han needs either pushed glyphs (existing 256-entry cache) or a dedicated large CJK font in assets. Stroke geometry should **not** be stuffed into `DynamicGlyphCache` (it is a bitmap font fallback). Prefer compact vector strokes from the server, or a small GIF/EAF only if vectors are too heavy. GIF reuse via `LvglGif` is already proven for emotion.

5. **Non-touch fallback**  
   If no LVGL indev: speak index / boot-button cycle / knob. That policy belongs in Application, not LcdDisplay.

6. **Style gates**  
   Gate the overlay on `HAVE_LVGL && LcdDisplay && width/height above OLED`. Exclude `USE_EMOTE_MESSAGE_STYLE`. WeChat vs default style should share the overlay, not two copies.

## 7. Key files

- `main/display/display.h`, `display.cc` — abstract UI contract, `NoDisplay`, `DisplayLockGuard`
- `main/display/lcd_display.h/.cc` — LVGL port init, SPI/RGB/MIPI, SetupUI, emotion GIF, chat
- `main/display/lvgl_display/lvgl_display.h/.cc` — status bar, glyph cache hook, snapshot, power-save emoji
- `main/display/lvgl_display/{lvgl_theme,lvgl_font,lvgl_image,dynamic_glyph_cache,emoji_collection}.*`
- `main/display/lvgl_display/gif/lvgl_gif.*` — timer-driven frame animation
- `main/display/oled_display.*`, `emote_display.*`
- `main/display/text_glyph.h/.cc`, `main/protocols/text_glyph_payload.*`
- `main/boards/common/{board,button,backlight,knob,power_save_timer}.*`
- `main/application.cc` — `SetupUI`, protocol → display, `ToggleChatState`
- `main/mcp_server.cc` — screen MCP tools
- `main/assets.cc` — font/emoji/skin apply
- `main/Kconfig.projbuild`, `main/CMakeLists.txt`, `main/idf_component.yml`
- Representative boards: `boards/lckfb/szpi-esp32s3/lichuang_dev_board.cc` (LVGL touch); `boards/freenove-esp32s3-display-2.8-lcd/*.cc` (touch-as-button); `boards/bread-compact-wifi-lcd/` (color, no touch); `boards/waveshare/esp32-s3-touch-lcd-1.54/` (small color + CST816S indev); OLED bread boards; `boards/espressif/esp32-s3-box-3/` (emote-capable)

## 8. Gaps / risks

- Core UI is display-only; tap-to-select is greenfield even on boards that already add LVGL touch.
- Touch wiring is inconsistent (indev vs poll-as-button vs none). Feature must detect LVGL indev or degrade.
- CJK rendering depends on server glyph-push or custom assets; builtin font will not draw 候选汉字.
- No hardware validation in this pass. Overlay RAM (canvas buffer) and partition size need a later board-class estimate (S3/P4 + PSRAM vs C3 14px).
- Emote and OLED should be explicit non-goals for v1 interactive 笔顺.

- Core UI is LVGL 9.5 via esp_lvgl_port, with Display → LvglDisplay → LcdDisplay/OledDisplay; EmoteDisplay is a separate non-LVGL renderer gated by CONFIG_USE_EMOTE_MESSAGE_STYLE.
- Board code creates the esp_lcd panel and optionally lvgl_port_add_touch; Application::Initialize calls SetupUI; protocol stt/tts/llm updates must Application::Schedule and DisplayLockGuard.
- LVGL touch indev exists on many Waveshare/Espressif/Lichuang boards, but core UI has no clickable widgets; some boards instead poll touch as ToggleChatState.
- CJK is not in builtin Noto Sans Basic; runtime Han uses assets LvglCBinFont plus DynamicGlyphCache (256 glyphs / 64KB) filled by TextGlyphPayload.
- Animation primitives already in-tree are lv_anim (label scroll) and LvglGif (emotion); there is no canvas/line stroke path yet.

<!-- pi-squad:6a7d04cf4d3144dc4ef2d7079cc11946450542b0241a506eeaebd1b5739d5a1b -->
## 2026-09-12T06:50:40.722Z — stroke-ui-cores3-retry

In-place retry of the CoreS3 笔划 UI (no rollback/commit). Nested DisplayLockGuard removed; draw uses CopyLoadedGlyph caches, not cross-lock StrokeViews; overlay timer/canvas_buf lifecycle and generation-gated clicks tightened; CoreS3 I2C stays on the 20ms poll with atomic indev coords and exclusive overlay vs short-tap ToggleChatState. Smoke extra-file remains stroke_order.bin + APL/NOTICE. clang-format 19 clean. Host tests 93/93 including production controller harness. Isolated ESP-IDF v6.0.2 m5stack-core-s3 CONFIG_STROKE_ORDER_LOCAL=y build succeeded: /tmp/xiaozhi-stroke-ui-cores3-build/xiaozhi.bin 2879248 bytes, app partition 30% free (0x1310f0). Hardware not flashed. 一/人/口 is not a release 字库; no type=stroke/remote STT.

- DisplayLockGuard always Unlocks even if Lock fails, so nested guards are unsafe even though lvgl_port_lock is recursive; StrokeOrderView now takes the guard once at Attach/Shutdown/OnPowerSave/HandleControl/HandleStatePresentation.
- StrokeOrderStore::StrokeView aliases caller-owned blob bytes and is invalid after Unbind/LoadCharacter; the LVGL renderer copies decoded points via StrokeOrderController::CopyLoadedGlyph() into a View cache at page switch.
- Runtime local 笔划 gate is CONFIG_STROKE_ORDER_LOCAL + GetAssetData("stroke_order.bin") bind + LVGL pointer indev; CMake packages scripts/package_stroke_order_smoke.py output (bin, ARPHICPL.TXT, NOTICE.md) as extra-files under that exact name.
- Isolated ESP-IDF v6.0.2 CoreS3 build with CONFIG_STROKE_ORDER_LOCAL=y produced /tmp/xiaozhi-stroke-ui-cores3-build/xiaozhi.bin = 2879248 bytes (0x2bef10) with 0x1310f0 (30%) free in the 0x3f0000 app partition; generated_assets.bin contains stroke_order.bin.

<!-- pi-squad:199025b36a34a2ebd572022c5e25afb0a7e8be7e642a9a05116f86443a15085f -->
## 2026-09-12T10:13:26.067Z — stroke-stt-local-candidates-final-revision-retry

Finished the interrupted stroke-stt-local-candidates close-out without rollback, commit, stage, reset, or touching unrelated Stick-S3/emoji/8m work.

Harness isolation (the leftover-active-stroke bug): scripts/tests/stroke_round_integration_harness.cc now aborts + EnsureIdle() after the 100-round retired-identity replay. Round 101 replaying round-1 STT/TTS/LLM/audio still FailStroke (not accepted as current STT, not PassNormal). After abort the same ID Drops. Coordinator is inactive before the following 100 generation-0 normal binds. EnsureIdle is test isolation only; during_open still only publishes a pending cancel fence and does not sync AbortRound.

Static checks (production already present from the interrupted pass, unchanged this round):
- Async cancel sources publish StrokeRoundCoordinator::PublishCancelFence before event/schedule bits. ContinueOpenAudioChannel checks fence before and after blocking open; StartListeningAudio checks fence/pending/CanStartListening immediately before SendStartListening and EnableVoiceProcessing.
- Protocol capabilities: WebSocket SupportsCorrelatedSessionOpen+SupportsStrokeVoiceRouting true; MQTT both false. MQTT click entry uses local 一/人/口 candidates and never stroke-voice open. Ordinary MQTT STT/bind without a stroke round still passes.
- AudioService uses AudioStreamGenerationGate (BumpStreaming/AcceptCapture/AcceptPlayback). Host harness proves reset rejects stale in-flight encode/decode.

Validation actually run:
- python3 -m unittest scripts.tests.test_stroke_order_ui -v → 8 tests, 3.811s, OK. Log /tmp/stroke-stt-retry-targeted.log SHA-256 ead3692c773237eac0c64edc00173497e79f4fbbf4e344e84c937a22da91660e
- python3 -m unittest discover -s scripts/tests -v → 99 tests, 10.577s, OK. Log /tmp/stroke-stt-retry-host.log SHA-256 adfaab54e00e973569c6bded41838f5a436fc8e532dcf7564d1238d4771ffc0d
- clang-format 19.1.7 scoped dry-run on harness + application/audio/protocol/coordinator: PASS. git diff --check PASS. No extra format rewrite of large functional diffs or Stick-S3.
- Isolated ESP-IDF v6.0.2 CoreS3 incremental: idf.py -B /private/tmp/xiaozhi-stroke-stt-local-candidates-revision-build -D SDKCONFIG=.../sdkconfig. CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y, CONFIG_STROKE_ORDER_LOCAL=y. Ninja had no source rebuild (harness-only change). Worktree sdkconfig hash unchanged 0fbe527664a621cc0a468b168bfef9346362f76932d4b4ba7831b90eec4aacd1.
- Artifacts unchanged vs prior isolated build: xiaozhi.bin 2900880 81d92a03b2c3f39b7b8c7ee4c930dc13d9d6cb38ddc944e741630a29de6ebd92; elf 40022980 84247ab6b2e14d1e7b66cf0b6c8f45181d4fab38f1d83e0ce909439d75f61393; map 24523474 e4bbb91859f6760cd96bdf688eb0d796a2e8e39878064c31a4f0ef30f814d6ad; flash_args 203 928ffa71cfb830d3c13e28cea1491354655f13052a52c6a501b8e59d4973d6db; generated_assets.bin 1678548 4d52ca7fafc7d9ce65779a435357c0c8b12346b852de2da59bed7eb753cd4e90. App 0x2c4390 / partition 0x3f0000, 30% free.

Review mapping: B2 stale listen/start closed by fence+final gates; M1 MQTT late hello not claimed fixed—device fail-closes to local candidates; M2 production-shaped harness closed; audio generation seam closed; retired overflow is fail-closed and no longer leaves an active stroke that breaks later normal binds.

Residual: not flashed; no live WS/MQTT; MQTT late hello remains a transport issue; 3-char smoke corpus; codec-committed PCM cannot be recalled; 16-slot overflow fail-closes the current demo rather than silent Drop.

- StrokeRoundCoordinator retired-session ring is 16 entries. An older valid ID that has fallen out of that ring is not treated as retired; against an active stroke it is unknown and CaptureRoute returns FailStroke (fail-closed), not PassNormal and not silent Drop.
- FailStroke is a route decision only; it does not itself clear generation/phase. Tests and production abort must follow, or BindOpenedChannel(0) fails because IsActivePhase remains true.
- Protocol stroke voice requires both SupportsCorrelatedSessionOpen and SupportsStrokeVoiceRouting. WebsocketProtocol returns true/true; MqttProtocol returns false/false. MQTT click entry must MarkLocalCandidates and must not OpenAudioChannel/listen/start.
- AudioService ResetStreamingState bumps AudioStreamGenerationGate capture and playback together; in-flight encode/decode may rejoin only when AcceptCapture/AcceptPlayback still match. Host helper AudioStreamCanRequeue/DropStale covers the same contract.
- Isolated CoreS3 firmware for this close-out lives at /private/tmp/xiaozhi-stroke-stt-local-candidates-revision-build with CONFIG_STROKE_ORDER_LOCAL=y; the worktree sdkconfig is not CoreS3 and must not be used to flash.

<!-- pi-squad:20471ebcc3a4cee65018758eebc09f2a1b59ad5c4818175deac70a2dab4c471d -->
## 2026-09-12T15:54:27.399Z — stroke-voice-autostop-engine-postcondition-finalize

同协议短收尾完成，未改源码。静态核对确认 AudioService enable/running/disable 均查询真实 AudioEngine::IsVoiceProcessingEnabled()，生产 helper 被 audio_service.cc 使用，harness+源码约束覆盖 engine/event/service 不一致。Application 仍为 fence→SendStartListening→EnableVoiceProcessing→IsAudioProcessorRunning→MarkListeningStarted→Speak；普通/wake RecoverOrdinaryListeningStartFailure 未回退。源码与 /private/tmp/xiaozhi-stroke-autostop-engine-pc-20260912-232736 快照 SAME，未重跑 clean。git diff --check 通过。本轮 host 119：Ran 119 tests in 16.683s OK (skipped=1)。复用 CoreS3 构建 complete：app 2910176 SHA 669e3b63169af57b56bb74714f369e64be9c42df16e8c149120fb032a9d55cbe，assets 2877806 SHA 6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0 不变。未打包未烧录。残余：真机 Connect→Speak/VAD/underrun/触摸回归；void EnableVoiceProcessing 若变异步仍有窗口。报告：stroke-voice-autostop-engine-postcondition-finalize.md。

- AudioService::EnableVoiceProcessing(true) 在 void engine enable 之后、SetBits 之前调用 audio_engine_->IsVoiceProcessingEnabled()，经 VoiceProcessingEnableMayPublishEvent 决定是否发布 AS_EVENT_AUDIO_PROCESSOR_RUNNING；engine 拒绝则 disable/清 event/返回 false。
- IsAudioProcessorRunning() 合取 service 未 stopped、engine initialized/present、engine->IsVoiceProcessingEnabled()、service event；service_stopped_ 在 engine/event 快照之后加载，Stop() 的 wake bit 不是 running 声明。
- Disable 成功必须 engine 实际 disabled 且 event 已清（VoiceProcessingDisableSucceeded）；engine 仍 enabled 时返回 false 并保持 event，不再用 !IsAudioProcessorRunning() 伪报成功。
- 生产 helper 在 main/audio/audio_processor_postcondition.h，由 audio_service.cc include 使用；host harness 覆盖 engine false+event、engine true+event false、stopped、stuck disable。integration FakeAudioService 只测 Application 策略，不替代 engine 后置条件。
- StartListeningAudio 成功顺序仍是 fence → SendStartListening → EnableVoiceProcessing(true) → IsAudioProcessorRunning → MarkListeningStarted → ShowListeningFromMain；generation 0 与 wake re-listen send 失败走 RecoverOrdinaryListeningStartFailure，ResetDecoder/popup 只在 wake send 成功分支。
- 复用 CoreS3 仓库外构建 /private/tmp/xiaozhi-stroke-autostop-engine-pc-20260912-232736/build：CONFIG_STROKE_ORDER_LOCAL=y，xiaozhi.bin 2910176 SHA-256 669e3b63169af57b56bb74714f369e64be9c42df16e8c149120fb032a9d55cbe，generated_assets.bin 2877806 SHA-256 6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0 相对 500 字固件不变。本轮 host 119 为 16.683s OK skipped=1。
