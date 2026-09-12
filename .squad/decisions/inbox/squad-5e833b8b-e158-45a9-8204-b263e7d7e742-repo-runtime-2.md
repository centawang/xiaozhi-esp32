## Proposal from backend
Run: squad-5e833b8b-e158-45a9-8204-b263e7d7e742

Propose keeping stroke playback off audio tasks and the main loop (LVGL timer); pause EnableVoiceProcessing during selection without closing the audio channel so auto-listen does not capture the next utterance.
