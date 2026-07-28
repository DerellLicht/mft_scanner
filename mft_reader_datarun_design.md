# MFT Reader — $MFT Data-Run Decoding Design Note (Draft)

Status: **Draft for review.**

## Why this is needed

Every record lookup so far assumes the `$MFT` is one contiguous run on
disk, starting at `mftStartCluster` from the boot sector:

```cpp
seekPos.QuadPart = mftOffsetBytes + recordNumber * mftRecordSize;
```

A live run against D: showed this assumption is false for this volume.
`ReadMftRecord`'s new signature classification (added to test the
theory) found **819,911 of 1,147,648 scanned records** (71%) reading a
signature that's neither `"FILE"`, `"BAAD"`, nor all-zero — i.e. bytes
that aren't MFT record data at all, consistent with the linear-offset
formula walking off the end of the `$MFT`'s true first extent and into
unrelated volume data.

Further, the "good" record categories (decoded + extension + unresolved
+ free-slot) summed to **328,510**, while the first `UnexpectedData`
record was **303,488** — meaning good records exist both before *and*
after that point. A single clean boundary can't produce that; the real
picture is a `$MFT` broken into **multiple scattered extents**, not one
contiguous run followed by garbage. This tracks with a volume that's
been in heavy use for years, where `$MFT` growth picks up whatever free
space happens to be available at the time rather than one reservation.

The fix: decode record 0's own `$DATA` attribute as a real non-resident
attribute (data runs), build the true VCN→LCN extent map, and resolve
every record lookup through that map instead of the linear formula.

## Terminology

- **VCN (Virtual Cluster Number)** — a cluster's position *within the
  attribute's own data*, counting from 0 at the start of the attribute.
  Independent of where that data physically sits on disk.
- **LCN (Logical Cluster Number)** — a cluster's position on the
  *volume*, i.e. its real physical location, counting from 0 at the
  start of the volume.
- **Data run** — one contiguous span of VCNs that maps to one
  contiguous span of LCNs. A non-resident attribute's content is stored
  as an ordered list of these runs; each run covers where the previous
  one left off in VCN space, but can jump anywhere in LCN space.
- **Run list** — the on-disk encoded byte sequence describing all of an
  attribute's data runs, found at `dataRunOffset` (already a field on
  `ATTR_NONRESIDENT_HEADER`, unused until now) within the attribute
  record.

## On-disk run-list encoding

Each run is a variable-length record:

| Bytes | Meaning |
|---|---|
| 1 header byte | Low nibble = byte count of the "length" field below. High nibble = byte count of the "offset" field below. `0x00` marks end of the run list. |
| `length` bytes | Unsigned, little-endian: cluster count of this run. |
| `offset` bytes | Signed, little-endian, sign-extended from its top bit: LCN delta *from the previous run's LCN* (first run's delta is effectively absolute, since the running LCN starts at 0). Can be negative — a run can sit physically *before* the previous one. |

A run whose header byte has offset-byte-count `0` (only the length
nibble is nonzero) is a **sparse run** — no physical allocation, reads
as all zero. `$MFT` is not expected to have sparse runs in practice
(that's mainly a sparse-file feature), but the decoder should detect
and flag one defensively rather than silently mis-resolving it, since
we don't support reading sparse ranges yet.

The run list is walked sequentially, decoding one run at a time, until
the `0x00` terminator or the attribute's own `length` bound is reached
(same defensive stop-on-malformed-data posture as the rest of this
file's attribute walking).

## Data structures

```cpp
struct DataRunExtent
{
    uint64_t startVcn;     // first VCN this extent covers
    uint64_t clusterCount; // number of clusters in this extent
    uint64_t startLcn;     // first LCN this extent maps to (physical cluster number)
};
```

A `std::vector<DataRunExtent>`, built in on-disk run order (which is
already ascending-VCN order by construction), forms a complete,
gapless covering of VCN space `0 .. totalClusters-1` for the `$MFT`'s
`$DATA` attribute. "Gapless" holds as long as no sparse run is
encountered; a sparse run breaks that invariant and should abort
extent-map construction with a clear error rather than silently
producing an incomplete map.

## Decoding algorithm

```
runningVcn = 0
runningLcn = 0
offset = attributeStart + dataRunOffset

while true:
    headerByte = byte at offset; offset += 1
    if headerByte == 0x00: break

    lengthByteCount = headerByte & 0x0F
    offsetByteCount = (headerByte >> 4) & 0x0F

    clusterCount = read lengthByteCount bytes at offset as unsigned little-endian
    offset += lengthByteCount

    if offsetByteCount == 0:
        # sparse run - not supported yet, abort with a clear diagnostic
        return failure

    lcnDelta = read offsetByteCount bytes at offset as SIGNED little-endian
               (sign-extend from the top bit of the last byte read)
    offset += offsetByteCount

    runningLcn += lcnDelta
    extents.push_back({ startVcn: runningVcn, clusterCount, startLcn: runningLcn })
    runningVcn += clusterCount
```

Bounds are checked against the attribute's own `attr->length` on every
step, matching the defensive style already used in `WalkAttributes`/
`ExtractRecordInfo` — a malformed run list stops the walk rather than
reading past the record buffer.

## Resolving a record number to a physical byte offset

Given the extent map, `clusterSize`, and `mftRecordSize`:

```
byteOffsetInMftFile = recordNumber * mftRecordSize
vcn = byteOffsetInMftFile / clusterSize
offsetWithinCluster = byteOffsetInMftFile % clusterSize

extent = the DataRunExtent whose [startVcn, startVcn + clusterCount) contains vcn
         (binary search over extents, sorted by startVcn - O(log n) per lookup,
         cheap next to the disk I/O this is guarding)

physicalByteOffset = (extent.startLcn + (vcn - extent.startVcn)) * clusterSize
                      + offsetWithinCluster
```

This replaces the current `mftOffsetBytes + recordNumber * mftRecordSize`
formula everywhere it's used (currently just `ReadMftRecord`).

### Records spanning a run boundary

If `mftRecordSize` divides evenly into `clusterSize` (true here: 1024
into 4096), every record lives entirely within one cluster, so it can
never straddle two extents — the case above is complete as written.

If a future volume has `mftRecordSize > clusterSize` (e.g. 4096-byte
records on a 512-byte-cluster volume), a single record's byte range
could legitimately span a run boundary, requiring a multi-piece read
(one read per cluster, reassembled) rather than one contiguous read.
**Proposed for this pass:** detect the case (the record's start and end
VCN resolve to different, non-adjacent extents) and count it in a new
diagnostic counter rather than attempting the multi-piece read, since
it doesn't apply to the reference volume. Full split-read support can
follow later if a volume actually needs it.

## Integration points

- New function `DecodeDataRuns(recordBuf, attrOffset, nonResHeader) ->
  std::vector<DataRunExtent>` (or `bool` + out-param, matching this
  file's existing error-handling style), called once on record 0's own
  `$DATA` attribute during startup, before Step 1 begins.
- New function `ResolveRecordOffset(recordNumber, mftRecordSize,
  clusterSize, const std::vector<DataRunExtent>&) -> uint64_t`,
  encapsulating the binary-search resolution above.
- `ReadMftRecord` gains the extent vector (or a small resolver
  struct wrapping it + `clusterSize`) as a parameter, and calls
  `ResolveRecordOffset` instead of computing the seek position inline.
  `mftOffsetBytes` as a standalone parameter goes away once this lands,
  since the extent map replaces it entirely (an extent's `startLcn`
  already gives an absolute volume position - no separate "start of the
  MFT" offset is needed).

## Diagnostics / validation

- After building the extent map, print extent count and total covered
  clusters, and cross-check `total covered clusters * clusterSize` against
  record 0's own `$DATA` `allocatedSize` (already captured by
  `GetMftFileSize`) - a mismatch means the run list didn't fully decode
  and should be treated as fatal for Phase 3 (can't safely scan without
  a complete, verified extent map).
- Keep a counter for sparse-run rejections and one for
  span-a-run-boundary records (see above), even though neither is
  expected to fire on the reference volume - cheap insurance and useful
  if this gets pointed at a different volume later.
- Once this lands, `MftRecordReadResult::UnexpectedData` should drop to
  (ideally) zero on a re-run against D: - that's the acceptance check
  for this feature landing correctly.

## Out of scope for this pass

- `$ATTRIBUTE_LIST`-based continuation of the `$MFT`'s *own* `$DATA` run
  list, if the run list itself is too large to fit in record 0 (only
  plausible on an extremely fragmented `$MFT` - not expected here, but
  worth a defensive bounds check + clear error rather than a silent
  truncation if it's ever hit).
- Compressed non-resident attributes (a `compressionUnit != 0` case
  already noted in `ATTR_NONRESIDENT_HEADER`'s own comment) - `$MFT`
  itself is never compressed, so this doesn't block record lookups, but
  worth remembering if data-run decoding gets reused later for ordinary
  file `$DATA` attributes (e.g. a future phase that reads file content).

---

## Addendum: Live-volume consistency (Phase 3)

Status: **Implemented.**

This addendum isn't about data-run decoding itself - it covers a
related question raised once Phase 3 (the full-MFT flat pass and
folder-tree build) was actually run against a live, actively-used
volume (`C:`, the boot/OS drive, mid-scan while dozens of processes -
including several browsers - were running): does scanning a mounted,
changing volume with no locking or snapshotting invalidate the results?

It's placed here rather than in `mft_reader_phase3_final.md` because
the underlying concern is the same one that motivated data-run
decoding in the first place - trusting raw, unlocked reads against a
volume that can change out from under the scan - just showing up at a
different layer (record content and cross-record references, rather
than the `$MFT`'s own physical layout).

### (a) What we're looking at

A ~1.66M-record scan of a live, heavily-used volume takes some
measurable wall-clock time, during which the filesystem keeps changing.
Two distinct failure modes follow from that, and they're worth keeping
separate because they call for different checks:

1. **Torn reads.** `ReadMftRecord` does a single unlocked `ReadFile` of
   a record's bytes. If NTFS happens to be rewriting that exact record
   in the moment we read it, we can get a mix of old and new bytes -
   most visibly, a record whose header says "in use" but whose
   `$FILE_NAME` attribute doesn't decode (an in-use record with no name
   at all, landing in the "unresolved base record" diagnostic bucket
   from Step 1). A run against `C:` showed 1,190 of these, versus 4
   (all expected reserved slots 12-15) on a comparatively idle `D:` -
   strongly suggesting this bucket is dominated by live-volume churn on
   an active volume, not corruption.
2. **Stale cross-record references.** Even a perfectly clean read can
   describe a relationship that's no longer true: entry E's
   `$FILE_NAME` says its parent is record P, but if the original folder
   at P was deleted and record P got reused for something else *before*
   we got around to reading P itself, E's parent pointer is now stale -
   it names the right record number, but the wrong record generation.
   Nothing about reading P is torn or wrong in isolation; the two reads
   (E's and P's) just describe two different points in time.

This is the same category of problem the "Orphaned entries" section of
`mft_reader_phase3_final.md` already anticipated in its discussion of
scanning a live volume - this addendum is that anticipated problem
actually showing up, plus the two specific, narrow checks added to
detect it.

### (b) The fixes

Both checks are local, single-record comparisons against data already
being read off disk - neither involves locking, snapshotting, or any
coordination with other processes.

1. **Fixup checksum, now surfaced instead of discarded.**
   `ApplyFixup`'s return value (whether every sector's update-sequence
   check value matched - i.e. whether the read was clean, not torn) was
   already being computed but thrown away. `ReadMftRecord` now returns
   it via an `outFixupOk` parameter, and `BuildFlatEntryList` tracks two
   counts: how many in-use records had a mismatch overall, and -
   specifically - how many of the "unresolved base record" bucket also
   had one. A high overlap between "`$FILE_NAME` missing" and "fixup
   mismatch" is direct evidence for the torn-read explanation, rather
   than something else (e.g. a genuine parsing gap) being responsible.
2. **Parent sequence-number cross-check.** Every NTFS record reference
   - including a `$FILE_NAME`'s parent pointer - encodes not just a
   record number but the sequence number the target record was expected
   to have (`ATTR_FILENAME_HEADER::parentRecordReference`, previously
   only the low 48 bits were kept; the high 16 bits are now decoded
   too). Every `FlatEntry` also now carries its own record's actual
   on-disk `sequenceNumber` (captured unconditionally from the header,
   independent of whether its own `$FILE_NAME` resolved, since any
   record can be pointed at as someone else's parent). `BuildFolderTree`
   compares the two before linking an entry under a resolved parent
   slot: if `flatEntries[parentRecordNumber].sequenceNumber` doesn't
   match what the entry's own `$FILE_NAME` expected, the parent has
   been reused since, and the entry is not linked there.

### (c) Runtime handling when a check fails

Consistent with this codebase's existing posture (see `ApplyFixup`'s
own comment, and the "Orphaned entries" handling in
`mft_reader_phase3_final.md`): neither check is fatal, and neither
guesses. A failure just changes which bucket an entry lands in, so it's
visible rather than silently wrong:

- A **fixup mismatch** on its own does not exclude a record - the
  existing behavior (attempt to use whatever fixup could be applied)
  is unchanged. It's tracked as a count and, for the "unresolved base
  record" bucket specifically, reported as an overlap ratio.
- A **stale parent reference** (sequence-number mismatch) excludes only
  the link, not the record - the entry itself is still fully decoded
  and stored in `flatEntries`, exactly like an ordinary orphan. It's
  routed to `FolderTree::staleParentRecordNumbers`, a bucket kept
  separate from `orphanedRecordNumbers` on purpose: "this parent record
  number was never a folder" (orphan) and "this used to be the right
  parent, but has been reused" (stale) are different diagnostic
  situations even though both end in "not linked," and collapsing them
  would lose that distinction.
- Nothing added here changes program exit behavior - a live volume with
  some churn during the scan is expected, not an error condition worth
  stopping the whole run over. The counts exist so the *scale* of churn
  is visible run to run, not to gate success/failure.

### Resolution

Status: **Closed. Documented and accepted, not pursued further.**

The two checks above were added to distinguish between competing
explanations for the "unresolved base record" bucket - and they did:
a live run against a real, actively-used `C:` volume (1,190 such
records) showed 0 fixup mismatches, ruling out torn reads/live-volume
churn as the cause. A third, cheaper check added afterward -
`FlatEntry::hasAttributeList`, flagging whether a record's own
attribute list contains an `$ATTRIBUTE_LIST` (type `0x20`) attribute -
found the real answer: 1,186 of the 1,190 had one (the remaining 4 are
simply the reserved, always-nameless slots 12-15, unrelated to this
mechanism). That's conclusive: these are ordinary files whose
`$FILE_NAME` attribute was relocated to an extension record - almost
always because the record has enough attributes (most commonly from
heavy hard-linking, one `$FILE_NAME` per link) to overflow a single
1024-byte MFT slot - and `ExtractRecordInfo` only ever reads a
record's own attribute list, never follows `$ATTRIBUTE_LIST` to check
extension records for a relocated `$FILE_NAME`.

Two paths were considered for what to do with that answer:

- **Follow `$ATTRIBUTE_LIST` and recover these entries fully.** Real,
  correct, and would close the gap to (ideally) zero. Also a genuine
  sub-project, not a small addition: it means parsing
  `$ATTRIBUTE_LIST`'s own content (a list of attribute-location
  entries - type, name, starting VCN, and a record reference to
  wherever that segment actually lives), finding the entry for
  `$FILE_NAME` when it points somewhere other than the base record,
  and reading that specific other record - which Step 1's single
  linear, in-order pass over the `$MFT` isn't currently structured to
  do (the target record may not have been reached yet, or may
  currently be treated purely as an extension record and skipped
  outright).
- **Document it and stop here.** The failure mode is already safe -
  these records land in `orphanedRecordNumbers`, visibly excluded, not
  silently misattributed - and the scale is small: 1,190 out of
  1,664,256 records scanned (0.07%) on the one volume where this was
  ever significant (`D:`, a comparatively idle secondary drive, showed
  only the 4 expected reserved slots and nothing more).

**Decision: the second path.** This project's purpose was to build a
working, from-scratch understanding of how MFT reading actually works
- boot sector, data runs, record structure, attribute walking, tree
reconstruction, and now the specific edge cases a live scan surfaces -
not to reach production-grade completeness against every attribute-
overflow case NTFS can produce. That understanding is already
achieved; chasing the remaining 0.07% would mean roughly doubling the
scope of work already done, for a return that doesn't serve the
project's original goal. `$ATTRIBUTE_LIST`-based `$FILE_NAME` recovery
remains a well-understood, fully-scoped candidate if this code is ever
picked back up for a more complete pass - the diagnosis above is
exactly what a future implementation would start from - but it is not
planned.
