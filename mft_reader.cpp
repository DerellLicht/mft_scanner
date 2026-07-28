// mft_reader.cpp
//
// Template program (Phase 1) for direct NTFS MFT reading, in the style of
// tools like WizTree. Instead of walking the filesystem with
// FindFirstFile/FindNextFile, this opens the raw NTFS volume, parses the
// boot sector to locate the $MFT, and reads the first MFT record ($MFT's
// own record, record #0) to confirm the raw-read pipeline works end to end.
//
// Phase 1 scope only:
//   - Resolve a drive letter from a required command-line argument
//     (usage() runs and the program exits if none is given)
//   - Open \\.\<drive>: for raw read access
//   - Parse the NTFS boot sector (BPB) to get cluster size / MFT location
//   - Read and print the header of MFT record 0
//
// NOT yet implemented (future phases):
//   - Parsing $FILE_NAME attributes (names aren't printed yet)
//   - Following non-resident attribute data runs (file size is reported
//     directly from the non-resident header instead - see $DATA handling
//     in WalkAttributes - but the runs themselves aren't decoded, so
//     locating a file's actual data clusters isn't possible yet)
//   - Walking the full MFT and reconstructing the directory tree
//   - Filtering output to a specific starting subfolder
//
// IMPORTANT: This must be run from an elevated (Administrator) command
// prompt. Raw volume handles are only obtainable with elevated privileges.
// The program checks this itself at startup (see IsProcessElevated) and
// exits with a clear message rather than attempting to self-elevate.
// (Self-elevation via a manifest's requireAdministrator was tried and
// rejected for this template: Windows cannot attach an elevated child
// process to a non-elevated parent's console, so it always opens a new
// console window to do so - there's no clean way around that at the OS
// level for a plain console app. Requiring the user to already be in an
// elevated prompt keeps everything in one console with no popups.)
//
// Build (TDM32, C++11): see accompanying Makefile.

// TOKEN_ELEVATION / TokenElevation require Vista+; declare that target
// before windows.h is included so the type is available.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

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
// Unicode output infrastructure (for Phase 2 onward, once we're printing
// real NTFS $FILE_NAME data - which is always stored as UTF-16LE on
// disk regardless of any UNICODE/_UNICODE macro settings).
//
// NTFS filenames need wchar_t/WriteConsoleW, not printf/wprintf: MinGW's
// older mingw.org-based runtimes (e.g. TDM32) don't correctly implement
// the CRT's wide-stdio text-mode translation (_O_U16TEXT), and the
// console code-page route (chcp 65001) has its own long history of
// truncation/corruption bugs independent of toolchain. WriteConsoleW
// talks directly to the console subsystem in native UTF-16, bypassing
// all of that CRT/code-page machinery.
//
// The catch: WriteConsoleW only works when stdout is an actual console
// object. If output is redirected to a file/pipe, WriteConsoleW returns
// FALSE and silently writes nothing - no error, filenames just vanish
// while everything else (plain printf output) keeps working fine. The
// pair of functions below detects that up front and falls back to
// UTF-8 + WriteFile for the redirected case, which is also the right
// encoding choice for a redirected .txt destination.
//
// Moved above the Phase 3 functions (Step 1/3) since they're the first
// callers of WriteWideLine, for progress-line output - originally this
// block lived just above main(), which is fine when nothing upstream of
// main() calls it, but Phase 3's helper functions now do.
// ---------------------------------------------------------------------

// Call once at startup with GetStdHandle(STD_OUTPUT_HANDLE). Returns
// true if stdout is a live console (WriteConsoleW is safe to use),
// false if it has been redirected to a file/pipe (fall back to UTF-8 +
// WriteFile instead - see WriteWideLine).
static bool StdoutIsConsole(HANDLE hStdOut)
{
    DWORD mode;
    return GetConsoleMode(hStdOut, &mode) != 0;
}

// Writes a wide string to stdout correctly whether it's a live console
// or has been redirected. isConsole should come from a single cached
// StdoutIsConsole() call at startup, not re-checked per line.
static void WriteWideLine(HANDLE hStdOut, bool isConsole, const std::wstring& text)
{
    if (isConsole) {
        DWORD written;
        WriteConsoleW(hStdOut, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    else {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
        std::string utf8(utf8Len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                             &utf8[0], utf8Len, nullptr, nullptr);

        DWORD written;
        WriteFile(hStdOut, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

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
    bool         isDirectory {false};
    bool         inUse {false};
};

struct FolderNode
{
    uint32_t               flatEntryIndex {0}; // which FlatEntry (record number) this node is for
    std::vector<uint32_t>  subdirs {};
    std::vector<uint32_t>  files {};
};

static const uint32_t FOLDER_INDEX_SENTINEL = 0xFFFFFFFFu; // "not a folder" marker in folderIndexOf
static const uint32_t ROOT_RECORD_NUMBER = 5;               // NTFS reserves record 5 for the volume root
static const uint32_t SYSTEM_RECORD_COUNT = 16;              // first ~16 records are reserved ($MFT, $MFTMirr, ...)
constexpr int PROGRESS_INTERVAL = 50000;

// Result of Step 3 (tree build). Kept together since all four are
// produced by the same Setup+Pass2 pass and are meaningless apart from
// each other.
struct FolderTree
{
    std::vector<FolderNode> folderNodes;
    std::vector<uint32_t>   folderIndexOf;
    std::vector<uint32_t>   orphanedRecordNumbers;
    std::vector<uint32_t>   systemRecordNumbers;
    uint32_t                rootFolderSlot {FOLDER_INDEX_SENTINEL};
};

// ---------------------------------------------------------------------
// Human-readable names for the standard NTFS attribute type codes.
// Returns "unknown" for anything not in this table rather than failing -
// third-party/rare attribute types shouldn't stop the walk.
// ---------------------------------------------------------------------
static const char* GetAttributeTypeName(uint32_t type)
{
    switch (type) {
        case 0x10:  return "$STANDARD_INFORMATION";
        case 0x20:  return "$ATTRIBUTE_LIST";
        case 0x30:  return "$FILE_NAME";
        case 0x40:  return "$OBJECT_ID";
        case 0x50:  return "$SECURITY_DESCRIPTOR";
        case 0x60:  return "$VOLUME_NAME";
        case 0x70:  return "$VOLUME_INFORMATION";
        case 0x80:  return "$DATA";
        case 0x90:  return "$INDEX_ROOT";
        case 0xA0:  return "$INDEX_ALLOCATION";
        case 0xB0:  return "$BITMAP";
        case 0xC0:  return "$REPARSE_POINT";
        case 0xD0:  return "$EA_INFORMATION";
        case 0xE0:  return "$EA";
        case 0x100: return "$LOGGED_UTILITY_STREAM";
        default:    return "unknown";
    }
}

// ---------------------------------------------------------------------
// Undoes the NTFS "update sequence" fixup in place on a raw MFT record
// buffer. On disk, the last 2 bytes of every 512-byte sector in the
// record are overwritten with a check value (the "update sequence
// number"), and the true original bytes are saved in a small array
// right after updateSeqOffset. This must run before any attribute
// content is read, or data crossing a sector boundary comes out wrong.
// Returns false if the check value doesn't match what's on disk in any
// sector (signals a torn/corrupt read), but still applies whatever
// fixups it can - callers may choose to proceed anyway for Phase 2
// diagnostics.
// ---------------------------------------------------------------------
static bool ApplyFixup(std::string& recordBuf, const MFT_RECORD_HEADER& header)
{
    const size_t sectorSize = 512;
    uint8_t* buf = reinterpret_cast<uint8_t*>(&recordBuf[0]);

    if (header.updateSeqOffset + 2u > recordBuf.size() || header.updateSeqSize == 0) {
        return false;
    }

    const uint16_t* usa = reinterpret_cast<const uint16_t*>(buf + header.updateSeqOffset);
    uint16_t checkValue = usa[0];
    uint32_t sectorCount = static_cast<uint32_t>(header.updateSeqSize) - 1;

    bool allMatched = true;
    for (uint32_t i = 0; i < sectorCount; ++i) {
        size_t sectorEndOffset = (i + 1) * sectorSize - 2;
        if (sectorEndOffset + 2 > recordBuf.size()) {
            break;
        }

        uint16_t* sectorTail = reinterpret_cast<uint16_t*>(buf + sectorEndOffset);
        if (*sectorTail != checkValue) {
            allMatched = false; // sector's on-disk tail didn't match - torn write or bad offset
        }
        *sectorTail = usa[i + 1]; // restore the real bytes regardless, so content is usable
    }

    return allMatched;
}

// ---------------------------------------------------------------------
// Walks the attribute list of an MFT record starting at
// header.firstAttributeOffset, printing each attribute's type, total
// length, resident/non-resident flag, and attribute id. Stops at the
// 0xFFFFFFFF end marker or if a malformed length would run past the
// end of the record (defensive - a corrupt or misread record should
// not walk off the buffer).
// ---------------------------------------------------------------------
static void WalkAttributes(const std::string& recordBuf, const MFT_RECORD_HEADER& header)
{
    printf("\n-- Attributes --\n");

    size_t offset = header.firstAttributeOffset;
    while (offset + sizeof(ATTR_HEADER_COMMON) <= recordBuf.size()) {
        const ATTR_HEADER_COMMON* attr =
            reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }

        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            printf("  [malformed attribute at offset %zu - length %u, stopping walk]\n",
                offset, attr->length);
            break;
        }

        printf("  type 0x%02X (%-22s) length %4u  %-12s  id %u\n",
            attr->type,
            GetAttributeTypeName(attr->type),
            attr->length,
            attr->nonResident ? "non-resident" : "resident",
            attr->attributeId);

        // $DATA size reporting only - data runs are intentionally not
        // decoded here (out of scope for this milestone; realSize/
        // allocatedSize come straight off the non-resident header, so
        // run decoding isn't needed just to report a file's size).
        // A file can carry more than one $DATA attribute: named
        // alternate data streams (nameLength > 0) show up as additional
        // 0x80 entries alongside the unnamed/default stream, so both
        // are reported here rather than assuming only one exists.
        if (attr->type == 0x80 && offset + attr->length <= recordBuf.size()) {
            const char* streamKind = (attr->nameLength == 0) ? "default" : "named";

            if (!attr->nonResident) {
                // Bounds-checked the same way as the ATTR_HEADER_COMMON
                // cast above: attr->length said this fits, but a
                // corrupt/torn record shouldn't be trusted blindly.
                if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_RESIDENT_HEADER) <= recordBuf.size()) {
                    const ATTR_RESIDENT_HEADER* res = reinterpret_cast<const ATTR_RESIDENT_HEADER*>(
                        recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                    printf("    -> %s stream, resident, size = %u bytes\n", streamKind, res->valueLength);
                }
            }
            else {
                if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                    const ATTR_NONRESIDENT_HEADER* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
                        recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                    printf("    -> %s stream, non-resident, real size = %llu bytes (allocated = %llu)\n",
                        streamKind,
                        static_cast<unsigned long long>(nonRes->realSize),
                        static_cast<unsigned long long>(nonRes->allocatedSize));
                }
            }
        }

        offset += attr->length;
    }
}

// ---------------------------------------------------------------------
// Decodes a single $FILE_NAME attribute's content (the bytes right
// after its ATTR_RESIDENT_HEADER) into a parent record number and a
// std::wstring name. Content is UTF-16LE on disk; copied directly since
// this target is little-endian x86, matching the rest of this file's
// approach to on-disk structures.
// ---------------------------------------------------------------------
static void DecodeFileNameAttribute(const uint8_t* fileNameContent, uint32_t& outParentRecordNumber,
    uint8_t& outNameType, std::wstring& outName)
{
    const ATTR_FILENAME_HEADER* fn = reinterpret_cast<const ATTR_FILENAME_HEADER*>(fileNameContent);

    outParentRecordNumber = static_cast<uint32_t>(fn->parentRecordReference & 0x0000FFFFFFFFFFFFULL);
    outNameType = fn->nameType;

    const wchar_t* nameChars = reinterpret_cast<const wchar_t*>(fileNameContent + sizeof(ATTR_FILENAME_HEADER));
    outName.assign(nameChars, fn->nameLength);
}

// ---------------------------------------------------------------------
// Walks one MFT record's attributes and fills in a FlatEntry for it -
// this is the per-record work behind Step 1's linear pass. Applies the
// name-selection rule from the spec: a Win32/POSIX-namespace
// $FILE_NAME wins over a DOS-namespace one; DOS is only used if it's
// the only $FILE_NAME present. File size comes from the unnamed
// (default) $DATA stream only, matching WalkAttributes' own reporting.
// Returns false if the record has no usable $FILE_NAME at all (should
// not normally happen for an in-use record, but a corrupt/torn record
// is defensively handled rather than crashing the pass).
// ---------------------------------------------------------------------
static bool ExtractRecordInfo(const std::string& recordBuf, const MFT_RECORD_HEADER& header, FlatEntry& outEntry)
{
    outEntry.isDirectory = (header.flags & 0x0002) != 0;
    outEntry.inUse = (header.flags & 0x0001) != 0;

    bool haveName = false;
    bool haveNonDosName = false;

    size_t offset = header.firstAttributeOffset;
    while (offset + sizeof(ATTR_HEADER_COMMON) <= recordBuf.size()) {
        const ATTR_HEADER_COMMON* attr =
            reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }
        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            break; // malformed - stop, same defensive rule as WalkAttributes
        }

        if (attr->type == 0x30 && !attr->nonResident) { // $FILE_NAME, always resident in practice
            if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_RESIDENT_HEADER) <= recordBuf.size()) {
                const ATTR_RESIDENT_HEADER* res = reinterpret_cast<const ATTR_RESIDENT_HEADER*>(
                    recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                // valueOffset is documented as measured from the start of
                // the attribute record itself (i.e. from `offset`), which
                // already accounts for both ATTR_HEADER_COMMON and this
                // resident header - adding sizeof(ATTR_HEADER_COMMON) here
                // double-counts it and lands inside the timestamp fields
                // of ATTR_FILENAME_HEADER instead of at the real name data.
                size_t contentOffset = offset + res->valueOffset;

                if (contentOffset + sizeof(ATTR_FILENAME_HEADER) <= recordBuf.size()) {
                    uint32_t parentRecordNumber = 0;
                    uint8_t  nameType = 0;
                    std::wstring name;
                    DecodeFileNameAttribute(reinterpret_cast<const uint8_t*>(recordBuf.data() + contentOffset),
                        parentRecordNumber, nameType, name);

                    bool isDos = (nameType == FILENAME_NAMESPACE_DOS);
                    if (!haveName || (haveName && !haveNonDosName && !isDos)) {
                        outEntry.parentRecordNumber = parentRecordNumber;
                        outEntry.name = name;
                        haveName = true;
                        haveNonDosName = haveNonDosName || !isDos;
                    }
                }
            }
        }
        else if (attr->type == 0x80 && attr->nameLength == 0) { // $DATA, default/unnamed stream only
            if (!attr->nonResident) {
                if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_RESIDENT_HEADER) <= recordBuf.size()) {
                    const ATTR_RESIDENT_HEADER* res = reinterpret_cast<const ATTR_RESIDENT_HEADER*>(
                        recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                    outEntry.fileSize = res->valueLength;
                }
            }
            else {
                if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                    const ATTR_NONRESIDENT_HEADER* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
                        recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                    outEntry.fileSize = nonRes->realSize;
                }
            }
        }

        offset += attr->length;
    }

    return haveName;
}

// ---------------------------------------------------------------------
// Returns the real size (in bytes) of the $MFT file itself, read from
// record 0's own unnamed $DATA attribute. totalRecordCount = this size
// / mftRecordSize, which is how Step 1 knows how many records to walk
// without needing a separate volume-wide record count from anywhere
// else. Returns 0 if record 0's $DATA attribute can't be found -
// caller should treat that as a fatal error.
// ---------------------------------------------------------------------
static uint64_t GetMftFileSize(const std::string& recordBuf, const MFT_RECORD_HEADER& header)
{
    size_t offset = header.firstAttributeOffset;
    while (offset + sizeof(ATTR_HEADER_COMMON) <= recordBuf.size()) {
        const ATTR_HEADER_COMMON* attr =
            reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }
        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            break;
        }

        if (attr->type == 0x80 && attr->nameLength == 0 && attr->nonResident) {
            if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                const ATTR_NONRESIDENT_HEADER* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
                    recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                return nonRes->realSize;
            }
        }

        offset += attr->length;
    }

    return 0;
}

// ---------------------------------------------------------------------
// $MFT data-run decoding - see mft_reader_datarun_design.md for the
// full design writeup. This is the intermediate validation pass only:
// it decodes record 0's own $DATA run list into a VCN->LCN extent map
// and cross-checks the map against the attribute's own allocatedSize.
// Nothing downstream (ReadMftRecord, Phase 3) is wired up to use this
// map yet - that's deliberately deferred until this step is confirmed
// working against a live volume.
// ---------------------------------------------------------------------

// One contiguous span of VCNs (positions within the attribute's own
// data) mapped to one contiguous span of LCNs (physical cluster
// numbers on the volume), as decoded from a single run in the run list.
struct DataRunExtent
{
    uint64_t startVcn;
    uint64_t clusterCount;
    uint64_t startLcn;
};

// ---------------------------------------------------------------------
// Locates record 0's own unnamed, non-resident $DATA attribute and
// returns its attribute-record bounds (offset/length of the
// ATTR_HEADER_COMMON within recordBuf, needed to bounds-check the run
// list) plus a copy of its ATTR_NONRESIDENT_HEADER. This is the
// attribute whose data runs describe where the $MFT itself physically
// lives on disk. Returns false if it can't be found - record 0 with no
// non-resident $DATA is not expected and should be treated as fatal by
// the caller.
// ---------------------------------------------------------------------
static bool FindMftDataAttribute(const std::string& recordBuf, const MFT_RECORD_HEADER& header,
    size_t& outAttrOffset, uint32_t& outAttrLength, ATTR_NONRESIDENT_HEADER& outNonRes)
{
    size_t offset = header.firstAttributeOffset;
    while (offset + sizeof(ATTR_HEADER_COMMON) <= recordBuf.size()) {
        const ATTR_HEADER_COMMON* attr =
            reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }
        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            break; // malformed - stop, same defensive rule as WalkAttributes
        }

        if (attr->type == 0x80 && attr->nameLength == 0 && attr->nonResident) {
            if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                const ATTR_NONRESIDENT_HEADER* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
                    recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                outAttrOffset = offset;
                outAttrLength = attr->length;
                outNonRes = *nonRes;
                return true;
            }
        }

        offset += attr->length;
    }

    return false;
}

// ---------------------------------------------------------------------
// Decodes a non-resident attribute's data-run list (found at
// attrOffset + nonRes.dataRunOffset - dataRunOffset is documented as
// relative to the start of the attribute record, not to the start of
// ATTR_NONRESIDENT_HEADER) into a vector of DataRunExtent, in on-disk
// (ascending-VCN) order. attrOffset/attrLength bound the attribute
// record within recordBuf and are used to bounds-check every step - a
// malformed run list stops the walk and returns false rather than
// reading past the record buffer, matching this file's existing
// defensive style (see WalkAttributes/ExtractRecordInfo).
//
// Returns false if a sparse run is encountered (offset-byte-count == 0
// in a run's header byte) - sparse runs aren't supported yet (see the
// design note's "Out of scope" section), so this is reported as a
// decode failure rather than silently producing an incomplete map.
// ---------------------------------------------------------------------
static bool DecodeDataRuns(const std::string& recordBuf, size_t attrOffset, uint32_t attrLength,
    const ATTR_NONRESIDENT_HEADER& nonRes, std::vector<DataRunExtent>& outExtents)
{
    outExtents.clear();

    size_t attrEnd = attrOffset + attrLength;
    size_t runListStart = attrOffset + nonRes.dataRunOffset;
    if (runListStart > attrEnd || runListStart >= recordBuf.size()) {
        return false;
    }

    const uint8_t* buf = reinterpret_cast<const uint8_t*>(recordBuf.data());
    size_t offset = runListStart;
    uint64_t runningVcn = 0;
    int64_t  runningLcn = 0;

    while (offset < attrEnd && offset < recordBuf.size()) {
        uint8_t headerByte = buf[offset];
        ++offset;

        if (headerByte == 0x00) {
            break; // end-of-run-list marker
        }

        uint32_t lengthByteCount = headerByte & 0x0F;
        uint32_t offsetByteCount = (headerByte >> 4) & 0x0F;

        if (offset + lengthByteCount > attrEnd || offset + lengthByteCount > recordBuf.size()) {
            return false; // malformed - would read past the attribute/record
        }

        uint64_t clusterCount = 0;
        for (uint32_t i = 0; i < lengthByteCount; ++i) {
            clusterCount |= static_cast<uint64_t>(buf[offset + i]) << (8 * i);
        }
        offset += lengthByteCount;

        if (offsetByteCount == 0) {
            return false; // sparse run - not supported yet, see design note
        }

        if (offset + offsetByteCount > attrEnd || offset + offsetByteCount > recordBuf.size()) {
            return false;
        }

        int64_t lcnDelta = 0;
        for (uint32_t i = 0; i < offsetByteCount; ++i) {
            lcnDelta |= static_cast<int64_t>(buf[offset + i]) << (8 * i);
        }
        // Sign-extend from the top bit of the last (most significant) byte
        // actually present in the on-disk field - offsetByteCount can be
        // less than 8, so the delta isn't naturally sign-extended by the
        // shifts above. Built with unsigned arithmetic and only converted
        // to signed at the end - left-shifting a negative signed value
        // (e.g. ~int64_t(0) << n) is undefined behavior, so that mask is
        // built as unsigned first.
        uint8_t signByte = buf[offset + offsetByteCount - 1];
        if ((signByte & 0x80) != 0 && offsetByteCount < 8) {
            uint64_t signExtendMask = ~static_cast<uint64_t>(0) << (8 * offsetByteCount);
            lcnDelta = static_cast<int64_t>(static_cast<uint64_t>(lcnDelta) | signExtendMask);
        }
        offset += offsetByteCount;

        runningLcn += lcnDelta;
        if (runningLcn < 0) {
            return false; // shouldn't happen for a well-formed $MFT run list
        }

        DataRunExtent extent;
        extent.startVcn = runningVcn;
        extent.clusterCount = clusterCount;
        extent.startLcn = static_cast<uint64_t>(runningLcn);
        outExtents.push_back(extent);

        runningVcn += clusterCount;
    }

    return true;
}

// ---------------------------------------------------------------------
// Resolves an MFT record number to its true physical byte offset on
// the volume, using the VCN->LCN extent map decoded by DecodeDataRuns.
// This replaces the old mftOffsetBytes + recordNumber * mftRecordSize
// linear formula, which only holds if the $MFT is one contiguous run -
// not a safe assumption on a live, fragmented volume (see the design
// note's "Why this is needed" section, and output6.txt, which showed
// this volume's $MFT split across two extents).
//
// extents must be sorted by startVcn - guaranteed by DecodeDataRuns,
// since on-disk run-list order is ascending-VCN by construction.
// Binary search is O(log n) per lookup, cheap next to the disk I/O
// this is guarding.
//
// Assumes mftRecordSize divides evenly into clusterSize (true for the
// reference volume: 1024 into 4096), so a single record can never
// straddle two extents - see the design note's "Records spanning a
// run boundary" section for the not-yet-supported case where that
// doesn't hold; that case isn't detected here.
//
// Returns false (leaving outByteOffset untouched) if the record's VCN
// isn't covered by any known extent - shouldn't happen for a fully
// and correctly decoded map, but checked defensively rather than
// assumed, same posture as the rest of this file.
// ---------------------------------------------------------------------
static bool ResolveRecordOffset(uint64_t recordNumber, uint32_t mftRecordSize, uint32_t clusterSize,
    const std::vector<DataRunExtent>& extents, uint64_t& outByteOffset)
{
    uint64_t byteOffsetInMftFile = recordNumber * mftRecordSize;
    uint64_t vcn = byteOffsetInMftFile / clusterSize;
    uint64_t offsetWithinCluster = byteOffsetInMftFile % clusterSize;

    size_t lo = 0;
    size_t hi = extents.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const DataRunExtent& extent = extents[mid];

        if (vcn < extent.startVcn) {
            hi = mid;
        }
        else if (vcn >= extent.startVcn + extent.clusterCount) {
            lo = mid + 1;
        }
        else {
            outByteOffset = (extent.startLcn + (vcn - extent.startVcn)) * clusterSize + offsetWithinCluster;
            return true;
        }
    }

    return false; // VCN not covered by any decoded extent
}

// ---------------------------------------------------------------------
// Reads and fixes up a single MFT record by record number: resolves
// the record's true physical offset via ResolveRecordOffset (the
// VCN->LCN extent map, not the old linear mftOffsetBytes +
// recordNumber * mftRecordSize formula - see mft_reader_datarun_
// design.md), reads mftRecordSize bytes, and applies the
// update-sequence fixup if the signature is "FILE". The return value
// distinguishes *why* a record wasn't usable, rather than collapsing
// every non-"FILE" case into one bool - this matters diagnostically:
// FreeSlot and Corrupt are both normal, expected states for a record
// slot on a live MFT, but UnexpectedData (a signature that's neither
// "FILE", "BAAD", nor all-zero) now means something has actually gone
// wrong with the extent map or the read itself, rather than "the $MFT
// probably has more than one extent and we haven't accounted for it
// yet" - that ambiguity is what this pass resolves.
// ---------------------------------------------------------------------
enum class MftRecordReadResult
{
    Success,        // "FILE" signature - record read and fixed up, ready to use
    IoFailure,      // seek or read itself failed, or the record's VCN wasn't covered by any extent
    FreeSlot,       // all-zero signature - a genuinely unused MFT slot, normal
    Corrupt,        // "BAAD" signature - NTFS itself flagged this record corrupt, normal
    UnexpectedData  // signature is none of the above - most likely not an MFT record at all
};

static MftRecordReadResult ReadMftRecord(HANDLE hVolume, const std::vector<DataRunExtent>& mftExtents,
    uint32_t clusterSize, uint32_t mftRecordSize, uint64_t recordNumber, std::string& recordBuf)
{
    uint64_t byteOffset = 0;
    if (!ResolveRecordOffset(recordNumber, mftRecordSize, clusterSize, mftExtents, byteOffset)) {
        return MftRecordReadResult::IoFailure; // record's VCN isn't covered by any known extent
    }

    LARGE_INTEGER seekPos;
    seekPos.QuadPart = static_cast<LONGLONG>(byteOffset);
    if (!SetFilePointerEx(hVolume, seekPos, nullptr, FILE_BEGIN)) {
        return MftRecordReadResult::IoFailure;
    }

    recordBuf.assign(mftRecordSize, '\0');
    DWORD bytesRead = 0;
    if (!ReadFile(hVolume, &recordBuf[0], mftRecordSize, &bytesRead, nullptr) || bytesRead != mftRecordSize) {
        return MftRecordReadResult::IoFailure;
    }

    const MFT_RECORD_HEADER* header = reinterpret_cast<const MFT_RECORD_HEADER*>(recordBuf.data());

    if (memcmp(header->signature, "FILE", 4) == 0) {
        ApplyFixup(recordBuf, *header); // torn/mismatched checksum isn't fatal here - see ApplyFixup's own comment
        return MftRecordReadResult::Success;
    }
    if (memcmp(header->signature, "BAAD", 4) == 0) {
        return MftRecordReadResult::Corrupt;
    }

    bool allZero = true;
    for (int i = 0; i < 4; ++i) {
        if (header->signature[i] != 0) {
            allZero = false;
            break;
        }
    }
    return allZero ? MftRecordReadResult::FreeSlot : MftRecordReadResult::UnexpectedData;
}


// ---------------------------------------------------------------------
// Step 1: the linear pass over every MFT record, producing the flat,
// index-addressable FlatEntry vector (index == record number). Records
// that are unused or unreadable are left as default-constructed
// (inUse == false) entries and simply skipped by Step 3's linking pass
// later - see mft_reader_phase3_final.md, "Resolution: unused MFT
// records - no bookkeeping needed". Records that ARE in use but whose
// $FILE_NAME couldn't be resolved (see ExtractRecordInfo) are still
// stored - their isDirectory/inUse flags are trustworthy from the header
// alone, and a directory among them still needs its FolderNode slot so
// its real children don't cascade into false orphans in Step 3.
//
// Prints the first entry decoded, then every PROGRESS_INTERVAL-th one
// after that, as a sanity check that decoding is keeping pace across a
// potentially multi-million-record MFT.
// ---------------------------------------------------------------------
static std::vector<FlatEntry> BuildFlatEntryList(HANDLE hVolume, const std::vector<DataRunExtent>& mftExtents,
    uint32_t clusterSize, uint32_t mftRecordSize, uint64_t totalRecordCount, bool stdoutIsConsole, HANDLE hStdOut)
{
    std::vector<FlatEntry> flatEntries(totalRecordCount);
    std::string recordBuf;
    uint64_t decodedCount = 0;
    uint64_t extensionRecordCount = 0;      // baseFileRecord != 0 - expected, holds overflow attributes only
    uint64_t unresolvedBaseRecordCount = 0; // baseFileRecord == 0 but still no $FILE_NAME - genuinely unexpected
    uint64_t ioFailureCount = 0;
    uint64_t freeSlotCount = 0;
    uint64_t corruptRecordCount = 0;
    uint64_t unexpectedDataCount = 0;
    uint64_t firstUnexpectedDataRecord = UINT64_MAX; // sentinel - see report at the end

    for (uint64_t recordNumber = 0; recordNumber < totalRecordCount; ++recordNumber) {
        MftRecordReadResult readResult =
            ReadMftRecord(hVolume, mftExtents, clusterSize, mftRecordSize, recordNumber, recordBuf);
        if (readResult != MftRecordReadResult::Success) {
            switch (readResult) {
                case MftRecordReadResult::IoFailure:     ++ioFailureCount;     break;
                case MftRecordReadResult::FreeSlot:      ++freeSlotCount;      break;
                case MftRecordReadResult::Corrupt:       ++corruptRecordCount; break;
                case MftRecordReadResult::UnexpectedData:
                    ++unexpectedDataCount;
                    if (firstUnexpectedDataRecord == UINT64_MAX) {
                        firstUnexpectedDataRecord = recordNumber;
                    }
                    break;
                default: break;
            }
            continue; // leave this slot as default (inUse == false)
        }

        const MFT_RECORD_HEADER* header = reinterpret_cast<const MFT_RECORD_HEADER*>(recordBuf.data());
        if (!(header->flags & 0x0001)) {
            continue; // not in use - stale/deleted record, leave as default
        }

        FlatEntry entry;
        bool haveName = ExtractRecordInfo(recordBuf, *header, entry);

        // Store the entry even when no $FILE_NAME was resolved.
        // isDirectory/inUse come straight from the record header inside
        // ExtractRecordInfo, unconditionally, before it ever tries to walk
        // attributes - they're trustworthy independent of whether the name
        // walk succeeded. Discarding the whole entry here (as this used to
        // do) silently erases this record's FolderNode slot when it's a
        // directory, which cascades into every one of its *real* children
        // showing up as orphaned in Step 3, even though nothing is wrong
        // with them individually - only this record's own name lookup
        // failed. The record itself still ends up correctly orphaned in
        // Step 3 (its own parentRecordNumber is unresolved too), but its
        // children can now find it.
        flatEntries[recordNumber] = entry;

        if (!haveName) {
            // baseFileRecord != 0 marks this as an *extension* record -
            // overflow storage for another record's attributes (most often
            // extra $DATA data-runs for a heavily fragmented file), reached
            // via that base record's own $ATTRIBUTE_LIST. Extension records
            // never carry their own $FILE_NAME by design - only the base
            // record does - so this is expected, not a parsing failure, and
            // is very plausibly the majority of this bucket on a volume with
            // large fragmented files (ISOs, VM images, media). A record with
            // baseFileRecord == 0 IS its own base record and genuinely
            // should have had a $FILE_NAME somewhere in its own attribute
            // list - that bucket is the one actually worth investigating
            // further if it turns out large.
            if ((header->baseFileRecord & 0x0000FFFFFFFFFFFFULL) != 0) {
                ++extensionRecordCount;
            }
            else {
                ++unresolvedBaseRecordCount;
            }
            continue; // nothing to show in the progress line
        }

        ++decodedCount;

        if (decodedCount == 1 || decodedCount % PROGRESS_INTERVAL == 0) {
            // Built with plain concatenation rather than swprintf/wprintf -
            // see WriteWideLine's own comment on wide-stdio formatting
            // being unreliable across MinGW runtime flavors; string
            // concatenation sidesteps that class of bug entirely.
            std::wstring line = L"  [" + std::to_wstring(decodedCount) + L"] " +
                flatEntries[recordNumber].name + L"\n";
            WriteWideLine(hStdOut, stdoutIsConsole, line);
        }
    }

    if (extensionRecordCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(extensionRecordCount) +
            L" extension record(s) - overflow attribute storage, no $FILE_NAME expected)\n";
        WriteWideLine(hStdOut, stdoutIsConsole, line);
    }
    if (unresolvedBaseRecordCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(unresolvedBaseRecordCount) +
            L" base record(s) in use with NO $FILE_NAME found - unexpected, worth a closer look)\n";
        WriteWideLine(hStdOut, stdoutIsConsole, line);
    }
    if (ioFailureCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(ioFailureCount) + L" record(s) - seek/read I/O failure)\n";
        WriteWideLine(hStdOut, stdoutIsConsole, line);
    }
    if (freeSlotCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(freeSlotCount) +
            L" record(s) - free/never-used MFT slot (all-zero signature), normal)\n";
        WriteWideLine(hStdOut, stdoutIsConsole, line);
    }
    if (corruptRecordCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(corruptRecordCount) +
            L" record(s) - NTFS self-flagged corrupt (\"BAAD\" signature), normal)\n";
        WriteWideLine(hStdOut, stdoutIsConsole, line);
    }
    if (unexpectedDataCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(unexpectedDataCount) +
            L" record(s) - UNRECOGNIZED signature (not \"FILE\"/\"BAAD\"/zero); first at record " +
            std::to_wstring(firstUnexpectedDataRecord) +
            L" - likely reading past the $MFT's first extent; data-run decoding not yet implemented)\n";
        WriteWideLine(hStdOut, stdoutIsConsole, line);
    }

    return flatEntries;
}

// ---------------------------------------------------------------------
// Step 3: builds the folder tree from the flat entry list using Option
// B storage (see mft_reader_phase3_final.md). Two stages, as specified:
//
//   Setup - count folders, allocate folderNodes at folder-count size
//           and folderIndexOf at record-count size (sentinel-filled),
//           then populate both together in one pass.
//   Pass 2 - walk the flat list once more; for each populated entry,
//            resolve its parent via folderIndexOf and push the entry's
//            own record number onto the parent's subdirs or files
//            vector. Entries whose parent doesn't resolve are routed
//            to systemRecordNumbers (record numbers 0-15) or
//            orphanedRecordNumbers (everything else) instead - see the
//            spec's "Orphaned entries" section for why.
//
// Root handling: record ROOT_RECORD_NUMBER (5) is its own parent on
// disk, so it's recognized here and never attached under any parent -
// its FolderNode slot is still created normally, just never linked in.
// ---------------------------------------------------------------------
static FolderTree BuildFolderTree(const std::vector<FlatEntry>& flatEntries, bool stdoutIsConsole, HANDLE hStdOut)
{
    FolderTree tree;
    const uint32_t totalRecordCount = static_cast<uint32_t>(flatEntries.size());

    // --- Setup: count folders, then allocate + populate folderNodes/folderIndexOf together ---
    uint32_t totalFolderCount = 0;
    for (uint32_t i = 0; i < totalRecordCount; ++i) {
        if (flatEntries[i].inUse && flatEntries[i].isDirectory) {
            ++totalFolderCount;
        }
    }

    tree.folderNodes.resize(totalFolderCount);
    tree.folderIndexOf.assign(totalRecordCount, FOLDER_INDEX_SENTINEL);

    uint32_t nextFolderSlot = 0;
    for (uint32_t i = 0; i < totalRecordCount; ++i) {
        if (flatEntries[i].inUse && flatEntries[i].isDirectory) {
            tree.folderNodes[nextFolderSlot].flatEntryIndex = i;
            tree.folderIndexOf[i] = nextFolderSlot;
            if (i == ROOT_RECORD_NUMBER) {
                tree.rootFolderSlot = nextFolderSlot;
            }
            ++nextFolderSlot;
        }
    }

    // --- Pass 2: link every populated entry (except the root) to its parent ---
    uint64_t linkedCount = 0;
    for (uint32_t i = 0; i < totalRecordCount; ++i) {
        if (!flatEntries[i].inUse) {
            continue;
        }
        if (i == ROOT_RECORD_NUMBER && flatEntries[i].parentRecordNumber == ROOT_RECORD_NUMBER) {
            continue; // root is its own parent on disk - never attached anywhere
        }

        uint32_t parentSlot = tree.folderIndexOf[flatEntries[i].parentRecordNumber];
        if (parentSlot == FOLDER_INDEX_SENTINEL) {
            if (i < SYSTEM_RECORD_COUNT) {
                tree.systemRecordNumbers.push_back(i);
            }
            else {
                tree.orphanedRecordNumbers.push_back(i);
            }
            continue;
        }

        if (flatEntries[i].isDirectory) {
            tree.folderNodes[parentSlot].subdirs.push_back(i);
        }
        else {
            tree.folderNodes[parentSlot].files.push_back(i);
        }

        ++linkedCount;
        if (linkedCount == 1 || linkedCount % PROGRESS_INTERVAL == 0) {
            std::wstring line = L"  [link " + std::to_wstring(linkedCount) + L"] " +
                flatEntries[i].name + L"\n";
            WriteWideLine(hStdOut, stdoutIsConsole, line);
        }
    }

    return tree;
}

// ---------------------------------------------------------------------
// Prints command-line usage/help text. Called when no argument is
// given at all, or when the given argument isn't a recognizable drive
// spec - see ResolveDriveLetter.
// ---------------------------------------------------------------------
static void PrintUsage(const char* programName)
{
    printf("Usage: %s <drive>:\n", programName);
    printf("\n");
    printf("Reads the NTFS Master File Table (MFT) directly from the given volume,\n");
    printf("bypassing the normal Win32 file enumeration APIs (FindFirstFile/\n");
    printf("FindNextFile), in the style of tools like WizTree.\n");
    printf("\n");
    printf("  <drive>:   Drive letter of the NTFS volume to scan, e.g. D:\n");
    printf("\n");
    printf("This program must be run from an elevated (Administrator) command\n");
    printf("prompt - raw volume access requires elevated privileges.\n");
}

// ---------------------------------------------------------------------
// Resolves a drive letter (e.g. 'C') from the required command-line
// argument. Only handles the simple "X:\..." / "X:" forms, which is
// sufficient for Phase 1. A drive argument is mandatory - there is no
// current-working-directory fallback (that was Phase 1's original
// behavior; removed so that running with no arguments unambiguously
// means "show usage", not "guess a drive"). Returns '\0' if argv[1]
// isn't in a recognizable "X:" form - the caller treats that as a
// usage error, same as no argument at all.
// ---------------------------------------------------------------------
static char ResolveDriveLetter(int argc, char* argv[])
{
    if (argc > 1 && argv[1][0] != '\0' && argv[1][1] == ':') {
        return static_cast<char>(toupper(static_cast<unsigned char>(argv[1][0])));
    }

    return '\0'; // missing or malformed - caller shows usage
}

// ---------------------------------------------------------------------
// Opens a raw handle to the volume containing the given drive letter.
// Requires the process to be running elevated.
// ---------------------------------------------------------------------
static HANDLE OpenVolumeRaw(char driveLetter)
{
    char volumePath[16];
    sprintf(volumePath, "\\\\.\\%c:", driveLetter);

    HANDLE h = CreateFileA(
        volumePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    return h; // caller checks INVALID_HANDLE_VALUE
}

// ---------------------------------------------------------------------
// Computes the on-disk size in bytes of one MFT record, per the signed
// clustersPerMftRecord encoding described in the NTFS boot sector spec.
// ---------------------------------------------------------------------
static uint32_t ComputeMftRecordSize(const NTFS_BOOT_SECTOR& boot)
{
    uint32_t clusterSize = static_cast<uint32_t>(boot.bytesPerSector) * boot.sectorsPerCluster;

    if (boot.clustersPerMftRecord >= 0) {
        return clusterSize * static_cast<uint32_t>(boot.clustersPerMftRecord);
    }
    else {
        // Negative value means "record size = 2^|value| bytes", independent of cluster size.
        return 1u << static_cast<uint32_t>(-boot.clustersPerMftRecord);
    }
}

// ---------------------------------------------------------------------
// Checks whether the current process is running with an elevated
// (Administrator) token. Used at startup so we can fail with a clear
// message in our own console instead of letting the raw volume open
// fail deeper in the program, or relying on a manifest to auto-elevate
// (which would open a separate console - see note at top of file).
// ---------------------------------------------------------------------
static bool IsProcessElevated()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = (elevation.TokenIsElevated != 0);
    }

    CloseHandle(hToken);
    return elevated;
}

// ---------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    char driveLetter = ResolveDriveLetter(argc, argv);
    if (driveLetter == '\0') {
        printf("Invalid drive argument: '%s'\n\n", argv[1]);
        PrintUsage(argv[0]);
        return 1;
    }

    if (!IsProcessElevated()) {
        printf("This program requires Administrator privileges to read raw NTFS volumes.\n");
        printf("Please re-run from an elevated (Administrator) Command Prompt.\n");
        return 1;
    }

    printf("Target volume: %c:\n", driveLetter);

    HANDLE hVolume = OpenVolumeRaw(driveLetter);
    if (hVolume == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        printf("Failed to open volume (error %lu).\n", static_cast<unsigned long>(err));
        printf("This program must be run as Administrator.\n");
        return 1;
    }

    // Read the boot sector (always the first 512 bytes of the volume).
    NTFS_BOOT_SECTOR boot;
    DWORD bytesRead = 0;
    if (!ReadFile(hVolume, &boot, sizeof(boot), &bytesRead, nullptr) || bytesRead != sizeof(boot)) {
        printf("Failed to read boot sector.\n");
        CloseHandle(hVolume);
        return 1;
    }

    if (memcmp(boot.oemId, "NTFS    ", 8) != 0) {
        printf("Volume %c: does not appear to be NTFS.\n", driveLetter);
        CloseHandle(hVolume);
        return 1;
    }

    uint32_t clusterSize = static_cast<uint32_t>(boot.bytesPerSector) * boot.sectorsPerCluster;
    uint32_t mftRecordSize = ComputeMftRecordSize(boot);
    uint64_t mftOffsetBytes = boot.mftStartCluster * clusterSize;

    printf("\n-- NTFS boot sector --\n");
    printf("Bytes per sector:      %u\n", boot.bytesPerSector);
    printf("Sectors per cluster:   %u\n", boot.sectorsPerCluster);
    printf("Cluster size:          %u bytes\n", clusterSize);
    printf("Total sectors:         %llu\n", static_cast<unsigned long long>(boot.totalSectors));
    printf("MFT start cluster:     %llu\n", static_cast<unsigned long long>(boot.mftStartCluster));
    printf("MFT byte offset:       %llu\n", static_cast<unsigned long long>(mftOffsetBytes));
    printf("MFT record size:       %u bytes\n", mftRecordSize);
    printf("Volume serial number:  0x%llX\n", static_cast<unsigned long long>(boot.volumeSerialNumber));

    // Seek to the start of the MFT and read its very first record (record 0,
    // which describes the $MFT file itself).
    LARGE_INTEGER seekPos;
    seekPos.QuadPart = static_cast<LONGLONG>(mftOffsetBytes);
    if (!SetFilePointerEx(hVolume, seekPos, nullptr, FILE_BEGIN)) {
        printf("Failed to seek to MFT.\n");
        CloseHandle(hVolume);
        return 1;
    }

    std::string recordBuf(mftRecordSize, '\0');
    if (!ReadFile(hVolume, &recordBuf[0], mftRecordSize, &bytesRead, nullptr) || bytesRead != mftRecordSize) {
        printf("Failed to read MFT record 0.\n");
        CloseHandle(hVolume);
        return 1;
    }

    const MFT_RECORD_HEADER* header = reinterpret_cast<const MFT_RECORD_HEADER*>(recordBuf.data());

    printf("\n-- MFT record 0 header ($MFT itself) --\n");
    printf("Signature:             %.4s\n", header->signature);
    printf("Sequence number:       %u\n", header->sequenceNumber);
    printf("Hard link count:       %u\n", header->hardLinkCount);
    printf("First attribute offset:%u\n", header->firstAttributeOffset);
    printf("Flags:                 0x%04X (%s)\n",
        header->flags,
        (header->flags & 0x0001) ? "in use" : "not in use");
    printf("Real size:             %u bytes\n", header->realSize);
    printf("Allocated size:        %u bytes\n", header->allocatedSize);

    if (memcmp(header->signature, "FILE", 4) != 0) {
        printf("\nUnexpected signature - record may be corrupt or offsets are wrong.\n");
        CloseHandle(hVolume);
        return 1;
    }

    // Undo the update-sequence fixup before reading any attribute content -
    // see ApplyFixup() comment. Re-point `header` at the now-corrected
    // buffer (ApplyFixup only touches sector-tail bytes, not the header
    // fields themselves, but recordBuf's storage didn't move, so the
    // existing `header` pointer stays valid either way).
    if (!ApplyFixup(recordBuf, *header)) {
        printf("\nWarning: update sequence check value mismatch - record may be corrupt.\n");
    }

    WalkAttributes(recordBuf, *header);

    printf("\nPhase 1 OK: raw MFT read pipeline confirmed working.\n");

    // --- Data-run decode validation (intermediate step) ---
    // See mft_reader_datarun_design.md. Decodes record 0's own $DATA
    // attribute's run list into a VCN->LCN extent map, and cross-checks
    // the map's total covered size against the attribute's own
    // allocatedSize as a sanity check that the run list decoded
    // completely and correctly. Nothing reads through this map yet -
    // ReadMftRecord and Phase 3 still use the old linear-offset formula
    // and haven't been touched. Once this step is confirmed against a
    // live volume, the next pass wires ReadMftRecord up to resolve
    // through the extent map instead.
    size_t dataAttrOffset = 0;
    uint32_t dataAttrLength = 0;
    ATTR_NONRESIDENT_HEADER mftDataHeader;
    if (!FindMftDataAttribute(recordBuf, *header, dataAttrOffset, dataAttrLength, mftDataHeader)) {
        printf("\nCould not locate $MFT's own non-resident $DATA attribute - cannot decode data runs.\n");
        CloseHandle(hVolume);
        return 1;
    }

    std::vector<DataRunExtent> mftExtents;
    bool runsDecodedOk = DecodeDataRuns(recordBuf, dataAttrOffset, dataAttrLength, mftDataHeader, mftExtents);

    printf("\n-- $MFT data-run decode (validation only) --\n");
    if (!runsDecodedOk) {
        printf("Data-run decode FAILED (malformed run list or unsupported sparse run).\n");
        CloseHandle(hVolume);
        return 1;
    }

    uint64_t totalCoveredClusters = 0;
    for (const DataRunExtent& extent : mftExtents) {
        totalCoveredClusters += extent.clusterCount;
    }
    uint64_t coveredBytes = totalCoveredClusters * clusterSize;
    bool sizesMatch = (coveredBytes == mftDataHeader.allocatedSize);

    printf("Extents decoded:        %zu\n", mftExtents.size());
    printf("Total covered clusters: %llu\n", static_cast<unsigned long long>(totalCoveredClusters));
    printf("Covered bytes:          %llu\n", static_cast<unsigned long long>(coveredBytes));
    printf("$DATA allocatedSize:    %llu\n", static_cast<unsigned long long>(mftDataHeader.allocatedSize));
    printf("Match:                   %s\n", sizesMatch ? "YES" : "NO - MISMATCH");

    size_t showCount = mftExtents.size() < 10 ? mftExtents.size() : 10;
    printf("\nFirst %zu extent(s):\n", showCount);
    for (size_t i = 0; i < showCount; ++i) {
        printf("  [%zu] VCN %llu, %llu cluster(s), LCN %llu\n", i,
            static_cast<unsigned long long>(mftExtents[i].startVcn),
            static_cast<unsigned long long>(mftExtents[i].clusterCount),
            static_cast<unsigned long long>(mftExtents[i].startLcn));
    }
    if (mftExtents.size() > showCount) {
        const DataRunExtent& last = mftExtents.back();
        printf("  ... (%zu more) ...\n", mftExtents.size() - showCount - 1);
        printf("  [last] VCN %llu, %llu cluster(s), LCN %llu\n",
            static_cast<unsigned long long>(last.startVcn),
            static_cast<unsigned long long>(last.clusterCount),
            static_cast<unsigned long long>(last.startLcn));
    }

    printf("\nData-run decode validation %s.\n", sizesMatch ? "PASSED" : "FAILED");
    if (!sizesMatch) {
        printf("Refusing to proceed to Phase 3 with an unverified extent map.\n");
        CloseHandle(hVolume);
        return 1;
    }

    // --- Phase 3: full-MFT flat pass, then folder-tree build ---
    uint64_t mftFileSize = GetMftFileSize(recordBuf, *header);
    if (mftFileSize == 0) {
        printf("\nCould not determine $MFT file size from record 0 - skipping Phase 3.\n");
        CloseHandle(hVolume);
        return 1;
    }
    uint64_t totalRecordCount = mftFileSize / mftRecordSize;

    bool stdoutIsConsole = StdoutIsConsole(GetStdHandle(STD_OUTPUT_HANDLE));
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

    printf("\n-- Phase 3: Step 1 - flat entry list --\n");
    printf("Total MFT records to scan: %llu\n", static_cast<unsigned long long>(totalRecordCount));
    std::vector<FlatEntry> flatEntries =
        BuildFlatEntryList(hVolume, mftExtents, clusterSize, mftRecordSize, totalRecordCount, stdoutIsConsole, hStdOut);

    CloseHandle(hVolume); // done with the volume - everything else works from flatEntries in memory

    uint64_t decodedTotal = 0;
    for (const FlatEntry& entry : flatEntries) {
        if (entry.inUse) {
            ++decodedTotal;
        }
    }
    printf("Decoded %llu in-use entries out of %llu records scanned.\n",
        static_cast<unsigned long long>(decodedTotal), static_cast<unsigned long long>(totalRecordCount));

    printf("\n-- Phase 3: Step 3 - folder tree build --\n");
    FolderTree tree = BuildFolderTree(flatEntries, stdoutIsConsole, hStdOut);

    printf("Folders found:        %zu\n", tree.folderNodes.size());
    printf("System records:       %zu\n", tree.systemRecordNumbers.size());
    printf("Orphaned entries:     %zu\n", tree.orphanedRecordNumbers.size());
    if (tree.rootFolderSlot != FOLDER_INDEX_SENTINEL) {
        const FolderNode& root = tree.folderNodes[tree.rootFolderSlot];
        printf("Root: %zu direct subfolders, %zu direct files\n", root.subdirs.size(), root.files.size());
    }
    else {
        printf("Warning: root record (%u) was not found as a folder.\n", ROOT_RECORD_NUMBER);
    }

    printf("\nPhase 3 OK: flat entry list and folder tree built.\n");
    return 0;
}
