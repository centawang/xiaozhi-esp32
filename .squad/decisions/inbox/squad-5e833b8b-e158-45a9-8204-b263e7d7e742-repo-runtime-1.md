## Proposal from backend
Run: squad-5e833b8b-e158-45a9-8204-b263e7d7e742

Propose LcdDisplay overlay + LVGL click→Schedule→uplink for candidate confirm and stroke animation; gate on LCD+touch; do not reuse SetChatMessage, glyph_push, or a new DeviceState unless Idle/listen transitions prove they destroy the overlay.
