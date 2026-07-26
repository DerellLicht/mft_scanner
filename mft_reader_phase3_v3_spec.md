# MFT Reader — Phase 3 Specification (Draft for Review)

Status: **Steps 1 and 2 drafted below, pending sign-off. Step 3 not started —
depends on decisions made in Step 2.**

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
- **Node** — a thing that has been placed into the tree structure (Step 2
  onward). A node is either a *folder node* or a *file node*.

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
keeping up across ~580k total records (525,557 files + 56,216 folders, per
your NDIR32 count), not a real progress bar.

---

## Step 2 — Data Structures

### Folders: per-node child vectors, not sons-and-brothers

Earlier drafts of this step modeled subfolder linkage as an intrusive
linked list: each folder node carried a `sons` index (first child) plus
every child carried a `nextBrother` index, so the set of a folder's
children was recovered by walking a chain. That's the right shape *when
nodes are connected by pointers/indices to each other* — every node has
to help maintain the list, so every node needs both fields.

Revisiting `ndir32` (personal color directory lister) surfaced a cleaner
alternative already in use there. Its `dirs` struct, under the
`USE_VECTOR` build path, drops the sibling-chain fields entirely:

```cpp
struct dirs {
    std::vector<dirs> brothers {};
    std::wstring name {};
    uchar attrib {};
    // ... size/count fields
};
```

There's no separate `sons` field at all — the reason being that when
children live in a `std::vector` owned by the parent, the vector itself
*is* the set of children. No node needs to know its own position or link
to a sibling; the parent's vector already enumerates "all of my
subfolders" directly. The sibling-chain fields in the linked-list version
only existed to simulate what a vector gives you for free.

**Design update:** the folder node's `sons` / `nextBrother` pair is
replaced with a single field, an index vector naming the folder's direct
children:

- Each **folder node** has a `subdirs` field: `std::vector<uint32_t>`,
  containing the index (record number) of every direct child folder, in
  whatever order they were appended.
- "All subfolders of parent P" is simply `P.subdirs` — no chain walk
  required, no "none" sentinel needed (an empty vector already means "no
  subfolders").
- This is still recursive by construction: any folder named in a
  `subdirs` vector can itself have a non-empty `subdirs`, with no
  special-casing at any depth.

Naming: `brothers` doesn't fit anymore now that it's a container the
parent owns rather than a sibling link — it's not "my brothers," it's
"my children." **`subdirs` is the name going forward** (`children` was
the other candidate considered; `subdirs` was preferred as more specific
to this domain).

Storage stays index-based (record numbers as indices into one flat,
contiguous vector of folder nodes, one slot per MFT record) for the same
reasons as before: no pointer invalidation as the vector grows, better
cache behavior during tree walks, one bulk allocation instead of many
small ones. The `subdirs` vectors hold indices into that same flat node
vector, not copies of the nodes themselves — unlike `ndir32`'s
`std::vector<dirs> brothers`, which nests full child structs inline. The
index-vector approach was chosen here because record order doesn't match
hierarchy (Step 1 already populates the flat vector before any linking
happens), so nodes need to be addressable by record number independent of
where they sit in the tree.

### Files: proposed extension — updated to match

Files can't have subfolders — they're leaves. Keeping the pattern
parallel to the folder mechanism (now updated to vectors, same as the
folder change above):

- Each **folder node** gains a `files` field: `std::vector<uint32_t>`,
  containing the index of every file directly inside that folder.
- **File nodes no longer need a chain field at all** — no `nextFile`,
  since there's nothing for a file to link to; it's just referenced by
  the parent's `files` vector. (This is a further simplification beyond
  the earlier proposal, which still gave file nodes a `nextFile` field to
  mirror the folder chain — that mirroring is no longer needed once
  neither side uses a chain.)

This gives folders two independent vectors — `subdirs` (child folders)
and `files` (child files) — rather than two independent chains. Files and
folders remain two separate flat vectors (a *file node* still doesn't
need a `subdirs`/`files` field, so it keeps its own struct with no dead
fields).

### Decision: folders and files use separate containers — CONFIRMED (updated)

Files are their own vector per folder, structurally unrelated to
`subdirs`. A folder node ends up with two independent containers —
`subdirs` (child folders) and `files` (child files) — each a flat list of
indices into the corresponding flat node vector. No unified/mixed node
type; no type-flag filtering needed when walking either one.

A second, smaller open question carried over unchanged: whether the
"vacant" placeholder for unused MFT records (Step 1) is worth carrying as
an explicit tombstone entry, or whether a separate used/unused bitmap
alongside the flat vector is cleaner. Doesn't affect Step 2's struct
shape, just flagging it now so it doesn't get forgotten before Step 3.

---

### Resolution: unused MFT records — no bookkeeping needed

Since this is a read-only tool with an explicit re-read step for
freshness (no live-update tracking, no in-app delete), we don't need a
used/unused bitmap or an explicit tombstone entry. The flat vector from
Step 1 is simply sized to the total record count, so `index == record
number` holds throughout. Records that turn out unused are left as
untouched, default-empty slots — Step 3's linking pass skips any slot
that was never populated. Nothing further to design here.

---

## Memory Budget (Tentative — Safety Check)

**Goal:** sanity-check RAM usage before Step 3 is built, since the flat
`FlatEntry` list is intended to stay resident for the whole run (the
folder tree only indexes into it — it doesn't duplicate name/size data).

### Assumptions (flag if any of these should change)

- 32-bit build (Makefile default `USE_64BIT = NO`), so pointer/size_t
  fields are 4 bytes and `std::vector`/`std::wstring` control blocks are
  the smaller 32-bit libstdc++ sizes, not 64-bit ones.
- Average filename length assumed at **~24 UTF-16 characters** — this is
  an unverified estimate and is the single largest source of error here.
  If a tighter number matters later, it's worth measuring the real
  average on the reference D: volume.
- Record count is assumed to scale linearly with used file/folder count
  for the 85%-full projection (real `$MFT` growth also retains
  stale/deleted-but-unreused slots, so the true number is likely a bit
  higher than this projection — this is a lower-bound-ish estimate, not
  an upper bound).

### `FlatEntry` struct (one slot per MFT record)

```cpp
struct FlatEntry {
    std::wstring name;               // ~24 bytes control block (32-bit, SSO ~7 wchar_t)
    uint64_t     fileSize {0};       // 8 bytes
    uint32_t     parentRecordNumber; // 4 bytes
    bool         isDirectory {false};// 1 byte (+ padding)
};
```

Fixed portion ≈ 40 bytes/entry with alignment. Names past the ~7-char SSO
threshold heap-allocate `(length+1)*2` bytes plus allocator overhead;
at the assumed ~24-char average that's roughly 60 bytes/entry. Working
figure used below: **~100 bytes/entry** for the `FlatEntry` table.

### Tree storage: two options, worth deciding before Step 3

The current Step 2 text sizes the folder-node vector uniformly — "one
slot per MFT record," matching `FlatEntry`'s indexing scheme. But only
~10% of records are folders in the reference data, so ~90% of those
slots (24 bytes each, for two empty `std::vector<uint32_t>` control
blocks) are spent on records that never populate `subdirs`/`files`.

- **Option A — uniform (current spec):** folder-node vector sized to
  total record count. Simplest: one index scheme everywhere.
- **Option B — folder-only compact array:** `std::vector<FolderNode>`
  sized to folder count only, plus a `uint32_t` lookup array (record
  number → folder-node index, or a sentinel for non-folders) sized to
  total record count. Costs 4 bytes/record instead of 24 for the
  majority (file) records, at the cost of one extra indirection when
  walking from a record number to its folder node.

### Two reference cases

Current (10.5% full, 420GB/4TB): 525,557 files + 56,216 folders =
**581,773 entries**.

Projected 85% full (linear scale from current counts, factor ≈
85/10.5 ≈ 8.10×): ≈4,255,000 files + ≈455,000 folders ≈
**4,710,000 entries**.

| | Current (10.5%) | Projected (85%) |
|---|---|---|
| `FlatEntry` table (~100 B/entry) | ~58 MB | ~471 MB |
| Tree, Option A (uniform) | ~17.5 MB | ~141 MB |
| Tree, Option B (folder-only + lookup) | ~7 MB | ~58 MB |
| **Total, Option A** | **~76 MB** | **~612 MB** |
| **Total, Option B** | **~65 MB** | **~529 MB** |

### Conclusion (tentative)

Even the worst case modeled here (85% full, Option A) comes in around
~612 MB — comfortably inside a 32-bit process's usable address space
(typically ~2 GB, or 3-4 GB with `/LARGEADDRESSAWARE`). No memory-pressure
concern is expected at 4TB scale under these assumptions. Option B saves
roughly 2.4x on the tree structure for the cost of one lookup array, and
the saving grows with volume size — worth deciding before Step 3 locks in
the folder-node array's sizing logic, rather than revisiting it after code
is written against the uniform assumption. This estimate should be
revisited if the average-filename-length assumption turns out to be
significantly off, or against a real, much larger, or much more
densely-populated volume.

---

## Step 3 — Building the Tree from the Flat List

### Overview: two passes total

- **Pass 1 (Step 1, already specified):** sequential read of every MFT
  record, producing the flat vector — record number, parent record
  number, name, type, size. No linking.
- **Pass 2 (this step):** walk that flat vector once, and for every
  populated entry, attach it into its parent's `subdirs` or `files`
  vector. By this point every parent is guaranteed to already exist as a
  slot in the vector (even if that slot hasn't been linked into anything
  yet itself), because the vector was pre-sized to the full record count
  in Pass 1 — record order no longer matters for correctness.

### The linking step, per entry

For each populated slot in the flat vector (in any order — index order is
fine and simplest):

1. Look up the entry's parent record number directly as an index into the
   same flat vector — O(1), no search.
2. If this entry is a **folder**: `push_back` its own index onto the
   parent's `subdirs` vector.
3. If this entry is a **file**: `push_back` its own index onto the
   parent's `files` vector.

### The O(n²) append trap — no longer applies

The earlier sons-and-brothers design needed a scratch `lastSubfolder[]` /
`lastFile[]` pair (tail pointers per folder, discarded after Pass 2) to
avoid re-walking a whole chain every time something was appended to it.

With `subdirs`/`files` as real vectors, that problem is gone: appending a
child is just `push_back`, which is amortized O(1) on its own — there's
no chain to walk and nothing to find a "tail" of. The whole pass is O(n)
with no scratch bookkeeping needed. (`push_back` does still trigger
occasional reallocation-and-copy as a vector grows, same as any
`std::vector` — worth an eventual `reserve()` if a folder's child count
is known ahead of time, but that's a minor tuning detail, not a
correctness concern like the old O(n²) risk was.)

### Root and edge cases

- **Root directory:** NTFS reserves record number 5 for the volume root,
  and its own `$FILE_NAME` parent reference points back at itself. This
  needs a specific check (`recordNumber == parentRecordNumber`) so it's
  recognized as the tree root and never attached into any parent's
  `subdirs`/`files` vector — it *is* the starting point Step 4
  (walking/printing) begins from.
- **Orphaned entries:** an entry whose parent record number points at a
  slot that turned out to be unused/empty (data corruption, or a parent
  that genuinely doesn't exist for some reason) has nowhere valid to
  attach. Proposed policy: rather than silently dropping these, attach
  them under a synthetic "orphaned" folder node created at the start of
  Pass 2, so nothing found in the MFT is ever discarded without a trace.
  Flagging this as a default, not a final answer — say if you'd rather
  just skip/drop them, or handle differently.

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
