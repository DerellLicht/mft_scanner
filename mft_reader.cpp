// mft_reader.cpp
//
// Template program (Phase 1) for direct NTFS MFT reading, in the style of
// tools like WizTree. Instead of walking the filesystem with
// FindFirstFile/FindNextFile, this opens the raw NTFS volume, parses the
// boot sector to locate the $MFT, and reads the first MFT record ($MFT's
// own record, record #0) to confirm the raw-read pipeline works end to end.
//
// Phase 1 scope only:
//   - Resolve a drive letter from an optional command-line path argument
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

static const uint32_t ATTR_TYPE_END = 0xFFFFFFFF;

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
// Resolves a drive letter (e.g. 'C') from a path given on the command
// line. Falls back to the current working directory's drive if no
// argument was supplied. Only handles the simple "X:\..." / "X:" forms,
// which is sufficient for Phase 1.
// ---------------------------------------------------------------------
static char ResolveDriveLetter(int argc, char* argv[])
{
    if (argc > 1 && argv[1][0] != '\0' && argv[1][1] == ':') {
        return static_cast<char>(toupper(static_cast<unsigned char>(argv[1][0])));
    }

    char cwd[MAX_PATH] = { 0 };
    GetCurrentDirectoryA(MAX_PATH, cwd);
    return static_cast<char>(toupper(static_cast<unsigned char>(cwd[0])));
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
int main(int argc, char* argv[])
{
    if (!IsProcessElevated()) {
        printf("This program requires Administrator privileges to read raw NTFS volumes.\n");
        printf("Please re-run from an elevated (Administrator) Command Prompt.\n");
        return 1;
    }

    char driveLetter = ResolveDriveLetter(argc, argv);
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

    CloseHandle(hVolume);

    if (memcmp(header->signature, "FILE", 4) != 0) {
        printf("\nUnexpected signature - record may be corrupt or offsets are wrong.\n");
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
    return 0;
}
