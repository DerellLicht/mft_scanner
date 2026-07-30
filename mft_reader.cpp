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
#define _WIN32_WINNT 0x0600   //  NOLINT(bugprone-reserved-identifier)
#endif

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

static bool stdoutIsConsole = true; // default = console
static HANDLE hStdOut = INVALID_HANDLE_VALUE ;

constexpr int PROGRESS_INTERVAL = 50000;

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
    std::vector<uint32_t>  subdirs {};
    std::vector<uint32_t>  files {};
};

static const uint32_t FOLDER_INDEX_SENTINEL = 0xFFFFFFFFu; // "not a folder" marker in folderIndexOf
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
static bool IsStdoutConsole(HANDLE hStdOutHdl)
{
    DWORD mode {};
    return GetConsoleMode(hStdOutHdl, &mode) != 0;
}

// Writes a wide string to stdout correctly whether it's a live console
// or has been redirected. isConsole should come from a single cached
// StdoutIsConsole() call at startup, not re-checked per line.
static void WriteWideLine(const std::wstring& text)
{
    DWORD written {};
    if (stdoutIsConsole) {
        WriteConsoleW(hStdOut, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    else {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
        std::string utf8(utf8Len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                             &utf8[0], utf8Len, nullptr, nullptr);

        WriteFile(hStdOut, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

//********************************************************************
int dputsf(const wchar_t *fmt, ...)
{
   wchar_t consoleBuffer[3000] ;
   va_list al {};

   va_start(al, fmt);
   _vswprintf(consoleBuffer, fmt, al);   //lint !e64
   // OutputDebugString(consoleBuffer) ;
   std::wstring wwl = consoleBuffer ;
   WriteWideLine(wwl) ;
   va_end(al);
   return 1;
}

// ---------------------------------------------------------------------
// Human-readable names for the standard NTFS attribute type codes.
// Returns "unknown" for anything not in this table rather than failing -
// third-party/rare attribute types shouldn't stop the walk.
// ---------------------------------------------------------------------
static const wchar_t* GetAttributeTypeName(uint32_t type)
{
    switch (type) {
        case 0x10:  return L"$STANDARD_INFORMATION";
        case 0x20:  return L"$ATTRIBUTE_LIST";
        case 0x30:  return L"$FILE_NAME";
        case 0x40:  return L"$OBJECT_ID";
        case 0x50:  return L"$SECURITY_DESCRIPTOR";
        case 0x60:  return L"$VOLUME_NAME";
        case 0x70:  return L"$VOLUME_INFORMATION";
        case 0x80:  return L"$DATA";
        case 0x90:  return L"$INDEX_ROOT";
        case 0xA0:  return L"$INDEX_ALLOCATION";
        case 0xB0:  return L"$BITMAP";
        case 0xC0:  return L"$REPARSE_POINT";
        case 0xD0:  return L"$EA_INFORMATION";
        case 0xE0:  return L"$EA";
        case 0x100: return L"$LOGGED_UTILITY_STREAM";
        default:    return L"unknown";
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
    auto* buf = reinterpret_cast<uint8_t*>(&recordBuf[0]);

    if (header.updateSeqOffset + 2u > recordBuf.size() || header.updateSeqSize == 0) {
        return false;
    }

    const auto* usa = reinterpret_cast<const uint16_t*>(buf + header.updateSeqOffset);
    uint16_t checkValue = usa[0];
    uint32_t sectorCount = static_cast<uint32_t>(header.updateSeqSize) - 1;

    bool allMatched = true;
    for (uint32_t i = 0; i < sectorCount; ++i) {
        size_t sectorEndOffset = (i + 1) * sectorSize - 2;
        if (sectorEndOffset + 2 > recordBuf.size()) {
            break;
        }

        auto* sectorTail = reinterpret_cast<uint16_t*>(buf + sectorEndOffset);
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
    dputsf(L"\n-- Attributes --\n");

    size_t offset = header.firstAttributeOffset;
    while (offset + sizeof(ATTR_HEADER_COMMON) <= recordBuf.size()) {
        const auto* attr = reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }

        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            dputsf(L"  [malformed attribute at offset %zu - length %u, stopping walk]\n",
                offset, attr->length);
            break;
        }

        dputsf(L"  type 0x%02X (%-22s) length %4u  %-12s  id %u\n",
            attr->type,
            GetAttributeTypeName(attr->type),
            attr->length,
            attr->nonResident ? L"non-resident" : L"resident",
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
            const wchar_t* streamKind = (attr->nameLength == 0) ? L"default" : L"named" ;

            if (!attr->nonResident) {
                // Bounds-checked the same way as the ATTR_HEADER_COMMON
                // cast above: attr->length said this fits, but a
                // corrupt/torn record shouldn't be trusted blindly.
                if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_RESIDENT_HEADER) <= recordBuf.size()) {
                    const auto* res = reinterpret_cast<const ATTR_RESIDENT_HEADER*>(
                        recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                    dputsf(L"    -> %s stream, resident, size = %u bytes\n", streamKind, res->valueLength);
                }
            }
            else {
                if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                    const auto* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
                        recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                    dputsf(L"    -> %s stream, non-resident, real size = %llu bytes (allocated = %llu)\n",
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
// after its ATTR_RESIDENT_HEADER) into a parent record number, the
// parent's *expected* sequence number, and a std::wstring name.
// parentRecordReference packs both: low 48 bits are the parent's
// record number, high 16 bits are the sequence number the parent
// record was expected to have at the time this $FILE_NAME was
// written. That sequence number matters for stale-reference detection
// - see the "Live-volume consistency" addendum in
// mft_reader_datarun_design.md and BuildFolderTree's use of it.
// Content is UTF-16LE on disk; copied directly since this target is
// little-endian x86, matching the rest of this file's approach to
// on-disk structures.
// ---------------------------------------------------------------------
static void DecodeFileNameAttribute(const uint8_t* fileNameContent, uint32_t& outParentRecordNumber,
    uint16_t& outParentSequenceNumber, uint8_t& outNameType, std::wstring& outName)
{
    const auto* fn = reinterpret_cast<const ATTR_FILENAME_HEADER*>(fileNameContent);

    outParentRecordNumber = static_cast<uint32_t>(fn->parentRecordReference & 0x0000FFFFFFFFFFFFULL);
    outParentSequenceNumber = static_cast<uint16_t>((fn->parentRecordReference >> 48) & 0xFFFFULL);
    outNameType = fn->nameType;

    const auto* nameChars = reinterpret_cast<const wchar_t*>(fileNameContent + sizeof(ATTR_FILENAME_HEADER));
    outName.assign(nameChars, fn->nameLength);
}

// ---------------------------------------------------------------------
// Walks one MFT record's attributes and fills in a FlatEntry for it -
// this is the per-record work behind Step 1's linear pass. Applies the
// name-selection rule from the spec: a Win32/POSIX-namespace
// $FILE_NAME wins over a DOS-namespace one; DOS is only used if it's
// the only $FILE_NAME present. File size comes from the unnamed
// (default) $DATA stream only, matching WalkAttributes' own reporting.
// Also records whether an $ATTRIBUTE_LIST attribute is present (see
// FlatEntry::hasAttributeList) - this walk only ever looks at the
// record's own attribute list, never follows $ATTRIBUTE_LIST to check
// extension records for a relocated $FILE_NAME, so its presence
// alongside a failed name lookup is the leading hypothesis for that
// failure (see the "unresolved base record" diagnostic in
// BuildFlatEntryList).
// Returns false if the record has no usable $FILE_NAME at all (should
// not normally happen for an in-use record, but a corrupt/torn record
// is defensively handled rather than crashing the pass).
// ---------------------------------------------------------------------
static bool ExtractRecordInfo(const std::string& recordBuf, const MFT_RECORD_HEADER& header, FlatEntry& outEntry)
{
    outEntry.isDirectory = (header.flags & 0x0002) != 0;
    outEntry.inUse = (header.flags & 0x0001) != 0;
    // Captured unconditionally, straight from the header, same as
    // isDirectory/inUse above - any in-use record can be referenced as
    // someone else's parent, regardless of whether its own $FILE_NAME
    // resolves, so this needs to be available even when haveName ends
    // up false. See BuildFolderTree's stale-parent-reference check.
    outEntry.sequenceNumber = header.sequenceNumber;

    bool haveName = false;
    bool haveNonDosName = false;

    size_t offset = header.firstAttributeOffset;
    while (offset + sizeof(ATTR_HEADER_COMMON) <= recordBuf.size()) {
        const auto* attr = reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }
        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            break; // malformed - stop, same defensive rule as WalkAttributes
        }

        if (attr->type == 0x20) { // $ATTRIBUTE_LIST - see FlatEntry::hasAttributeList
            outEntry.hasAttributeList = true;
        }
        else if (attr->type == 0x30 && !attr->nonResident) { // $FILE_NAME, always resident in practice
            if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_RESIDENT_HEADER) <= recordBuf.size()) {
                const auto* res = reinterpret_cast<const ATTR_RESIDENT_HEADER*>(
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
                    uint16_t parentSequenceNumber = 0;
                    uint8_t  nameType = 0;
                    std::wstring name;
                    DecodeFileNameAttribute(reinterpret_cast<const uint8_t*>(recordBuf.data() + contentOffset),
                        parentRecordNumber, parentSequenceNumber, nameType, name);

                    bool isDos = (nameType == FILENAME_NAMESPACE_DOS);
                    if (!haveName || (haveName && !haveNonDosName && !isDos)) {
                        outEntry.parentRecordNumber = parentRecordNumber;
                        outEntry.parentSequenceNumber = parentSequenceNumber;
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
                    const auto* res = reinterpret_cast<const ATTR_RESIDENT_HEADER*>(
                        recordBuf.data() + offset + sizeof(ATTR_HEADER_COMMON));
                    outEntry.fileSize = res->valueLength;
                }
            }
            else {
                if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                    const auto* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
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
        const auto* attr =
            reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }
        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            break;
        }

        if (attr->type == 0x80 && attr->nameLength == 0 && attr->nonResident) {
            if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                const auto* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
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
        const auto* attr = reinterpret_cast<const ATTR_HEADER_COMMON*>(recordBuf.data() + offset);

        if (attr->type == ATTR_TYPE_END) {
            break;
        }
        if (attr->length < sizeof(ATTR_HEADER_COMMON) || offset + attr->length > recordBuf.size()) {
            break; // malformed - stop, same defensive rule as WalkAttributes
        }

        if (attr->type == 0x80 && attr->nameLength == 0 && attr->nonResident) {
            if (offset + sizeof(ATTR_HEADER_COMMON) + sizeof(ATTR_NONRESIDENT_HEADER) <= recordBuf.size()) {
                const auto* nonRes = reinterpret_cast<const ATTR_NONRESIDENT_HEADER*>(
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

    const auto* buf = reinterpret_cast<const uint8_t*>(recordBuf.data());
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

        DataRunExtent extent {};
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
//
// outFixupOk reports ApplyFixup's own result - true if every sector's
// update-sequence check value matched (a clean, untorn read), false if
// any didn't. Only meaningful when this function returns Success (the
// only case ApplyFixup runs at all); callers should not read it
// otherwise. A mismatch isn't treated as fatal here - see ApplyFixup's
// own comment - but the caller can now use it as a live-volume-churn
// signal instead of the mismatch being silently discarded. See the
// "Live-volume consistency" addendum in mft_reader_datarun_design.md.
// ---------------------------------------------------------------------
enum class MftRecordReadResult   // NOLINT(performance-enum-size)
{
    Success,        // "FILE" signature - record read and fixed up, ready to use
    IoFailure,      // seek or read itself failed, or the record's VCN wasn't covered by any extent
    FreeSlot,       // all-zero signature - a genuinely unused MFT slot, normal
    Corrupt,        // "BAAD" signature - NTFS itself flagged this record corrupt, normal
    UnexpectedData  // signature is none of the above - most likely not an MFT record at all
};

static MftRecordReadResult ReadMftRecord(HANDLE hVolume, const std::vector<DataRunExtent>& mftExtents,
    uint32_t clusterSize, uint32_t mftRecordSize, uint64_t recordNumber, std::string& recordBuf, bool& outFixupOk)
{
    outFixupOk = true; // meaningful only when this function returns Success - see comment above

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

    const auto* header = reinterpret_cast<const MFT_RECORD_HEADER*>(recordBuf.data());

    if (memcmp(header->signature, "FILE", 4) == 0) {
        outFixupOk = ApplyFixup(recordBuf, *header); // captured, not discarded - see comment above
        return MftRecordReadResult::Success;
    }
    if (memcmp(header->signature, "BAAD", 4) == 0) {
        return MftRecordReadResult::Corrupt;
    }

    bool allZero = true;
    for (int i = 0; i < 4; ++i) {   // NOLINT(modernize-loop-convert)
        if (header->signature[i] != 0) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
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
    uint32_t clusterSize, uint32_t mftRecordSize, uint64_t totalRecordCount,
    std::vector<uint32_t>& outUnresolvedBaseRecordNumbers)
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
    uint64_t fixupMismatchCount = 0;                 // any in-use record whose update-sequence check failed -
                                                      // see the "Live-volume consistency" addendum in
                                                      // mft_reader_datarun_design.md
    uint64_t unresolvedBaseRecordFixupMismatchCount = 0; // subset of the above that also had no $FILE_NAME -
                                                          // a high overlap here is the live-volume-churn signal
    uint64_t unresolvedBaseRecordAttributeListCount = 0; // subset of unresolvedBaseRecordCount that also had
                                                          // an $ATTRIBUTE_LIST attribute - a high overlap here
                                                          // supports the relocated-$FILE_NAME/hard-link
                                                          // hypothesis instead - see FlatEntry::hasAttributeList

    for (uint64_t recordNumber = 0; recordNumber < totalRecordCount; ++recordNumber) {
        bool fixupOk = true;
        MftRecordReadResult readResult =
            ReadMftRecord(hVolume, mftExtents, clusterSize, mftRecordSize, recordNumber, recordBuf, fixupOk);
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

        const auto* header = reinterpret_cast<const MFT_RECORD_HEADER*>(recordBuf.data());
        if (!(header->flags & 0x0001)) {
            continue; // not in use - stale/deleted record, leave as default
        }

        if (!fixupOk) {
            ++fixupMismatchCount;
        }

        FlatEntry entry;
        bool haveName = ExtractRecordInfo(recordBuf, *header, entry);

        // baseFileRecord != 0 marks this as an *extension* record - overflow
        // storage for another record's attributes (most often extra $DATA
        // data-runs for a heavily fragmented file), reached via that base
        // record's own $ATTRIBUTE_LIST. Set unconditionally from the header
        // field itself, independent of whether a $FILE_NAME was found, so
        // Step 3 can skip these outright rather than misfiling them as
        // orphans - see BuildFolderTree.
        entry.isExtensionRecord = (header->baseFileRecord & 0x0000FFFFFFFFFFFFULL) != 0;

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
            // Extension records never carry their own $FILE_NAME by design -
            // only the base record does - so that bucket is expected, not a
            // parsing failure, and is very plausibly the majority of this
            // bucket on a volume with large fragmented files (ISOs, VM
            // images, media). A record with baseFileRecord == 0 IS its own
            // base record and genuinely should have had a $FILE_NAME
            // somewhere in its own attribute list - that bucket is the one
            // actually worth investigating further, so its record numbers
            // are collected for the caller to report/inspect, and both its
            // fixup status and $ATTRIBUTE_LIST presence are tracked
            // separately to test which explanation (torn read from
            // live-volume churn, vs. a relocated $FILE_NAME this program
            // doesn't follow yet) actually accounts for it.
            if (entry.isExtensionRecord) {
                ++extensionRecordCount;
            }
            else {
                ++unresolvedBaseRecordCount;
                outUnresolvedBaseRecordNumbers.push_back(static_cast<uint32_t>(recordNumber));
                if (!fixupOk) {
                    ++unresolvedBaseRecordFixupMismatchCount;
                }
                if (entry.hasAttributeList) {
                    ++unresolvedBaseRecordAttributeListCount;
                }
            }
            continue; // nothing to show in the progress line
        }

        //  this took 30 seconds to read 1,664,256 MFT records.
        //  All the rest of this program took less than one second.
        ++decodedCount;
        if (decodedCount == 1 || decodedCount % PROGRESS_INTERVAL == 0) {
            // Built with plain concatenation rather than swprintf/wprintf -
            // see WriteWideLine's own comment on wide-stdio formatting
            // being unreliable across MinGW runtime flavors; 
            // string concatenation sidesteps that class of bug entirely.
            // std::wstring line = L"  [" + std::to_wstring(decodedCount) + L"] " +
            //     flatEntries[recordNumber].name + L"\n";
            std::wstring line = L"\r[" + std::to_wstring(decodedCount) + L"]";
            WriteWideLine(line);
        }
    }
    std::wstring endline = L"\n";
    WriteWideLine(endline);

    if (extensionRecordCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(extensionRecordCount) +
            L" extension record(s) - overflow attribute storage, no $FILE_NAME expected)\n";
        WriteWideLine(line);
    }
    if (unresolvedBaseRecordCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(unresolvedBaseRecordCount) +
            L" base record(s) in use with NO $FILE_NAME found - unexpected, worth a closer look)\n";
        WriteWideLine(line);

        // Fixup-mismatch overlap: a high fraction here supports "torn
        // read from live-volume churn during the scan" as the cause; a
        // low fraction means something else is going on and this bucket
        // is worth investigating further. See the "Live-volume
        // consistency" addendum in mft_reader_datarun_design.md.
        std::wstring fixupLine = L"    of those, " + std::to_wstring(unresolvedBaseRecordFixupMismatchCount) +
            L" of " + std::to_wstring(unresolvedBaseRecordCount) +
            L" also had a fixup (update-sequence) checksum mismatch\n";
        WriteWideLine(fixupLine);

        // $ATTRIBUTE_LIST overlap: a high fraction here supports "this
        // record's own $FILE_NAME was relocated to an extension record
        // (most commonly from heavy hard-linking) and this program
        // doesn't follow $ATTRIBUTE_LIST to find it yet" as the cause.
        // See the "Live-volume consistency" addendum in
        // mft_reader_datarun_design.md.
        std::wstring attrListLine = L"    of those, " + std::to_wstring(unresolvedBaseRecordAttributeListCount) +
            L" of " + std::to_wstring(unresolvedBaseRecordCount) +
            L" have an $ATTRIBUTE_LIST attribute (possible relocated $FILE_NAME)\n";
        WriteWideLine(attrListLine);

        // Per-record-number listing retired - the fixup and $ATTRIBUTE_LIST
        // overlap ratios above now explain this bucket (heavy hard-linking
        // relocating $FILE_NAME to an extension record this program
        // doesn't follow - see the "Live-volume consistency" addendum in
        // mft_reader_datarun_design.md), so enumerating every record
        // number is no longer worth the output size. outUnresolvedBase-
        // RecordNumbers is still collected by BuildFlatEntryList and
        // available here if a specific record ever needs inspecting again.
        // std::wstring recordListLine = L"    record number(s): ";
        // for (size_t i = 0; i < outUnresolvedBaseRecordNumbers.size(); ++i) {
        //     if (i > 0) {
        //         recordListLine += L", ";
        //     }
        //     recordListLine += std::to_wstring(outUnresolvedBaseRecordNumbers[i]);
        // }
        // recordListLine += L"\n";
        // WriteWideLine(recordListLine);
    }
    if (fixupMismatchCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(fixupMismatchCount) +
            L" record(s) overall had a fixup checksum mismatch - a torn/mid-write read, not "
            L"necessarily fatal to that record's own data)\n";
        WriteWideLine(line);
    }
    if (ioFailureCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(ioFailureCount) + L" record(s) - seek/read I/O failure)\n";
        WriteWideLine(line);
    }
    if (freeSlotCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(freeSlotCount) +
            L" record(s) - free/never-used MFT slot (all-zero signature), normal)\n";
        WriteWideLine(line);
    }
    if (corruptRecordCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(corruptRecordCount) +
            L" record(s) - NTFS self-flagged corrupt (\"BAAD\" signature), normal)\n";
        WriteWideLine(line);
    }
    if (unexpectedDataCount > 0) {
        std::wstring line = L"  (" + std::to_wstring(unexpectedDataCount) +
            L" record(s) - UNRECOGNIZED signature (not \"FILE\"/\"BAAD\"/zero); first at record " +
            std::to_wstring(firstUnexpectedDataRecord) +
            L" - likely reading past the $MFT's first extent; data-run decoding not yet implemented)\n";
        WriteWideLine(line);
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
// Extension records (isExtensionRecord) are skipped outright in both
// stages, not linked and not orphaned. They're overflow attribute
// storage for another record, never a node in the directory tree in
// their own right - without this, every one of them fails to resolve
// a parent (their parentRecordNumber is left at its default, since
// ExtractRecordInfo never found a $FILE_NAME to read one from) and
// gets miscounted as an orphan, even though nothing is actually wrong.
//
// Root handling: record ROOT_RECORD_NUMBER (5) is its own parent on
// disk, so it's recognized here and never attached under any parent -
// its FolderNode slot is still created normally, just never linked in.
//
// Stale-parent-reference check: even once a parent slot resolves, its
// sequence number is cross-checked against what this entry's own
// $FILE_NAME expected (see FlatEntry::parentSequenceNumber). A
// mismatch means the parent record number has been reused for a
// different file/folder since this entry's $FILE_NAME was read - the
// entry is routed to staleParentRecordNumbers rather than linked under
// an unrelated folder. See the "Live-volume consistency" addendum in
// mft_reader_datarun_design.md.
// ---------------------------------------------------------------------
static FolderTree BuildFolderTree(const std::vector<FlatEntry>& flatEntries)
{
    FolderTree tree;
    const auto totalRecordCount = static_cast<uint32_t>(flatEntries.size());

    // --- Setup: count folders, then allocate + populate folderNodes/folderIndexOf together ---
    uint32_t totalFolderCount = 0;
    for (uint32_t i = 0; i < totalRecordCount; ++i) {
        if (flatEntries[i].inUse && flatEntries[i].isDirectory && !flatEntries[i].isExtensionRecord) {
            ++totalFolderCount;
        }
    }

    tree.folderNodes.resize(totalFolderCount);
    tree.folderIndexOf.assign(totalRecordCount, FOLDER_INDEX_SENTINEL);

    uint32_t nextFolderSlot = 0;
    for (uint32_t i = 0; i < totalRecordCount; ++i) {
        if (flatEntries[i].inUse && flatEntries[i].isDirectory && !flatEntries[i].isExtensionRecord) {
            tree.folderNodes[nextFolderSlot].flatEntryIndex = i;
            tree.folderIndexOf[i] = nextFolderSlot;
            if (i == ROOT_RECORD_NUMBER) {
                tree.rootFolderSlot = nextFolderSlot;
            }
            ++nextFolderSlot;
        }
    }

    // --- Pass 2: link every populated entry (except the root) to its parent ---
    // uint64_t linkedCount = 0;
    for (uint32_t i = 0; i < totalRecordCount; ++i) {
        if (!flatEntries[i].inUse) {
            continue;
        }
        if (flatEntries[i].isExtensionRecord) {
            ++tree.skippedExtensionRecordCount;
            continue; // never a tree node - see comment above
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

        // Sequence-number cross-check: the parent record we're about to
        // link under must still be the same "generation" of record this
        // entry's $FILE_NAME pointed at when we read it. NTFS can reuse a
        // record number for a completely unrelated file/folder if the
        // original was deleted mid-scan - flatEntries[parentRecordNumber].
        // sequenceNumber is that parent's ACTUAL on-disk sequence number
        // as of when we read it; flatEntries[i].parentSequenceNumber is
        // what this entry's own $FILE_NAME expected it to be. A mismatch
        // means the parent slot has been reused since - the relationship
        // our snapshot implies may no longer hold, so it isn't linked.
        // See the "Live-volume consistency" addendum in
        // mft_reader_datarun_design.md.
        uint32_t parentRecordNumber = flatEntries[i].parentRecordNumber;
        if (flatEntries[parentRecordNumber].sequenceNumber != flatEntries[i].parentSequenceNumber) {
            tree.staleParentRecordNumbers.push_back(i);
            continue;
        }

        if (flatEntries[i].isDirectory) {
            tree.folderNodes[parentSlot].subdirs.push_back(i);
        }
        else {
            tree.folderNodes[parentSlot].files.push_back(i);
        }

        // ++linkedCount;
        // if (linkedCount == 1 || linkedCount % PROGRESS_INTERVAL == 0) {
        //     std::wstring line = L"  [link " + std::to_wstring(linkedCount) + L"] " +
        //         flatEntries[i].name + L"\n";
        //     WriteWideLine(line);
        // }
    }

    return tree;
}

// ---------------------------------------------------------------------
// Looks up a FlatEntry by record number. Trivial today - a FlatEntry's
// index in the vector already *is* the record number (see the
// FlatEntry/FolderNode summary comment near their definitions) - but
// wrapped in a named function so callers that just want "the name/size
// for this record number" (printing a folder's children below, or a
// later real lookup against a specific record) don't need to know or
// depend on that indexing detail directly; this is the seam to change
// if that ever stops being true. Bounds-checks defensively rather than
// assuming recordNumber is always valid, since callers may be passing
// in a number that came from elsewhere (a folder's subdirs/files
// vector, a future user-supplied record number, etc.), not just a loop
// already bounded by flatEntries.size().
// ---------------------------------------------------------------------
static const FlatEntry* LookupEntry(const std::vector<FlatEntry>& flatEntries, uint32_t recordNumber)
{
    if (recordNumber >= flatEntries.size()) {
        return nullptr;
    }
    return &flatEntries[recordNumber];
}

// ---------------------------------------------------------------------
// Prints a folder's direct children by name via LookupEntry - subfolders
// first, then files with their size - sorted case-insensitively within
// each group so the output lines up for easy comparison against a
// normal alphabetized directory listing (e.g. ndir). This is a
// diagnostic convenience, not part of the Phase 3 build itself - Step 3
// already noted that sort/display order is an output-time concern, not
// a build-time one (see mft_reader_phase3_final.md, "What Step 3
// deliberately does *not* cover").
// ---------------------------------------------------------------------
static void PrintFolderChildren(const FolderNode& folder, const std::vector<FlatEntry>& flatEntries)  // NOLINT(clang-diagnostic-unused-function)
{
    auto caseInsensitiveLess = [](const std::wstring& a, const std::wstring& b) {
        size_t len = (a.size() < b.size()) ? a.size() : b.size();
        for (size_t i = 0; i < len; ++i) {
            // wchar_t ca = static_cast<wchar_t>(towlower(a[i]));
            // wchar_t cb = static_cast<wchar_t>(towlower(b[i]));
            auto ca = static_cast<wchar_t>(towlower(a[i]));
            auto cb = static_cast<wchar_t>(towlower(b[i]));
            if (ca != cb) {
                return ca < cb;
            }
        }
        return a.size() < b.size();
    };

    auto byName = [&](uint32_t a, uint32_t b) {
        const FlatEntry* ea = LookupEntry(flatEntries, a);
        const FlatEntry* eb = LookupEntry(flatEntries, b);
        return (ea && eb) ? caseInsensitiveLess(ea->name, eb->name) : false;
    };

    std::vector<uint32_t> sortedSubdirs = folder.subdirs;
    std::sort(sortedSubdirs.begin(), sortedSubdirs.end(), byName);

    std::vector<uint32_t> sortedFiles = folder.files;
    std::sort(sortedFiles.begin(), sortedFiles.end(), byName);

    for (uint32_t recordNumber : sortedSubdirs) {
        const FlatEntry* entry = LookupEntry(flatEntries, recordNumber);
        if (!entry) {
            continue; // shouldn't happen - record number came from this same flatEntries vector
        }
        std::wstring line = L"  [DIR]  " + entry->name + L"  (record " + std::to_wstring(recordNumber) + L")\n";
        WriteWideLine(line);
    }
    for (uint32_t recordNumber : sortedFiles) {
        const FlatEntry* entry = LookupEntry(flatEntries, recordNumber);
        if (!entry) {
            continue;
        }
        std::wstring line = L"  " + std::to_wstring(entry->fileSize) + L"  " + entry->name +
            L"  (record " + std::to_wstring(recordNumber) + L")\n";
        WriteWideLine(line);
    }
}

// ---------------------------------------------------------------------
// Opens a raw handle to the volume containing the given drive letter.
// Requires the process to be running elevated.
// ---------------------------------------------------------------------
static HANDLE OpenVolumeRaw(char driveLetter)
{
    char volumePath[16];
    sprintf(volumePath, "\\\\.\\%c:", driveLetter);   // NOLINT(modernize-raw-string-literal)

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
// Prints command-line usage/help text. Called when no argument is
// given at all, or when the given argument isn't a recognizable drive
// spec - see ResolveDriveLetter.
// ---------------------------------------------------------------------
static void PrintUsage(const wchar_t* programName)
{
    dputsf(L"Usage: %s <drive>:\n", programName);
    dputsf(L"\n");
    dputsf(L"Reads the NTFS Master File Table (MFT) directly from the given volume,\n");
    dputsf(L"bypassing the normal Win32 file enumeration APIs (FindFirstFile/\n");
    dputsf(L"FindNextFile), in the style of tools like WizTree.\n");
    dputsf(L"\n");
    dputsf(L"  <drive>:   Drive letter of the NTFS volume to scan, e.g. D:\n");
    dputsf(L"\n");
    dputsf(L"This program must be run from an elevated (Administrator) command\n");
    dputsf(L"prompt - raw volume access requires elevated privileges.\n");
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
static char ResolveDriveLetter(int argc, wchar_t* argv[])
{
    if (argc > 1 && argv[1][0] != '\0' && argv[1][1] == ':') {
        return static_cast<char>(toupper(static_cast<unsigned char>(argv[1][0])));
    }

    return '\0'; // missing or malformed - caller shows usage
}

//********************************************************************************
//  this solution is from:
//  https://github.com/coderforlife/mingw-unicode-main/
//********************************************************************************
#ifndef  USE_SO_METHOD
#if defined(__GNUC__) && defined(_UNICODE)

#ifndef __MSVCRT__
#error Unicode main function requires linking to MSVCRT
#endif

#include <wchar.h>
#include <stdlib.h>

extern int _CRT_glob;
extern 
#ifdef __cplusplus
"C" 
#endif
void __wgetmainargs(int*,wchar_t***,wchar_t***,int,int*);   // NOLINT

#ifdef MAIN_USE_ENVP
int wmain(int argc, wchar_t *argv[], wchar_t *envp[]);
#else
int wmain(int argc, wchar_t *argv[]);
#endif

int main() 
{
   wchar_t **enpv, **argv;
   int argc, si = 0;
   __wgetmainargs(&argc, &argv, &enpv, _CRT_glob, &si); // this also creates the global variable __wargv
#ifdef MAIN_USE_ENVP
   return wmain(argc, argv, enpv);
#else
   return wmain(argc, argv);
#endif
}

#endif //defined(__GNUC__) && defined(_UNICODE)
#endif   // #ifndef  USE_SO_METHOD

// ---------------------------------------------------------------------
#ifdef  USE_SO_METHOD
int main(void) 
#else
// int main(int argc, char* argv[]) // NOLINT(bugprone-exception-escape)
int wmain(int argc, wchar_t *argv[]) // NOLINT(bugprone-exception-escape)
#endif
{
    stdoutIsConsole = IsStdoutConsole(GetStdHandle(STD_OUTPUT_HANDLE));
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    char driveLetter = ResolveDriveLetter(argc, argv);
    if (driveLetter == '\0') {
        dputsf(L"Invalid drive argument: '%s'\n\n", argv[1]);
        PrintUsage(argv[0]);
        return 1;
    }

    if (!IsProcessElevated()) {
        dputsf(L"This program requires Administrator privileges to read raw NTFS volumes.\n");
        dputsf(L"Please re-run from an elevated (Administrator) Command Prompt.\n");
        return 1;
    }

    dputsf(L"Target volume: %c:\n", driveLetter);

    HANDLE hVolume = OpenVolumeRaw(driveLetter);
    if (hVolume == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        dputsf(L"Failed to open volume (error %lu).\n", static_cast<unsigned long>(err));
        dputsf(L"This program must be run as Administrator.\n");
        return 1;
    }

    // Read the boot sector (always the first 512 bytes of the volume).
    NTFS_BOOT_SECTOR boot{};
    DWORD bytesRead = 0;
    if (!ReadFile(hVolume, &boot, sizeof(boot), &bytesRead, nullptr) || bytesRead != sizeof(boot)) {
        dputsf(L"Failed to read boot sector.\n");
        CloseHandle(hVolume);
        return 1;
    }

    if (memcmp(boot.oemId, "NTFS    ", 8) != 0) {
        dputsf(L"Volume %c: does not appear to be NTFS.\n", driveLetter);
        CloseHandle(hVolume);
        return 1;
    }

    uint32_t clusterSize = static_cast<uint32_t>(boot.bytesPerSector) * boot.sectorsPerCluster;
    uint32_t mftRecordSize = ComputeMftRecordSize(boot);
    uint64_t mftOffsetBytes = boot.mftStartCluster * clusterSize;

    dputsf(L"\n-- NTFS boot sector --\n");
    dputsf(L"Bytes per sector:      %u\n", boot.bytesPerSector);
    dputsf(L"Sectors per cluster:   %u\n", boot.sectorsPerCluster);
    dputsf(L"Cluster size:          %u bytes\n", clusterSize);
    dputsf(L"Total sectors:         %llu\n", static_cast<unsigned long long>(boot.totalSectors));
    dputsf(L"MFT start cluster:     %llu\n", static_cast<unsigned long long>(boot.mftStartCluster));
    dputsf(L"MFT byte offset:       %llu\n", static_cast<unsigned long long>(mftOffsetBytes));
    dputsf(L"MFT record size:       %u bytes\n", mftRecordSize);
    dputsf(L"Volume serial number:  0x%llX\n", static_cast<unsigned long long>(boot.volumeSerialNumber));

    // Seek to the start of the MFT and read its very first record (record 0,
    // which describes the $MFT file itself).
    LARGE_INTEGER seekPos;
    seekPos.QuadPart = static_cast<LONGLONG>(mftOffsetBytes);
    if (!SetFilePointerEx(hVolume, seekPos, nullptr, FILE_BEGIN)) {
        dputsf(L"Failed to seek to MFT.\n");
        CloseHandle(hVolume);
        return 1;
    }

    std::string recordBuf(mftRecordSize, '\0');
    if (!ReadFile(hVolume, &recordBuf[0], mftRecordSize, &bytesRead, nullptr) || bytesRead != mftRecordSize) {
        dputsf(L"Failed to read MFT record 0.\n");
        CloseHandle(hVolume);
        return 1;
    }

    // const MFT_RECORD_HEADER* header = reinterpret_cast<const MFT_RECORD_HEADER*>(recordBuf.data());
    const auto* header = reinterpret_cast<const MFT_RECORD_HEADER*>(recordBuf.data());

    dputsf(L"\n-- MFT record 0 header ($MFT itself) --\n");
    //  convert header signature from uint8_t array to wchar_t array
    wchar_t wsig[5];
    //std::copy(&header->signature[0], &header->signature[4], wsig);
    for (int i = 0; i < 4; ++i) {
        wsig[i] = static_cast<wchar_t>(header->signature[i]);
    }    
    wsig[4] = L'\0';
    dputsf(L"Signature:             %s\n", wsig);
    dputsf(L"Sequence number:       %u\n", header->sequenceNumber);
    dputsf(L"Hard link count:       %u\n", header->hardLinkCount);
    dputsf(L"First attribute offset:%u\n", header->firstAttributeOffset);
    dputsf(L"Flags:                 0x%04X (%s)\n",
        header->flags,
        (header->flags & 0x0001) ? L"in use" : L"not in use");
    dputsf(L"Real size:             %u bytes\n", header->realSize);
    dputsf(L"Allocated size:        %u bytes\n", header->allocatedSize);

    if (memcmp(header->signature, "FILE", 4) != 0) {
        dputsf(L"\nUnexpected signature - record may be corrupt or offsets are wrong.\n");
        CloseHandle(hVolume);
        return 1;
    }

    // Undo the update-sequence fixup before reading any attribute content -
    // see ApplyFixup() comment. Re-point `header` at the now-corrected
    // buffer (ApplyFixup only touches sector-tail bytes, not the header
    // fields themselves, but recordBuf's storage didn't move, so the
    // existing `header` pointer stays valid either way).
    if (!ApplyFixup(recordBuf, *header)) {
        dputsf(L"\nWarning: update sequence check value mismatch - record may be corrupt.\n");
    }

    WalkAttributes(recordBuf, *header);

    dputsf(L"\nPhase 1 OK: raw MFT read pipeline confirmed working.\n");

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
    ATTR_NONRESIDENT_HEADER mftDataHeader {};
    if (!FindMftDataAttribute(recordBuf, *header, dataAttrOffset, dataAttrLength, mftDataHeader)) {
        dputsf(L"\nCould not locate $MFT's own non-resident $DATA attribute - cannot decode data runs.\n");
        CloseHandle(hVolume);
        return 1;
    }

    std::vector<DataRunExtent> mftExtents;
    bool runsDecodedOk = DecodeDataRuns(recordBuf, dataAttrOffset, dataAttrLength, mftDataHeader, mftExtents);

    dputsf(L"\n-- $MFT data-run decode (validation only) --\n");
    if (!runsDecodedOk) {
        dputsf(L"Data-run decode FAILED (malformed run list or unsupported sparse run).\n");
        CloseHandle(hVolume);
        return 1;
    }

    uint64_t totalCoveredClusters = 0;
    for (const DataRunExtent& extent : mftExtents) {
        totalCoveredClusters += extent.clusterCount;
    }
    uint64_t coveredBytes = totalCoveredClusters * clusterSize;
    bool sizesMatch = (coveredBytes == mftDataHeader.allocatedSize);

    dputsf(L"Extents decoded:        %zu\n", mftExtents.size());
    dputsf(L"Total covered clusters: %llu\n", static_cast<unsigned long long>(totalCoveredClusters));
    dputsf(L"Covered bytes:          %llu\n", static_cast<unsigned long long>(coveredBytes));
    dputsf(L"$DATA allocatedSize:    %llu\n", static_cast<unsigned long long>(mftDataHeader.allocatedSize));
    dputsf(L"Match:                   %s\n", sizesMatch ? L"YES" : L"NO - MISMATCH");

    size_t showCount = mftExtents.size() < 10 ? mftExtents.size() : 10;
    dputsf(L"\nFirst %zu extent(s):\n", showCount);
    for (size_t i = 0; i < showCount; ++i) {
        dputsf(L"  [%zu] VCN %llu, %llu cluster(s), LCN %llu\n", i,
            static_cast<unsigned long long>(mftExtents[i].startVcn),
            static_cast<unsigned long long>(mftExtents[i].clusterCount),
            static_cast<unsigned long long>(mftExtents[i].startLcn));
    }
    if (mftExtents.size() > showCount) {
        const DataRunExtent& last = mftExtents.back();
        dputsf(L"  ... (%zu more) ...\n", mftExtents.size() - showCount - 1);
        dputsf(L"  [last] VCN %llu, %llu cluster(s), LCN %llu\n",
            static_cast<unsigned long long>(last.startVcn),
            static_cast<unsigned long long>(last.clusterCount),
            static_cast<unsigned long long>(last.startLcn));
    }

    dputsf(L"\nData-run decode validation %s.\n", sizesMatch ? L"PASSED" : L"FAILED");
    if (!sizesMatch) {
        dputsf(L"Refusing to proceed to Phase 3 with an unverified extent map.\n");
        CloseHandle(hVolume);
        return 1;
    }

    // --- Phase 3: full-MFT flat pass, then folder-tree build ---
    uint64_t mftFileSize = GetMftFileSize(recordBuf, *header);
    if (mftFileSize == 0) {
        dputsf(L"\nCould not determine $MFT file size from record 0 - skipping Phase 3.\n");
        CloseHandle(hVolume);
        return 1;
    }
    uint64_t totalRecordCount = mftFileSize / mftRecordSize;

    dputsf(L"\n-- Phase 3: Step 1 - flat entry list --\n");
    dputsf(L"Total MFT records to scan: %llu\n", static_cast<unsigned long long>(totalRecordCount));
    std::vector<uint32_t> unresolvedBaseRecordNumbers;
    std::vector<FlatEntry> flatEntries = BuildFlatEntryList(hVolume, mftExtents, clusterSize, mftRecordSize,
        totalRecordCount, unresolvedBaseRecordNumbers);

    CloseHandle(hVolume); // done with the volume - everything else works from flatEntries in memory

    uint64_t decodedTotal = 0;
    for (const FlatEntry& entry : flatEntries) {
        if (entry.inUse) {
            ++decodedTotal;
        }
    }
    dputsf(L"Decoded %llu in-use entries out of %llu records scanned.\n",
        static_cast<unsigned long long>(decodedTotal), static_cast<unsigned long long>(totalRecordCount));

    dputsf(L"\n-- Phase 3: Step 3 - folder tree build --\n");
    FolderTree tree = BuildFolderTree(flatEntries);

    dputsf(L"Folders found:        %zu\n", tree.folderNodes.size());
    dputsf(L"System records:       %zu\n", tree.systemRecordNumbers.size());
    dputsf(L"Orphaned entries:     %zu\n", tree.orphanedRecordNumbers.size());
    dputsf(L"Stale parent references (parent record reused): %zu\n", tree.staleParentRecordNumbers.size());
    dputsf(L"Extension records skipped (not tree nodes): %llu\n",
        static_cast<unsigned long long>(tree.skippedExtensionRecordCount));
    if (tree.rootFolderSlot != FOLDER_INDEX_SENTINEL) {
        const FolderNode& root = tree.folderNodes[tree.rootFolderSlot];
        dputsf(L"Root: %zu direct subfolders, %zu direct files\n", root.subdirs.size(), root.files.size());
        // Root-children name dump - served its purpose confirming the
        // metadata-file/root-count hypothesis, no longer needed each run.
        // PrintFolderChildren itself is left in place (unused) as
        // reference code for later real lookups - see its own comment.
        // dputsf(L"\n-- Root's direct children --\n");
        PrintFolderChildren(root, flatEntries);
    }
    else {
        dputsf(L"Warning: root record (%u) was not found as a folder.\n", ROOT_RECORD_NUMBER);
    }

    dputsf(L"\nPhase 3 OK: flat entry list and folder tree built.\n");
    return 0;
}
