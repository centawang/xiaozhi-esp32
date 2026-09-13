

<!-- pi-squad:f2ce5c091edbc4974d684a7eb3a2ee12410d20b1d9c9e50d69f442cd3dfecfa9 -->
## animation — stroke-order-sequential-animation-fix
Mode: write; outcome: completed
Engine: a03d89c7-a928-4796-bc65-2a7bb1942deb

Implemented sequential-visibility repair within the same 12-file visual-only artifact. Only seven existing artifact paths changed: controller .cc/.h, shared UI-action/clock header, both harnesses, interaction Python tests, and interaction documentation.

Production clock and public Controller::Tick now cap each admitted automatic settlement to 33 ms. Successful settlement rebases to the admitted sample, discards whole-ms backlog, and preserves sub-ms remainder. A static duration check and two-phase visit bound prevent crossing more than one adjacent boundary. Pause uses the same bounded settlement inside its existing exact-generation/session/cancel-fence admission. Replay reset, Step/debounce, visual feedback and fence safety remain intact.

TDD observed: first added the exact production-shaped regression and ran it red under TSAN: modes 0/1/2 each printed done=0->2, then original assertion failed with return -6. Final TSAN and UBSAN executions return 0, all modes done=0->0; original assertion retained and strengthened with exact progress 283, adjacent snapshots and prior-partial-frame checks. Mutation probes fail when removing Pause settlement, fraction retention, baseline rebase or public Tick cap.

Validation: focused interaction suite 6 passed; focused UI suite 12 passed; shared and fresh clean-HEAD+patch full suites each passed 133 tests without skips. Earlier optional-input skip and wrong transcription-path invocations are retained transparently in evidence; final full runs use the verified pinned transcription checkout. Touched-range format checks, artifact diff checks, source hashes and protected logical scope audits pass.

Clean ESP-IDF v6.0.2 CoreS3 feature-on/off builds, merged images and ZIPs passed, with fullclean between variants. Compile audit: 7 stroke units on, 0 off, only CoreS3 board factory. App/assets sizes on: 2,918,096/7,568,207 bytes; off: 2,868,448/1,664,169 bytes. Both fit partitions and asset margins. Dependency audits verified 72 manifests/10,629 files without mismatches; all merged flash slices, ZIP payloads and app/bootloader image checksums passed.

Frozen patch: /tmp/stroke-sequential-evidence/artifact.patch; SHA256 81252835e03c4ea0116b0cb91405831f762cd2910edc96a098635fb7167da083. SOURCE-SHA256SUMS SHA256 faf9dedaa09452cd6a8059d637138221df4e0abfe76837f0255759d23db0c62f. Catalog remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Protected Application/audio/board/CMake/default-assets logical diff is zero; unrelated dirty non-Squad file hashes are preserved. No project staging, commit, push, publishing or Squad-state edits.

Feature-on package: /tmp/stroke-sequential-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip, SHA256 732fd3a1c4ef72114f0c3964470acf22fd3f4d8fa7303f0d5312a965753969ae. On merged SHA256 1c627e61ac6f4420d5ff61476976fbbcb93d275e1267e9ef779e461b6a50b2c7. Feature-off ZIP SHA256 56d1670c7ae20ca7aa47a678a0885c68f06c322eb1a7d6e57feb4db90cfe2af5.

Full findings written to the required output path. Report SHA256 1fcc05d7648dce09067d47515aa900a621b3ea2ef14b864f3dfe660f78de3d17. Evidence manifest SHA256 073559419468ac04506d48711d8ddd7a9f8fe23b94975717a54e0080f2b479bc; all 100 evidence-file checksums verified.

Independent Rai review remains pending. No hardware flashing/testing occurred. Host tests establish sequential controller/presentation opportunities, not physical panel visibility; LVGL canvas/flush coalescing and reference-outline perception remain explicit hardware gates, along with touch responsiveness, cancellation races, FPS, ordinary audio and 100-session stability.
