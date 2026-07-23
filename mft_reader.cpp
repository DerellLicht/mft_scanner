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
//   - Parsing $FILE_NAME / $DATA attributes
//   - Following non-resident attribute data runs
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
// Resolves a drive letter (e.g. 'C') from a path given on the command
// line. Falls back to the current working directory's drive if no
// argument was supplied. Only handles the simple "X:\..." / "X:" forms,
// which is sufficient for Phase 1.
// ---------------------------------------------------------------------
static char ResolveDriveLetter(int argc, char* argv[])
{
    if (argc > 1 && argv[1][0] != '\0' && argv[1][1] == ':')
    {
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

    if (boot.clustersPerMftRecord >= 0)
    {
        return clusterSize * static_cast<uint32_t>(boot.clustersPerMftRecord);
    }
    else
    {
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
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        return false;
    }

    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size))
    {
        elevated = (elevation.TokenIsElevated != 0);
    }

    CloseHandle(hToken);
    return elevated;
}

int main(int argc, char* argv[])
{
    if (!IsProcessElevated())
    {
        printf("This program requires Administrator privileges to read raw NTFS volumes.\n");
        printf("Please re-run from an elevated (Administrator) Command Prompt.\n");
        return 1;
    }

    char driveLetter = ResolveDriveLetter(argc, argv);
    printf("Target volume: %c:\n", driveLetter);

    HANDLE hVolume = OpenVolumeRaw(driveLetter);
    if (hVolume == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        fprintf(stderr, "Failed to open volume (error %lu).\n", static_cast<unsigned long>(err));
        fprintf(stderr, "This program must be run as Administrator.\n");
        return 1;
    }

    // Read the boot sector (always the first 512 bytes of the volume).
    NTFS_BOOT_SECTOR boot;
    DWORD bytesRead = 0;
    if (!ReadFile(hVolume, &boot, sizeof(boot), &bytesRead, nullptr) || bytesRead != sizeof(boot))
    {
        fprintf(stderr, "Failed to read boot sector.\n");
        CloseHandle(hVolume);
        return 1;
    }

    if (memcmp(boot.oemId, "NTFS    ", 8) != 0)
    {
        fprintf(stderr, "Volume %c: does not appear to be NTFS.\n", driveLetter);
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
    if (!SetFilePointerEx(hVolume, seekPos, nullptr, FILE_BEGIN))
    {
        fprintf(stderr, "Failed to seek to MFT.\n");
        CloseHandle(hVolume);
        return 1;
    }

    std::string recordBuf(mftRecordSize, '\0');
    if (!ReadFile(hVolume, &recordBuf[0], mftRecordSize, &bytesRead, nullptr) || bytesRead != mftRecordSize)
    {
        fprintf(stderr, "Failed to read MFT record 0.\n");
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

    if (memcmp(header->signature, "FILE", 4) != 0)
    {
        fprintf(stderr, "\nUnexpected signature - record may be corrupt or offsets are wrong.\n");
        return 1;
    }

    printf("\nPhase 1 OK: raw MFT read pipeline confirmed working.\n");
    return 0;
}
