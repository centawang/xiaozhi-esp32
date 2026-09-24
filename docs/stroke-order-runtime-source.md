# W4b read-only runtime source (opt-in, not a product switch)

This layer is host-integrated with the production Controller. It does **not** select
3500 assets, add an ESP-IDF source/dependency, create a worker, or alter the existing
View/Application path. Existing `BindCatalog`/`RebindStrokeOrderAssets` still use
SCB1/SPY1/SOB1, and the one-SOB1 smoke path is unchanged.

## Ownership and profile contract

`stroke_order_source.h` is the Controller boundary (no v2/link dependency):

- `Legacy2000`: exactly 2000 characters / 8 shards.
- `Level1_3500`: exactly 3500 characters / 1–32 shards.
- `GetInfo` only succeeds after profile bundle admission **and Commit**, while not
  suspended. Counts are read from validated inputs, not Controller constants.
- `Find` maps codepoint to official rank; `Homophones` preserves each reader's
  rank ordering, excludes the primary, and returns at most six.
- `Acquire(generation, codepoint)` only lends an already prepared, owned raw glyph.
  Its `StrokeView`s never reference compressed/mmap bytes. One outstanding borrow
  is allowed. `Release` checks source identity, generation and a unique token;
  duplicate/old releases cannot clear a later borrow, even within one generation.
- The adapter must outlive the Controller binding, workers and borrows. Call
  Controller `Unbind`, drain workers/borrows and successfully `Suspend` before
  destroying the source. A borrow must not be used after its matching Release.

`StrokeOrderBundleOwner` is an **owning lease**, not a bag of unpinned pointers.
Its returned catalog/pinyin/shard spans must be immutable for the lease's entire
lifetime. A production implementation must pin an Assets mapping and prevent
unmap/overwrite until its last lease is gone. The adapter keeps the owner alive
for staged and published readers. Host tests implement the owner with real fixture
file bytes. A naked `GetAssetData` wrapper without a pin is not sufficient.

## Worker → publish → UI sequence

1. On a worker, outside LVGL/display locks, call
   `PrepareBundle(base_generation, profile, owner, &prepared)`.
   v2 uses `ValidateBundle(..., ValidationMode::Structural)`, `Catalog`, `Pinyin`
   and `Shard` readers (complete metadata/mapping/stored-CRC admission, not glyph
   decoding; host build admission retains `Deep`, and PrepareGlyphs is strict);
   v1 validates catalog, pinyin, every shard/record and all rank mappings.
   Reader state is constructed in local temporaries. Failure preserves the live
   generation. There is one worker slot and one staged bundle, with no queue.
2. `Commit(prepared)` atomically swaps the validated state and advances generation.
   It rejects an active borrow or worker. The ticket carries a source identity and
   monotonic preparation token, so an old callback cannot commit a later staged
   bundle at the same base generation. Any newly admitted prepare replaces the
   staged slot; a failed one leaves no staged result to accidentally commit.
3. Call Controller `BindSource(&source)` for the new generation. This is bounded
   profile admission only; no validation/decompression. After source replacement
   a Controller bound to the previous generation is not ready until rebound.
4. Use `Homophones(generation, primary, ...)` to form primary + ranked extras,
   truncate/deduplicate to the same <=6 candidates the UI will present. On a
   worker call `PrepareGlyphs(generation, codepoints, count)` before exposing them
   to Controller/UI. This owns at most six raw glyphs, each <=16 KiB, with a
   temporary replacement cache of at most six more during preparation.
5. Controller `SetCandidatesFromPrimary(primary)` without a separate provider uses
   the source's homophones. Existing explicit providers still retain their old
   behavior. Candidate rendering and selection only Acquire/copy/Release these
   prepared glyphs. An unprepared glyph fails closed; no implicit decoding,
   validation or task creation occurs on the UI path. UI/playback operations are
   otherwise unchanged.

The caller must serialize candidate jobs with its existing session/cancel fence.
Corpus generation is not a replacement for the voice/UI round generation. A
new candidate cache is transactionally published by `PrepareGlyphs`; failure
preserves the prior cache. If an existing cache is borrowed while the worker is
preparing its replacement, the replacement is rejected rather than invalidating
the borrow. Fixed bounds apply regardless of corpus size.

## Suspend and unmap

`Suspend`/`Unbind` first disables new Acquire/metadata capability and advances the
corpus generation (without wrapping). It is nonblocking: **false means do not
unmap**, because a worker or borrow still exists. The existing borrower retains
valid owned raw views and may Release its original token. The worker may finish
validation but its obsolete result is discarded. Retry `Suspend` after they
finish; only **true** means all source cache/staged/published readers and leases
have been cleared. No background task, polling loop, condition-variable wait or
unbounded work queue is created by the adapter.

The existing `SuspendStrokeOrderAssets` void helper is for the legacy View path;
calling it alone is not sufficient for the new adapter. A future product caller
must honor the adapter's boolean drain result before allowing Assets unmap.

## Next stage (not implemented here)

- Under `CONFIG_STROKE_ORDER_LOCAL`, add `stroke_order_source_adapter.cc` and
  `stroke_order_v2.cc` to `main/CMakeLists.txt`, with the resolved heatshrink 0.4.1
  decoder dependency/include settings. Confirm 10/4 dynamic-decoder configuration;
  do not edit the approved reader/vendor files to bypass these checks.
- Keep asset-profile/resource selection separate and explicit; this work does
  not change the current 2000 package outputs or Kconfig defaults.
- Replace `StrokeOrderView::RebindAssetsLocked`'s synchronous new-profile bind with
  a bounded worker job and fenced completion published through the main task.
  `Attach`/`RebindAssets`, local candidate entry and STT candidate completion need
  preparation before `RenderCandidates`/`CopyCandidateGlyph`. Never call either
  Prepare method from the LVGL callbacks or while holding the display lock.
- Coordinate the main-task publication with Application's existing scheduling and
  cancel fences. Rebuild capability only after Commit + Controller bind + pointer
  input admission; preserve current touch/animation semantics.
- `Assets::UnApplyPartition` currently delegates to View suspend.
  Extend that path to close adapter admission, cancel pending main-task publishes,
  drain workers/borrows, clear readers and only then approve unmap. Do not block
  while holding LVGL waiting for a worker that might need the same lock.
- Rollback to a prior live bundle requires that its mapping remains pinned while
  the replacement is prepared. If a flash-update flow must first unmap/overwrite
  the only available mapping, suspend first; it cannot promise old-state rollback
  after destroying the old storage.
- Measure ESP32-S3 worker stack, PSRAM/internal heap, latency and actual assets
  partition size; then run feature-on/off IDF builds and hardware UI/audio tests.
  This host phase does not establish those results. Controller vector-copy OOM
  handling remains the existing implementation; the adapter's fault injection
  covers its nothrow state/cache/raw allocations and the approved v2 reader hook,
  not every STL/shared-owner allocation.

## Focused validation

`python3 -m unittest scripts.tests.test_stroke_order_source -v` builds production
Controller/source/v1+v2 readers and the verified heatshrink C decoder with
ASan+UBSan, `-fno-exceptions`, `-fno-rtti`, and warnings as errors. It loads both
repository fixtures directly, iterates all 5500 profile entries, checks every
homophone result against the corresponding production index, and exercises rank
and shard boundaries, 敦's ten readings, allocation failures, rollback, owned
lifetime, active borrow drain, old tokens and controlled worker/Suspend ordering.
No optional input, download or IDF build is needed for this test.
