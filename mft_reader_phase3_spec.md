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

### Folders: sons-and-brothers, restated precisely

- Each **folder node** has exactly one `sons` field: an index pointing at
  its *first* child folder, or "none" if it has no subfolders.
- Every other subfolder sharing that same parent is reached by following
  the **`brothers`** chain starting from that first child — each folder
  node has a `nextBrother` index (or "none" if it's the last one).
- The set of "all subfolders of parent P" is therefore never stored
  directly on P — it's implicit, recovered by walking the `brothers` chain
  starting at `P.sons`.
- This is recursive by construction: any folder reached via a `brothers`
  walk can itself have a non-empty `sons`, starting its own subtree, with
  no special-casing needed at any depth.

Storage is index-based rather than pointer-based (indices into one flat,
contiguous vector of folder nodes), for the same reasons discussed
earlier: no pointer invalidation as the vector grows, better cache
behavior during tree walks, and it keeps the whole tree in one bulk
allocation instead of many small heap allocations.

### Files: proposed extension

Files can't have `sons` — they're leaves. Proposal, kept deliberately
parallel to the folder mechanism so the pattern stays familiar:

- Each **folder node** gains a `firstFile` field — index of the first file
  entry inside that folder, or "none."
- Each **file node** gains a `nextFile` field — index of the next file in
  the same folder, or "none" if it's the last one.

This gives folders two independent chains hanging off them — one for
subfolders (`sons` → `brothers` → `brothers` → ...), one for files
(`firstFile` → `nextFile` → `nextFile` → ...) — rather than one mixed
chain. Files and folders end up as two separate flat vectors (a *file
node* doesn't need a `sons`/`brothers` field at all, so giving it its own
struct avoids carrying dead fields).

### Decision: folders and files use separate chains — CONFIRMED

Files are their own list/vector per folder, structurally unrelated to the
`sons`/`brothers` chain. A folder node ends up with two independent
head-pointers — `sons` (first subfolder) and `firstFile` (first file) —
each leading into its own chain (`brothers` for subfolders, `nextFile`
for files). No unified/mixed node type; no type-flag filtering needed
when walking either chain.

A second, smaller open question: whether the "vacant" placeholder for
unused MFT records (Step 1) is worth carrying as an explicit tombstone
entry, or whether a separate used/unused bitmap alongside the flat vector
is cleaner. Doesn't affect Step 2's struct shape, just flagging it now so
it doesn't get forgotten before Step 3.

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

## Step 3 — Building the Tree from the Flat List

### Overview: two passes total

- **Pass 1 (Step 1, already specified):** sequential read of every MFT
  record, producing the flat vector — record number, parent record
  number, name, type, size. No linking.
- **Pass 2 (this step):** walk that flat vector once, and for every
  populated entry, attach it into its parent's chain. By this point every
  parent is guaranteed to already exist as a slot in the vector (even if
  that slot hasn't been linked into anything yet itself), because the
  vector was pre-sized to the full record count in Pass 1 — record order
  no longer matters for correctness.

### The linking step, per entry

For each populated slot in the flat vector (in any order — index order is
fine and simplest):

1. Look up the entry's parent record number directly as an index into the
   same flat vector — O(1), no search.
2. If this entry is a **folder**: attach it into the parent's
   `sons`/`brothers` chain.
3. If this entry is a **file**: attach it into the parent's
   `firstFile`/`nextFile` chain.

### Avoiding the O(n²) append trap

Attaching "at the end of a chain" naively means walking the whole
existing chain to find its tail — fine for a folder with 3 children,
expensive for one with thousands (and this drive has 525k files spread
across 56k folders, so some folders will be large).

Fix: during Pass 2 only, keep two **scratch** vectors, indexed by folder
record number, that are *not* part of the permanent node struct and are
discarded once Pass 2 finishes:

- `lastSubfolder[]` — the current tail of each folder's `brothers` chain.
- `lastFile[]` — the current tail of each folder's `nextFile` chain.

When entry E is attached to parent P:

- If P currently has no children of E's type yet (`sons`/`firstFile` is
  "none"), E becomes the head directly (`P.sons = E` or
  `P.firstFile = E`), and the scratch tail is set to E.
- Otherwise, the previous tail's `nextBrother`/`nextFile` is set to E,
  and the scratch tail is advanced to E.

This makes every attach O(1), so the whole pass is O(n) instead of O(n²).

### Root and edge cases

- **Root directory:** NTFS reserves record number 5 for the volume root,
  and its own `$FILE_NAME` parent reference points back at itself. This
  needs a specific check (`recordNumber == parentRecordNumber`) so it's
  recognized as the tree root and never attached into any chain — it *is*
  the starting point Step 4 (walking/printing) begins from.
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
chains as linked here are in whatever order Pass 2 happened to encounter
entries, not any particular display order. Worth a short conversation of
its own once linking is confirmed working, rather than folding it in
here.
