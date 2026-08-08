//****************************************************************************
//  This file extracts all of the system-level data structs
//  into a separate header file, so the actual code is more accessible
//****************************************************************************

// ---------------------------------------------------------------------
// NTFS boot sector (BIOS Parameter Block), exactly 512 bytes on disk.
// Field layout/offsets are per the documented NTFS on-disk format.
// Packed to 1-byte alignment so the struct maps directly onto the raw
// sector bytes with no compiler-inserted padding.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct NTFS_BOOT_SECTOR
{
    uint8_t  jump[3];
    uint8_t  oemId[8];              // should read "NTFS    "
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;       // always 0 for NTFS
    uint8_t  unused1[3];            // always 0
    uint16_t unused2;               // always 0
    uint8_t  mediaDescriptor;
    uint16_t unused3;                // always 0
    uint16_t sectorsPerTrack;
    uint16_t numberOfHeads;
    uint32_t hiddenSectors;
    uint32_t unused4;                // always 0
    uint32_t unused5;                // always 0
    uint64_t totalSectors;
    uint64_t mftStartCluster;
    uint64_t mftMirrStartCluster;
    int8_t   clustersPerMftRecord;   // if negative: record size = 2^|value|
    uint8_t  unused6[3];
    int8_t   clustersPerIndexBlock;  // same encoding as above
    uint8_t  unused7[3];
    uint64_t volumeSerialNumber;
    uint32_t checksum;
    uint8_t  bootCode[426];
    uint16_t bootSignature;          // 0xAA55
};
#pragma pack(pop)

// ---------------------------------------------------------------------
// Header common to every MFT record ("FILE record"). Attributes follow
// immediately after this header, starting at firstAttributeOffset.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct MFT_RECORD_HEADER
{
    uint8_t  signature[4];          // "FILE" (or "BAAD" if corrupt)
    uint16_t updateSeqOffset;
    uint16_t updateSeqSize;
    uint64_t logFileSeqNumber;
    uint16_t sequenceNumber;
    uint16_t hardLinkCount;
    uint16_t firstAttributeOffset;
    uint16_t flags;                 // bit 0: in use, bit 1: directory
    uint32_t realSize;
    uint32_t allocatedSize;
    uint64_t baseFileRecord;
    uint16_t nextAttributeId;
};
#pragma pack(pop)

// ---------------------------------------------------------------------
// Common header shared by every attribute in an MFT record, whether
// resident or non-resident. Resident/non-resident-specific fields
// (content offset/length, or data-run info) follow immediately after
// this and are attribute-type-specific - Phase 2's first milestone
// only needs `type` and `length` to walk the attribute list, so those
// specific layouts aren't modeled yet.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct ATTR_HEADER_COMMON
{
    uint32_t type;          // attribute type code; 0xFFFFFFFF marks end of list
    uint32_t length;        // total size of this attribute (header + content), 8-byte aligned
    uint8_t  nonResident;   // 0 = resident, 1 = non-resident
    uint8_t  nameLength;    // length of attribute name in UTF-16 characters, 0 if unnamed
    uint16_t nameOffset;    // offset from start of this attribute to its name
    uint16_t flags;
    uint16_t attributeId;
};
#pragma pack(pop)

// ---------------------------------------------------------------------
// Follows ATTR_HEADER_COMMON when nonResident == 0 (the attribute's
// content is small enough to store inline in the MFT record itself).
// Only the two fields needed for size reporting are modeled here -
// indexedFlag/padding exist on disk but aren't used yet.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct ATTR_RESIDENT_HEADER
{
    uint32_t valueLength;
    uint16_t valueOffset;
    uint8_t  indexedFlag;
    uint8_t  padding;
};
#pragma pack(pop)

// ---------------------------------------------------------------------
// Follows ATTR_HEADER_COMMON when nonResident == 1 (content lives
// elsewhere on disk, located via a data-run list). realSize/allocatedSize
// give accurate file-size figures directly from this header, with no
// need to decode the run list - that's why size reporting can skip run
// decoding entirely for now. dataRunOffset is kept here for
// documentation even though runs aren't decoded yet: if a future phase
// adds compressed-stream support, a compressedSize field appears between
// this header and the runs only when compressionUnit != 0, so
// dataRunOffset (not sizeof(ATTR_NONRESIDENT_HEADER)) must be used to
// find them.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct ATTR_NONRESIDENT_HEADER
{
    uint64_t startingVCN;
    uint64_t lastVCN;
    uint16_t dataRunOffset;
    uint16_t compressionUnit;
    uint32_t padding;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint64_t initializedSize;
};
#pragma pack(pop)

// ---------------------------------------------------------------------
// Follows ATTR_HEADER_COMMON + ATTR_RESIDENT_HEADER for a $FILE_NAME
// (type 0x30) attribute - these are always resident in practice. Holds
// the parent directory reference and namespace tag needed by Phase 3's
// flat-entry pass; timestamps are on disk but not modeled here since
// nothing in this phase reads them. The name itself (UTF-16LE,
// `nameLength` characters) immediately follows this struct in the
// attribute's content.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct ATTR_FILENAME_HEADER
{
    uint64_t parentRecordReference; // low 48 bits = parent record number, high 16 = sequence number
    uint64_t creationTime;
    uint64_t modificationTime;
    uint64_t mftModificationTime;
    uint64_t accessTime;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint32_t flags;
    uint32_t reparseValue;
    uint8_t  nameLength;   // in UTF-16 characters
    uint8_t  nameType;     // 0 = POSIX, 1 = Win32, 2 = DOS (8.3), 3 = Win32 and DOS
};
#pragma pack(pop)

static const uint8_t FILENAME_NAMESPACE_DOS = 2;

static const uint32_t ATTR_TYPE_END = 0xFFFFFFFF;

// ---------------------------------------------------------------------
// Phase 3 data structures - see mft_reader_phase3_final.md for the full
// design writeup. Summary:
//
//   FlatEntry   - one slot per MFT record (index == record number),
//                 produced by the Step 1 linear pass. Owns name, size,
//                 parent linkage, and directory flag for every record.
//   FolderNode  - one slot per *folder only* (Option B: compacted, not
//                 index == record number). subdirs/files hold child
//                 record numbers, which index back into FlatEntry.
//   folderIndexOf - sized to the total record count; translates a
//                 record number into its FolderNode slot (or the
//                 sentinel below if that record isn't a folder). This
//                 is the one lookup array Option B pays for in exchange
//                 for not allocating a FolderNode per file.
// ---------------------------------------------------------------------
struct FlatEntry
{
    std::wstring name;
    uint64_t     fileSize {0};
    uint32_t     parentRecordNumber {0};
    uint16_t     sequenceNumber {0};       // this record's own on-disk sequence number (MFT_RECORD_HEADER.
                                            // sequenceNumber) - captured for every in-use record regardless
                                            // of whether its own $FILE_NAME resolved, since any record can
                                            // be referenced as someone else's parent
    uint16_t     parentSequenceNumber {0}; // the sequence number this entry's $FILE_NAME expected its
                                            // parent to have, from the high 16 bits of parentRecordReference
                                            // - only meaningful when name/parentRecordNumber were resolved.
                                            // See BuildFolderTree's stale-parent-reference check and the
                                            // "Live-volume consistency" addendum in
                                            // mft_reader_datarun_design.md
    bool         isDirectory {false};
    bool         inUse {false};
    bool         isExtensionRecord {false}; // baseFileRecord != 0 - overflow attribute storage for
                                             // another record, never has its own $FILE_NAME, and is
                                             // never itself a node in the tree - see BuildFolderTree
    bool         hasAttributeList {false};  // type 0x20 ($ATTRIBUTE_LIST) present in this record's own
                                             // attribute list - a record with too many attributes to fit
                                             // in one MFT slot (most commonly from heavy hard-linking)
                                             // relocates some of them, including possibly its own
                                             // $FILE_NAME, to extension records. ExtractRecordInfo does
                                             // not follow this yet, so a record with hasAttributeList set
                                             // and no resolved name is the leading hypothesis for the
                                             // "unresolved base record" diagnostic bucket in
                                             // BuildFlatEntryList.
};

struct FolderNode
{
    uint32_t               flatEntryIndex {0}; // which FlatEntry (record number) this node is for
    std::vector<uint32_t>  subdirs ;
    std::vector<uint32_t>  files ;
};

static const uint32_t FOLDER_INDEX_SENTINEL = 0xFFFFFFFFU; // "not a folder" marker in folderIndexOf
static const uint32_t ROOT_RECORD_NUMBER = 5;               // NTFS reserves record 5 for the volume root
static const uint32_t SYSTEM_RECORD_COUNT = 16;              // first ~16 records are reserved ($MFT, $MFTMirr, ...)

// Result of Step 3 (tree build). Kept together since all four are
// produced by the same Setup+Pass2 pass and are meaningless apart from
// each other.
struct FolderTree
{
    std::vector<FolderNode> folderNodes;
    std::vector<uint32_t>   folderIndexOf;
    std::vector<uint32_t>   orphanedRecordNumbers;
    std::vector<uint32_t>   systemRecordNumbers;
    std::vector<uint32_t>   staleParentRecordNumbers; // a parent slot resolved, but its ACTUAL sequence
                                                        // number didn't match what this entry's own
                                                        // $FILE_NAME expected - the parent record has been
                                                        // reused since. See BuildFolderTree and the
                                                        // "Live-volume consistency" addendum in
                                                        // mft_reader_datarun_design.md
    uint32_t                rootFolderSlot {FOLDER_INDEX_SENTINEL};
    uint64_t                skippedExtensionRecordCount {0}; // sanity-checked against Step 1's own count
};

