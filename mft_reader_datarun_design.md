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

Further, the "good" record categories (decoded + extension + unresolved + free-slot) summed to **328,510**, while the first `UnexpectedData`
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
