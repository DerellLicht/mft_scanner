# MFT Reader — Phase 3 Specification (Final)

Status: **Finalized. Option B selected for `FolderNode` storage. Ready for
implementation.**

Changes since v3.5 (draft):
- **Option A vs Option B resolved: Option B selected.** See "Storage
  option: A vs B" under Step 2.
- `FolderNode` struct finalized to include a merged `flatEntryIndex`
  field (the reverse-mapping value discussed for Option B is now a
  member of the struct itself, not a separate parallel vector).
- Added `folderIndexOf` as the one remaining lookup array Option B
  requires, and explained why it can't be merged away the same way
  `flatEntryIndex` was (different index space, needed on the hot path
  of Pass 2 linking).

---

## Terminology

- **MFT record** — a raw, fixed-size record read from the volume's Master
  File Table, identified by its record number (its position in the MFT).
- **`$FILE_NAME` attribute** (type `0x30`) — the attribute inside an MFT
  record that carries a name, a parent-record reference, and a namespace
  tag. A record may carry more than one (see Step 1, name selection rule).
- **Entry** — the umbrella term used below for "a thing found in the MFT,"
  before we know or care whether it ends up wired into a tree. Used only in
  Step 1.
- **Node** — a folder's tree data (Step 2 onward): its `subdirs` and
  `files` vectors. Only folders get a node; a file is represented purely
  by its entry in the flat list from Step 1, referenced by record number
  from its parent's `files` vector.

---

## Step 1 — Flat Entry List (Linear Pass)

**Goal:** one sequential pass over all MFT records, producing a flat,
index-addressable list. No parent/child linking happens in this step — that
is deliberately deferred to Step 3, because MFT record order has no
relationship to directory hierarchy (a folder's record number can be higher
*or* lower than its children's).

### What gets captured per entry

| Field | Source | Notes |
|---|---|---|
| Record number | position in MFT | Doubles as this entry's index in the flat vector, so entry-at-index N corresponds to record N. Records that are unused/free get a "vacant" placeholder (or are tracked via a separate used-bitmap — open question, see below). |
| Parent record number | winning `$FILE_NAME`'s parent reference | We only need the low 48 bits (record number) of the parent file reference for linking purposes. We are *not* validating the sequence-number field of the reference at this phase — flag if you want that cross-check added (it protects against a stale reference pointing at a since-recycled record number, at the cost of extra bookkeeping). |
| `isDirectory` | MFT record header flags, bit 1 | Straight copy, no parsing needed. |
| Name | winning `$FILE_NAME`, decoded | See name selection rule below. |
| File size | `$DATA` attribute (resident or non-resident header) | Only meaningful for files; ignored/zero for directories. |
| In-use flag | MFT record header flags, bit 0 | Records not in use are skipped entirely — deleted files leave stale records behind until reused. |

### Name selection rule (the multi-`$FILE_NAME` gotcha)

A record can carry up to **two** `$FILE_NAME` attributes: one in the
Win32/POSIX namespace (the "real" long name) and one in the DOS namespace
(the 8.3 short name), distinguished by the `nameType` byte in the
attribute. Proposed rule:

- If a Win32 or POSIX namespace name exists, use it.
- Only fall back to the DOS-namespace name if that's the *only* `$FILE_NAME`
  present (this does happen for some system files).

### Output

A single vector, one slot per MFT record processed, indexable by record
number. This is "the linear list from Step 1" you referred to. No tree
structure exists yet at the end of this step — just flat, self-contained
records of "this record number is a [file|folder] named X, whose parent is
record number Y."

### Progress reporting

Per our earlier agreement: print the name of the first entry decoded, then
every 1000th one after that. This is purely a sanity check that decoding is
keeping up across ~580k total records (525,557 files + 56,216 folders on
the reference D: volume), not a real progress bar.

Made explicit: the interval is a `constexpr`, not a hardcoded literal, so
it's a one-line change to tune later:

```cpp
constexpr int PROGRESS_INTERVAL = 1000;
```

Start at 1000; once a pass is confirmed working, drop it lower or swap the
printed line for a `.` per record — same constant either way, only the
print statement's body changes. Same constant/pattern applies to Pass 2's
progress reporting below (Step 3), and either could get its own constant
if the two passes end up wanting different intervals.

---

## Step 2 — Data Structures

### `FlatEntry` (one slot per MFT record)

```cpp
struct FlatEntry {
    std::wstring name;
    uint64_t     fileSize {0};
    uint32_t     parentRecordNumber;
    bool         isDirectory {false};
};
```

Owns the name and parent linkage for every record. `FolderNode` (below)
deliberately does **not** duplicate either field — since `FlatEntry` and
`FolderNode` are indexed differently (see below), a folder's name or
parent is always reachable via its `flatEntryIndex`.

### Storage option: A vs B (resolved — Option B selected)

Two options were weighed for how `FolderNode`s are stored:

- **Option A (considered, not chosen):** one `FolderNode` slot per MFT
  record, uniform with `FlatEntry` — `index == record number` throughout.
  Simple (no lookup needed to go from a record number to its node), but
  wasteful: most MFT records are files, not folders, so most of those
  slots sit empty. At the reference volume's projected 85%-full size,
  this costs roughly an extra ~123 MB over Option B (see Memory Budget
  table below) for node slots that are never used.
- **Option B (selected):** `FolderNode`s are compacted to folder-count
  size only — no wasted slots. This means a folder's record number no
  longer equals its `FolderNode` index, so something has to translate
  between the two. That something is `folderIndexOf` (below) — one small
  `uint32_t` array, cheap relative to the memory it saves.

Option B was chosen because the savings scale with volume size and the
cost (one `uint32_t` per MFT record, not per folder) is small and fixed
by comparison — see the Memory Budget section for the worked numbers.

### `FolderNode` struct (finalized)

```cpp
struct FolderNode {
    uint32_t               flatEntryIndex;  // which FlatEntry (record number) this node is for
    std::vector<uint32_t>  subdirs {};
    std::vector<uint32_t>  files {};
};
```

- `flatEntryIndex`: the `FlatEntry` index (i.e. MFT record number) this
  node was built from. This is the reverse-mapping value that earlier
  drafts discussed keeping in a separate lookup vector — it's merged
  directly into the struct instead, since it's always populated in
  lockstep with `subdirs`/`files` by the same Pass-2-setup loop, at the
  same index, with no case where one is touched without the other.
  Folding it in avoids a redundant parallel array, keeps it on the same
  cache line as the rest of the node's data, and removes any risk of the
  two getting out of sync.
- `subdirs`: the record number of every direct child *folder*.
- `files`: the record number of every direct child *file*.

"All subfolders of parent P" is simply `P.subdirs`; "all files directly
inside P" is simply `P.files`. No chain-walking, no "none" sentinel
needed — an empty vector already means "no children of that type." This
is recursive by construction: any folder named in a `subdirs` vector can
itself have a non-empty `subdirs`, with no special-casing needed at any
depth.

Both vectors hold indices (record numbers) into the flat `FlatEntry`
table from Step 1 — they don't duplicate name/size data, which already
lives there.

### `folderIndexOf` — the one remaining lookup array

Under Option B, `FolderNode`s no longer live at `index == record number`,
so Pass 2 (Step 3) needs the reverse direction: "given a record number
(e.g. a `parentRecordNumber` read off some entry), which `FolderNode`
slot does it correspond to?" `flatEntryIndex` on `FolderNode` doesn't
answer this — it goes the other way (`FolderNode` → `FlatEntry`), and the
two directions are genuinely different index spaces, so one can't be
folded into the other the way `flatEntryIndex` was folded into the
struct.

```cpp
std::vector<uint32_t> folderIndexOf; // sized totalRecordCount; sentinel for non-folder records
```

This is on Pass 2's hot path — it's consulted once per entry linked, up
to ~4.7 million times in the 85%-full projection — so it needs to be an
O(1) array lookup, not a scan. (A linear search through `FolderNode`s for
the matching `flatEntryIndex` was considered and rejected for this
reason: O(entries × folders) in the worst case is billions of
comparisons, versus O(entries) for the array. A scan remains fine for
rare, off-hot-path lookups — e.g. a debug dump — just not for anything
Pass 2 itself depends on.)

Cost: 4 bytes × `totalRecordCount` (every record, not just folders) —
this is the "one relatively small `uint32_t` vector" cost Option B pays
in exchange for compacting `FolderNode`s down to folder-count size. See
the Memory Budget table for how this nets out.

### Files: no dedicated node

Files are leaves — they never have children, so they don't need a struct
of their own. All file data (name, size, etc.) already lives in
`FlatEntry`; a file's record number simply gets pushed onto its parent's
`files` vector when Pass 2 (Step 3) runs. There's no separate "file node"
type and no chain field anywhere — the parent's `files` vector *is* the
complete record of "which files live directly in this folder."

This gives folders two independent vectors — `subdirs` (child folders)
and `files` (child files) — rather than any kind of linked chain. No
unified/mixed node type; no type-flag filtering needed when walking
either one.

---

### Resolution: unused MFT records — no bookkeeping needed

Since this is a read-only tool with an explicit re-read step for
freshness (no live-update tracking, no in-app delete), we don't need a
used/unused bitmap or an explicit tombstone entry. The flat vector from
Step 1 is simply sized to the total record count, so `index == record
number` holds throughout for `FlatEntry` (this guarantee is specific to
`FlatEntry`; it does not extend to the compacted `FolderNode`s under
Option B — see above). Records that turn out unused are left as
untouched, default-empty slots — Step 3's linking pass skips any slot
that was never populated. Nothing further to design here.

---

## Memory Budget (Final)

**Goal:** sanity-check RAM usage before Step 3 is built, since the flat
`FlatEntry` list is intended to stay resident for the whole run (the
folder tree only indexes into it — it doesn't duplicate name/size data).

### Assumptions (flag if any of these should change)

- **64-bit build** (Makefile default `USE_64BIT = YES`), so pointer/
  `size_t` fields are 8 bytes and `std::vector`/`std::wstring` control
  blocks are the larger 64-bit libstdc++ sizes.
- Average filename length assumed at **~24 UTF-16 characters** — this is
  an unverified estimate and is the single largest source of error here.
  If a tighter number matters later, it's worth measuring the real
  average on the reference D: volume.
- Record count is assumed to scale linearly with used file/folder count
  for the 85%-full projection (real `$MFT` growth also retains
  stale/deleted-but-unreused slots, so the true number is likely a bit
  higher than this projection — this is a lower-bound-ish estimate, not
  an upper bound).

**Caveat on the figures below:** libstdc++'s exact `std::wstring` layout
(SSO buffer size, allocator chunk overhead) is a real implementation
detail of the specific MinGW-w64/clang toolchain in use, not something
worth hand-deriving to the byte. Good enough for a sanity check; if the
85%-full case ever gets close to a real constraint, a five-minute
`sizeof(FlatEntry)`/`sizeof(FolderNode)` printout compiled with the
actual toolchain would be worth more than further refining this estimate
by hand.

### `FlatEntry` struct, 64-bit sizing

```cpp
struct FlatEntry {
    std::wstring name;               // ~32 bytes control block (64-bit, SSO ~7 wchar_t)
    uint64_t     fileSize {0};       // 8 bytes
    uint32_t     parentRecordNumber; // 4 bytes
    bool         isDirectory {false};// 1 byte (+ padding)
};
```

Fixed portion ≈ 48 bytes/entry with alignment. Names past the ~7-char SSO
threshold heap-allocate `(length+1)*2` bytes plus allocator overhead — at
the assumed ~24-char average that's roughly 50 bytes of string data plus
~16 bytes of 64-bit allocator overhead, call it ~66 bytes/entry. Working
figure used below: **~115 bytes/entry** for the `FlatEntry` table.

### `FolderNode` struct, 64-bit sizing

A `std::vector` control block is 3 machine words — 24 bytes on 64-bit.
Two empty vectors (`subdirs` + `files`) per `FolderNode` → ~48 bytes for
the vectors, plus 4 bytes for `flatEntryIndex` (+ padding) → **~56
bytes/node**.

### Two reference cases

Current (10.5% full, 420GB/4TB): 525,557 files + 56,216 folders =
**581,773 entries**.

Projected 85% full (linear scale from current counts, factor ≈
85/10.5 ≈ 8.10×): ≈4,255,000 files + ≈455,000 folders ≈
**4,710,000 entries**.

| | Current (10.5%) | Projected (85%) |
|---|---|---|
| `FlatEntry` table (~115 B/entry) | ~67 MB | ~542 MB |
| Tree, Option A (uniform, ~48 B/record, no `folderIndexOf` needed) | ~28 MB | ~226 MB |
| Tree, Option B (`FolderNode`s @ folder-count only, ~56 B/node + `folderIndexOf` @ 4 B/record) | ~5.5 MB | ~44 MB |
| **Total, Option A** | **~95 MB** | **~768 MB** |
| **Total, Option B (selected)** | **~72.5 MB** | **~586 MB** |

Option B comes out ahead in both cases, and the gap widens with volume
size — at the 85%-full projection it saves roughly 180 MB over Option A,
for the fixed cost of one 4-byte-per-record lookup array. That's the
trade this spec settles on.

### Conclusion

Even the Option B worst case modeled here (85% full) comes in around
~586 MB — well inside a 64-bit process's address space. No
memory-pressure concern is expected at 4TB scale under these
assumptions. This estimate should be revisited if the average-filename-
length assumption turns out to be significantly off, or against a real,
much larger, or much more densely-populated volume.

---

## Step 3 — Building the Tree from the Flat List

### Overview: three stages total

- **Pass 1 (Step 1, already specified):** sequential read of every MFT
  record, producing the flat vector — record number, parent record
  number, name, type, size. No linking, and no `FolderNode`s exist yet at
  the end of this stage — only `FlatEntry`.
- **Setup:** now that Pass 1 has determined the total record count and
  (in the same pass, or a quick second scan) the folder count, allocate
  the Option-B storage in one shot:
  - `FolderNodes.resize(totalFolderCount)` — one empty node (two empty
    vectors + a `flatEntryIndex` slot) per folder, not per record.
  - `folderIndexOf.resize(totalRecordCount, SENTINEL)` — one entry per
    *record*, initialized to a sentinel value (e.g. `UINT32_MAX`) meaning
    "not a folder."
  - While populating `FolderNodes`, also write each folder's assigned
    slot index into `folderIndexOf[recordNumber]` and its record number
    into `FolderNodes[slot].flatEntryIndex` — the two arrays are built
    together, in the same loop.
  This stage is what makes Pass 2 valid: it guarantees every folder's
  `FolderNode` slot already exists, empty and ready to be populated,
  and that `folderIndexOf` can resolve any record number to that slot,
  before Pass 2's linking loop ever starts.
- **Pass 2 (this step):** walk the flat vector once, and for every
  populated entry, use `folderIndexOf[parentRecordNumber]` to find the
  parent's `FolderNode` slot, then push the entry's own record number
  onto that node's `subdirs` or `files` vector. This only works *because*
  the Setup stage already guarantees the parent's `FolderNode` slot
  exists and is reachable via `folderIndexOf` — Pass 2 never creates a
  node, it only ever populates one that's already there. Visit order
  within Pass 2 doesn't matter for correctness: attaching entry E to
  parent P only touches P's already-existing slot, regardless of whether
  P's own entry has itself been attached to *its* parent yet.

### The linking step, per entry

For each populated slot in the flat vector (in any order — index order is
fine and simplest):

1. Look up `parentSlot = folderIndexOf[entry.parentRecordNumber]`. If
   this is the sentinel, the parent isn't a folder or wasn't found — see
   Orphaned entries, below.
2. If this entry is a **folder**: `push_back` its own record number onto
   `FolderNodes[parentSlot].subdirs`.
3. If this entry is a **file**: `push_back` its own record number onto
   `FolderNodes[parentSlot].files`.

### Why appending is cheap

Attaching a child to its parent is just `push_back`, which is amortized
O(1) — there's no chain to walk and nothing to find a "tail" of. The
whole pass is O(n) with no scratch bookkeeping needed beyond the
`folderIndexOf` lookup itself, which is also O(1) per entry. (`push_back`
does still trigger occasional reallocation-and-copy as a vector grows,
same as any `std::vector` — worth an eventual `reserve()` if a folder's
child count is known ahead of time, but that's a minor tuning detail, not
a correctness concern.)

### Root and edge cases

- **Root directory:** NTFS reserves record number 5 for the volume root,
  and its own `$FILE_NAME` parent reference points back at itself. This
  needs a specific check (`recordNumber == parentRecordNumber`) so it's
  recognized as the tree root and never attached into any parent's
  `subdirs`/`files` vector — it *is* the starting point Step 4
  (walking/printing) begins from.
- **Orphaned entries:** an entry whose parent record number resolves to
  the `folderIndexOf` sentinel (not a known folder) has nowhere valid to
  attach. This is a symptom, not a diagnosis — actual on-disk corruption
  is only one of several causes, and probably not the most common one:
  1. **Scanning a live, mounted volume.** We're not taking a snapshot
     first, so the volume can change between when an entry's parent
     record number is read (Pass 1) and when Pass 2 tries to resolve
     that parent. A folder deleted or moved mid-scan can produce a
     momentary, legitimate orphan with no corruption involved. Likely
     the most common real-world cause.
  2. **Reserved system records** (the first ~16 MFT records: `$MFT`,
     `$MFTMirr`, `$LogFile`, `$Volume`, `$Bitmap`, `$Boot`, etc.) may not
     carry "normal" parent linkage, and could surface as pseudo-orphans
     even on a perfectly healthy volume — worth excluding/special-casing
     rather than lumping in with genuine anomalies.
  3. **Actual corruption** (bad sectors, unclean shutdown) — real, but
     rarer than the above two in practice.

  **Handling:** orphans are tracked, not discarded, and not silently
  merged into the main tree. During Pass 2, any entry whose parent slot
  resolves to the sentinel gets its record number pushed onto a separate
  vector — `std::vector<uint32_t> orphanedRecordNumbers` — instead of
  onto a `subdirs`/`files` vector. The entry's own `FlatEntry` data is
  untouched either way, so nothing extra needs to be stored; the orphan
  list is purely "which record numbers didn't resolve," resolvable back
  to full detail via `flatEntries[recordNumber]` same as any other entry.

  Two further splits worth keeping, since they're diagnostically
  different things (live-volume artifact vs. accident of NTFS layout
  vs. genuine damage), even though it's the same underlying mechanism:
  - Reserved system records (record numbers 0–15) are excluded from
    `orphanedRecordNumbers` entirely and tracked separately — call it
    `systemRecordNumbers` — so they don't inflate the orphan count with
    something that's expected on every healthy volume. This is also the
    list to dig through if the reserved-records themselves are of
    interest.
  - Everything else that fails to resolve goes into
    `orphanedRecordNumbers` as a single list for now (cause #1 vs. #3
    above aren't distinguishable from the data alone at this scan
    time — a mid-scan deletion and real corruption look identical from
    Pass 2's point of view). If that distinction matters later, it'd
    need to come from re-checking the parent's status after the scan
    completes (e.g. "does record P exist *now*, just not when this
    entry's parent pointer was read?"), which is a Step 4+ concern, not
    Step 3.

  Neither list is walked or printed by the default tree output — Step 4
  starts from the root and walks `subdirs`/`files` only, so orphans and
  system records are invisible unless something explicitly asks for
  them (e.g. a `--show-orphaned` / `--show-system` flag, or just a
  logged count at the end of the scan: "N orphaned entries, M system
  records"). Whether that surfaces as a real flag now or stays a "count
  only, dig deeper via debugger" thing for now is a Step 4 UI decision,
  not a data-structure one — the point of this section is only that the
  data itself isn't thrown away.

### Progress reporting

Same rule as Pass 1: print the first entry linked, then every 1000th
after that, as a sanity check that Pass 2 is proceeding — separate from
Pass 1's own progress output, so we get visibility into both passes
independently.

### What Step 3 deliberately does *not* cover

Sorting children (alphabetical, by size, etc.) and the actual tree-print
walk (Step 4-ish, the "1st filename found + every 1000th" test you
proposed originally) are output-time concerns, not build-time ones — the
`subdirs`/`files` vectors are populated in whatever order Pass 2 happened
to encounter entries, not any particular display order. Worth a short
conversation of its own once linking is confirmed working, rather than
folding it in here.
