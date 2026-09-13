## Proposal from tester
Run: squad-cd4e80a2-3c6a-404b-9559-42703668ed14

PROPOSAL: Add an explicit Application/AudioService shutdown sequence that disables and joins audio work and synchronously detaches callbacks before deleting the Application event group; do not rely on member destruction after the destructor body.
