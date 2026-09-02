# P3-impl-7 Prerequisite Verification Report

**Status:** PASS
**Date:** 2026-09-02
**Scope:** Phase 1 (LC-1) — minimal DB-layer `MetadataDb::SetReconstructionStatus` prerequisite. No spatial-model or schema changes.

---

## 0. Objective

Per the approved P3-impl-7 authorization (two strict phases), Phase 1 implements only the minimal database-layer prerequisite identified in LC-1 of `docs/architecture/P3-impl-7-blocker-resolution.md`: transition an existing `ReconstructionRow` to a new status (the P3-impl-7 use case is `"succeeded" → "superseded"`).

Explicit constraints honored: no migration, reuse of the existing `ReconstructionRow.status` field and `"superseded"` vocabulary, no redesign of reconstruction lifecycle semantics, no modification of P2.5 reconstruction domain types, no JSON schema changes, no COLMAP adapter changes.

---

## 1. Files changed

| File | Change |
|------|--------|
| `core/storage/metadata_db.h` | Added `SetReconstructionStatus(const Uuid&, const std::string&)` declaration (+11 lines, after the `FindReconstructionsByScene` block) |
| `core/storage/metadata_db.cpp` | Added `SetReconstructionStatus` definition: SELECT current status → validate transition → UPDATE status (+73 lines, after `FindReconstructionsByScene`) |
| `tests/unit/test_reconstruction.cpp` | Added 7 focused unit tests (+108 lines, `ReconstructionDbTest` suite) |

**Scope guard:** No other files modified. No migration added. No schema files touched. No `core/trajectory/*`, `core/reconstruction/*`, or `adapters/colmap/*` changes.

---

## 2. API contract

```cpp
void MetadataDb::SetReconstructionStatus(const Uuid& reconstruction_id,
                                         const std::string& new_status);
```

- **No migration.** Reuses the existing `reconstructions` table and its `status` column (migration 0007).

- **Allowed transitions** (validated before the UPDATE):

  | From | To |
  |------|----|
  | `"reconstructing"` | `"succeeded"` \| `"failed"` \| `"superseded"` |
  | `"succeeded"` | `"superseded"` (the P3-impl-7 use case) |
  | `"failed"` / `"superseded"` | none (terminal states) |

- **Transaction semantics:** the method performs a single-statement `UPDATE` (preceded by a read of the current status), consistent with the existing `AddReconstruction` pattern. SQLite WAL mode makes the UPDATE atomic; it executes within the caller's existing connection.

- **Error conditions (all throw `StorageError`):**
  - read-only database → `kStorageReadOnly`;
  - row not found → `kStorageIo` with a descriptive message;
  - invalid transition (e.g. `"superseded" → "succeeded"`) → `kStorageIo`.

---

## 3. Test results

### New tests (7)

| # | Test | What it verifies |
|---|------|------------------|
| 1 | `SetStatusSucceededToSuperseded` | Valid `"succeeded" → "superseded"`; row reflects new status; `QueryLatestReconstructionByScene` no longer returns it (superseded excluded from "latest") |
| 2 | `SetStatusReconstructingToSucceeded` | `"reconstructing" → "succeeded"` valid transition |
| 3 | `SetStatusUnknownIdThrows` | Nonexistent `reconstruction_id` throws `StorageError` |
| 4 | `SetStatusTerminalTransitionThrows` | `"superseded" → "succeeded"` throws `StorageError` |
| 5 | `SetStatusFailedToSucceededThrows` | `"failed" → "succeeded"` throws `StorageError` |
| 6 | `SetStatusReadOnlyThrows` | Read-only DB throws `StorageError` |
| 7 | `SetStatusPersistsAfterReopen` | Status persists after DB close/reopen; document_json and coordinate_frame unaffected |

### Full suites

| Configuration | Tests | Pass | Fail |
|---------------|-------|------|------|
| Debug (full ctest) | 571 | 571 | 0 |
| Release (full ctest) | 571 | 571 | 0 |

Total test count increased from 564 → 571 (+7).

---

## 4. Verification commands (as executed)

```powershell
cmake --build --preset default          # Debug build
ctest --test-dir build/default -C Debug
cmake --build --preset release         # Release build
ctest --test-dir build/release -C Release
```

Both Debug and Release: **571/571 PASS**.

---

## 5. Architecture / boundary / schema gates

| Check | Result |
|-------|--------|
| `check_dependencies.py` | **PASS** (6 conan packages, all registered, permissive licenses) |
| `check_schemas.py` | **PASS** (18 JSON schemas, 7 migrations, schema.sql present) |
| `check_worker_boundary.py` | **PASS** (25 files scanned, no direct DB/scene access from workers) |
| `check_arch_debt.py` | **PASS** (149 files scanned, no debt markers) |
| `check_rfc.py` | **PASS** (39 ADRs, 8 RFCs, index consistent) |
| `check_domain_types.py` | **FAIL (pre-existing, NOT a Phase-1 regression)** — only `core/trajectory/pose_graph_helpers.h:48,69` (6c debt, `Eigen::Vector3d` outside allowed dirs). This file was **not modified** by Phase 1 (confirmed via `git status`/`git diff`). |

The domain-types failure is the previously-documented, pre-existing 6c debt; it is unchanged by this increment.

---

## 6. Findings & residual risks

| # | Finding | Severity | Mitigation |
|---|---------|----------|------------|
| 1 | `SetReconstructionStatus` returns no row count / no bool; a missing row is detected by reading the current status first and throwing. | Low | Documented; matches the existing `AddReconstruction` throw-on-error pattern. |
| 2 | Invalid transitions throw `kStorageIo` (there is no dedicated `kStorageRowNotFound`/`kStorageConstraint` code). | Low | Clear messages distinguish "row not found" from "invalid transition"; consistent with existing `StorageError` usage in the file. |
| 3 | Pre-existing `check_domain_types` FAIL at `pose_graph_helpers.h:48,69` remains. | Pre-existing | Out of scope for P3-impl-7; tracked separately (6c debt). |

---

## 7. Verdict

**PASS.** Phase 1 (LC-1 prerequisite) is complete and verified:
- minimal DB-layer API added (no migration, no schema change, no spatial-model change);
- valid transitions enforced, terminal states and missing rows fail closed;
- persistence across reopen verified;
- Debug + Release full suites 571/571 PASS;
- all architecture/boundary gates PASS except the pre-existing, unrelated 6c domain-types debt.

**STOP — do not begin P3-impl-7 (Phase 2).** Phase 2 is authorized only after this Phase 1 verification is accepted.
